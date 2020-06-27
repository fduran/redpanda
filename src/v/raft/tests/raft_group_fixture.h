#pragma once
#include "model/metadata.h"
#include "model/record_batch_reader.h"
#include "raft/consensus.h"
#include "raft/consensus_client_protocol.h"
#include "raft/heartbeat_manager.h"
#include "raft/rpc_client_protocol.h"
#include "raft/service.h"
#include "random/generators.h"
#include "rpc/backoff_policy.h"
#include "rpc/connection_cache.h"
#include "rpc/server.h"
#include "rpc/simple_protocol.h"
#include "rpc/types.h"
#include "storage/log_manager.h"
#include "storage/tests/utils/random_batch.h"
#include "test_utils/fixture.h"

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/noncopyable_function.hh>

#include <boost/range/iterator_range_core.hpp>
#include <fmt/core.h>

#include <chrono>

inline static ss::logger tstlog("raft_test");

using namespace std::chrono_literals; // NOLINT

inline static auto heartbeat_interval = 40ms;

inline static const raft::replicate_options
  default_replicate_opts(raft::consistency_level::quorum_ack);

using consensus_ptr = ss::lw_shared_ptr<raft::consensus>;
struct test_raft_manager {
    consensus_ptr consensus_for(raft::group_id) { return c; };
    consensus_ptr c = nullptr;
};

struct consume_to_vector {
    ss::future<ss::stop_iteration> operator()(model::record_batch b) {
        batches.push_back(std::move(b));
        return ss::make_ready_future<ss::stop_iteration>(
          ss::stop_iteration::no);
    }

    std::vector<model::record_batch> end_of_stream() {
        return std::move(batches);
    }

    std::vector<model::record_batch> batches;
};

struct raft_node {
    using log_t = std::vector<model::record_batch>;
    using leader_clb_t
      = ss::noncopyable_function<void(raft::leadership_status)>;

    raft_node(
      model::ntp ntp,
      model::broker broker,
      raft::group_id gr_id,
      raft::group_configuration cfg,
      raft::timeout_jitter jit,
      ss::sstring storage_dir,
      storage::log_config::storage_type storage_type,
      leader_clb_t l_clb)
      : broker(std::move(broker))
      , leader_callback(std::move(l_clb)) {
        cache.start().get();

        storage
          .start(
            storage::kvstore_config(
              1_MiB, 10ms, storage_dir, storage::debug_sanitize_files::yes),
            storage::log_config(
              storage_type,
              storage_dir,
              100_MiB,
              storage::debug_sanitize_files::yes))
          .get();
        storage.invoke_on_all(&storage::api::start).get();

        log = std::make_unique<storage::log>(
          storage.local()
            .log_mgr()
            .manage(storage::ntp_config(
              ntp, storage.local().log_mgr().config().base_dir))
            .get0());

        // setup consensus
        consensus = ss::make_lw_shared<raft::consensus>(
          broker.id(),
          gr_id,
          std::move(cfg),
          std::move(jit),
          *log,
          seastar::default_priority_class(),
          std::chrono::seconds(10),
          raft::make_rpc_client_protocol(cache),
          [this](raft::leadership_status st) { leader_callback(st); },
          storage.local());

        // create connections to initial nodes
        consensus->config().for_each([this](const model::broker& broker) {
            create_connection_to(broker);
        });
    }

    raft_node(const raft_node&) = delete;
    raft_node& operator=(const raft_node&) = delete;
    raft_node(raft_node&&) noexcept = delete;
    raft_node& operator=(raft_node&&) noexcept = delete;

    void start() {
        tstlog.info("Starting node {} stack ", id());
        // start rpc
        server
          .start(rpc::server_configuration{
            .addrs = {broker.rpc_address().resolve().get0()},
            .max_service_memory_per_core = 1024 * 1024 * 1024,
            .credentials = std::nullopt,
            .disable_metrics = rpc::metrics_disabled::yes,
          })
          .get0();
        raft_manager.start().get0();
        raft_manager
          .invoke_on(0, [this](test_raft_manager& mgr) { mgr.c = consensus; })
          .get0();
        server
          .invoke_on_all([this](rpc::server& s) {
              auto proto = std::make_unique<rpc::simple_protocol>();
              proto
                ->register_service<raft::service<test_raft_manager, raft_node>>(
                  ss::default_scheduling_group(),
                  ss::default_smp_service_group(),
                  raft_manager,
                  *this);
              s.set_protocol(std::move(proto));
          })
          .get0();
        server.invoke_on_all(&rpc::server::start).get0();
        hbeats = std::make_unique<raft::heartbeat_manager>(
          heartbeat_interval,
          raft::make_rpc_client_protocol(cache),
          broker.id());
        hbeats->start().get0();
        consensus->start().get0();
        hbeats->register_group(consensus).get();
        started = true;
    }

    ss::future<> stop_node() {
        if (!started) {
            return ss::make_ready_future<>();
        }

        tstlog.info("Stopping node stack {}", broker.id());
        return server.stop()
          .then([this] {
              if (hbeats) {
                  tstlog.info("Stopping heartbets manager at {}", broker.id());
                  return hbeats->deregister_group(consensus->group())
                    .then([this] { return hbeats->stop(); });
              }
              return ss::make_ready_future<>();
          })
          .then([this] {
              tstlog.info("Stopping raft at {}", broker.id());
              return consensus->stop();
          })
          .then([this] {
              tstlog.info("Raft stopped at node {}", broker.id());
              return raft_manager.stop();
          })
          .then([this] { return cache.stop(); })
          .then([this] { return storage.stop(); })
          .then([this] {
              tstlog.info("Node {} stopped", broker.id());
              started = false;
          });
    }

    ss::shard_id shard_for(raft::group_id) { return ss::shard_id(0); }

    bool contains(raft::group_id) { return true; }

    ss::future<log_t> read_log() {
        auto max_offset = model::offset(consensus->committed_offset());
        storage::log_reader_config cfg(
          model::offset(0), max_offset, ss::default_priority_class());
        return log->make_reader(cfg).then(
          [this, max_offset](model::record_batch_reader rdr) {
              tstlog.debug(
                "Reading logs from {} max offset {}, log offsets {}",
                id(),
                max_offset,
                log->offsets());
              return std::move(rdr).consume(
                consume_to_vector{}, model::no_timeout);
          });
    }

    model::node_id id() { return broker.id(); }

    void create_connection_to(const model::broker& broker) {
        for (ss::shard_id i = 0; i < ss::smp::count; ++i) {
            auto sh = rpc::connection_cache::shard_for(i, broker.id());
            cache
              .invoke_on(
                sh,
                [&broker, this](rpc::connection_cache& c) {
                    if (c.contains(broker.id())) {
                        return seastar::make_ready_future<>();
                    }
                    return broker.rpc_address().resolve().then(
                      [this, &broker, &c](ss::socket_address addr) {
                          return c.emplace(
                            broker.id(),
                            {.server_addr = addr,
                             .disable_metrics = rpc::metrics_disabled::yes},
                            rpc::make_exponential_backoff_policy<
                              rpc::clock_type>(
                              std::chrono::milliseconds(1),
                              std::chrono::milliseconds(1)));
                      });
                })
              .get0();
        }
    }

    bool started = false;
    model::broker broker;
    ss::sharded<storage::api> storage;
    std::unique_ptr<storage::log> log;
    ss::sharded<rpc::connection_cache> cache;
    ss::sharded<rpc::server> server;
    ss::sharded<test_raft_manager> raft_manager;
    std::unique_ptr<raft::heartbeat_manager> hbeats;
    consensus_ptr consensus;
    leader_clb_t leader_callback;
};

model::ntp node_ntp(raft::group_id gr_id, model::node_id n_id) {
    return model::ntp(
      model::ns("test"),
      model::topic(fmt::format("group_{}", gr_id())),
      model::partition_id(n_id()));
}

struct raft_group {
    using members_t = std::unordered_map<model::node_id, raft_node>;
    using logs_t = std::unordered_map<model::node_id, raft_node::log_t>;

    raft_group(
      raft::group_id id,
      int size,
      storage::log_config::storage_type storage_type
      = storage::log_config::storage_type::disk)
      : _id(id)
      , _storage_type(storage_type)
      , _storage_dir("test.raft." + random_generators::gen_alphanum_string(6)) {
        std::vector<model::broker> brokers;
        for (auto i : boost::irange(0, size)) {
            _initial_brokers.push_back(make_broker(model::node_id(i)));
        }
    };

    consensus_ptr member_consensus(model::node_id node) {
        return _members.find(node)->second.consensus;
    }

    model::broker make_broker(model::node_id id) {
        return model::broker(
          model::node_id(id),
          unresolved_address("localhost", 9092),
          unresolved_address("localhost", base_port + id),
          std::nullopt,
          model::broker_properties{
            .cores = 1,
          });
    }

    void enable_node(model::node_id node_id) {
        auto ntp = node_ntp(_id, node_id);
        tstlog.info("Enabling node {} in group {}", node_id, _id);
        auto [it, _] = _members.try_emplace(
          node_id,
          ntp,
          make_broker(node_id),
          _id,
          raft::group_configuration{
            .nodes = _initial_brokers,
          },
          raft::timeout_jitter(heartbeat_interval * 2),
          fmt::format("{}/{}", _storage_dir, node_id()),
          _storage_type,
          [this, node_id](raft::leadership_status st) {
              election_callback(node_id, st);
          });
        it->second.start();
    }

    model::broker create_new_node(model::node_id node_id) {
        auto ntp = node_ntp(_id, node_id);
        auto broker = make_broker(node_id);
        tstlog.info("Enabling node {} in group {}", node_id, _id);
        auto [it, _] = _members.try_emplace(
          node_id,
          ntp,
          broker,
          _id,
          raft::group_configuration{
            .nodes = {},
          },
          raft::timeout_jitter(heartbeat_interval * 2),
          fmt::format("{}/{}", _storage_dir, node_id()),
          _storage_type,
          [this, node_id](raft::leadership_status st) {
              election_callback(node_id, st);
          });
        it->second.start();

        for (auto& [_, n] : _members) {
            n.create_connection_to(broker);
        }

        return broker;
    }

    void disable_node(model::node_id node_id) {
        tstlog.info("Disabling node {} in group {}", node_id, _id);
        _members.find(node_id)->second.stop_node().get0();
        _members.erase(node_id);
    }

    void enable_all() {
        for (auto& br : _initial_brokers) {
            enable_node(br.id());
        }
    }

    std::optional<model::node_id> get_leader_id() {
        std::optional<model::node_id> leader_id{std::nullopt};
        raft::group_id::type leader_term = model::term_id(0);

        for (auto& [id, m] : _members) {
            if (m.consensus->is_leader() && m.consensus->term() > leader_term) {
                leader_id.emplace(id);
            }
        }

        return leader_id;
    }

    void election_callback(model::node_id src, raft::leadership_status st) {
        if (st.current_leader != src) {
            // only accept election callbacks from current leader.
            return;
        }
        tstlog.info(
          "Group {} has new leader {}", st.group, st.current_leader.value());
        _election_sem.signal();
        _elections_count++;
    }

    void wait_for_next_election() {
        ss::thread::yield();
        _election_sem.wait().get0();
    }

    ss::future<std::vector<model::record_batch>>
    read_member_log(model::node_id member) {
        return _members.find(member)->second.read_log();
    }

    logs_t read_all_logs() {
        logs_t logs_map;
        for (auto& [id, m] : _members) {
            logs_map.try_emplace(id, m.read_log().get0());
        }
        return logs_map;
    }

    ~raft_group() {
        std::vector<ss::future<>> close_futures;
        for (auto& [_, m] : _members) {
            close_futures.push_back(m.stop_node());
        }
        ss::when_all(close_futures.begin(), close_futures.end()).get0();
    }

    members_t& get_members() { return _members; }

    raft_node& get_member(model::node_id n) { return _members.find(n)->second; }

    uint32_t get_elections_count() const { return _elections_count; }

private:
    uint16_t base_port = 35000;
    raft::group_id _id;
    members_t _members;
    std::vector<model::broker> _initial_brokers;
    ss::condition_variable _leader_elected;
    ss::semaphore _election_sem{0};
    uint32_t _elections_count{0};
    storage::log_config::storage_type _storage_type;
    ss::sstring _storage_dir;
};

struct raft_test_fixture {
    raft_test_fixture() {
        ss::smp::invoke_on_all([] {
            config::shard_local_cfg().get("disable_metrics").set_value(true);
        }).get();
    }

    model::record_batch_reader random_batches_entry(int max_batches) {
        auto batches = storage::test::make_random_batches(
          model::offset(0), max_batches);
        return model::make_memory_record_batch_reader(std::move(batches));
    }

    template<typename Rep, typename Period, typename Pred>
    void wait_for(
      std::chrono::duration<Rep, Period> timeout, Pred&& p, ss::sstring msg) {
        using clock_t = std::chrono::system_clock;
        auto start = clock_t::now();
        auto res = p();
        while (!res) {
            auto elapsed = clock_t::now() - start;
            if (elapsed > timeout) {
                BOOST_FAIL(
                  fmt::format("Timeout elapsed while wating for: {}", msg));
            }
            res = p();
            ss::sleep(std::chrono::milliseconds(400)).get0();
        }
    }

    void assert_at_most_one_leader(raft_group& gr) {
        std::unordered_map<long, int> leaders_per_term;
        for (auto& [_, m] : gr.get_members()) {
            auto term = static_cast<long>(m.consensus->term());
            if (auto it = leaders_per_term.find(term);
                it == leaders_per_term.end()) {
                leaders_per_term.try_emplace(term, 0);
            }
            auto it = leaders_per_term.find(m.consensus->term());
            it->second += m.consensus->is_leader();
        }
        for (auto& [term, leaders] : leaders_per_term) {
            BOOST_REQUIRE_LE(leaders, 1);
        }
    }

    bool are_logs_the_same_length(const raft_group::logs_t& logs) {
        auto size = logs.begin()->second.size();
        if (size == 0) {
            return false;
        }
        for (auto& [id, l] : logs) {
            tstlog.debug("Node {} log has {} batches", id, l.size());
            if (size != l.size()) {
                return false;
            }
        }
        return true;
    }

    bool are_all_commit_indexes_the_same(raft_group& gr) {
        auto c_idx
          = gr.get_members().begin()->second.consensus->committed_offset();
        for (auto& [id, m] : gr.get_members()) {
            auto current = model::offset(m.consensus->committed_offset());
            auto log_offset = m.log->offsets().dirty_offset;
            tstlog.debug(
              "Node {} commit index {}, log offset {}",
              id,
              current,
              log_offset);
            if (c_idx != current || log_offset != c_idx) {
                return false;
            }
        }
        return true;
    }

    void assert_all_logs_are_the_same(const raft_group::logs_t& logs) {
        auto it = logs.begin();
        auto& reference = logs.begin()->second;
        it++;
        while (it != logs.end()) {
            if (reference.size() != it->second.size()) {
                auto r_size = reference.size() != it->second.size();
                auto it_sz = it->second.size();
                tstlog.error("Different length {}, {}", r_size, it_sz);
            }
            for (int i = 0; i < reference.size(); ++i) {
                BOOST_REQUIRE_EQUAL(reference[i], it->second[i]);
            }
            it++;
        }
    }

    bool are_logs_the_same(const raft_group::logs_t& logs) {
        auto prev = logs.begin()->second.size();
        for (auto& [_, l] : logs) {
            if (prev != l.size()) {
                return false;
            }
        }

        return true;
    }

    void validate_logs_replication(raft_group& gr) {
        auto logs = gr.read_all_logs();
        wait_for(
          10s,
          [this, &gr, &logs] {
              logs = gr.read_all_logs();
              return are_logs_the_same_length(logs);
          },
          "Logs are replicated");

        assert_all_logs_are_the_same(logs);
    }

    model::node_id wait_for_group_leader(raft_group& gr) {
        gr.wait_for_next_election();
        assert_at_most_one_leader(gr);
        auto leader_id = gr.get_leader_id();
        while (!leader_id) {
            assert_at_most_one_leader(gr);
            gr.wait_for_next_election();
            assert_at_most_one_leader(gr);
            leader_id = gr.get_leader_id();
        }

        return leader_id.value();
    }

    void assert_stable_leadership(
      const raft_group& gr, int number_of_intervals = 5) {
        auto before = gr.get_elections_count();
        ss::sleep(heartbeat_interval * number_of_intervals).get0();
        BOOST_TEST(
          before = gr.get_elections_count(),
          "Group leadership is required to be stable");
    }
};
