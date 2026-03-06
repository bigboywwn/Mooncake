#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "client_service.h"
#include "master_client.h"
#include "replica.h"
#include "types.h"
#include "utils.h"

namespace {

using mooncake::Client;
using mooncake::ErrorCode;
using mooncake::MasterClient;
using mooncake::QueryResult;
using mooncake::Replica;
using mooncake::ReplicateConfig;
using mooncake::Slice;
using mooncake::UUID;

DEFINE_string(case, "TC-CFG-01", "Case ID to run");
DEFINE_string(master, "127.0.0.1:50051", "Master server address");
DEFINE_string(metadata, "P2PHANDSHAKE", "Transfer engine metadata URL");
DEFINE_string(protocol, "tcp", "Transfer protocol");
DEFINE_string(device_name, "", "Device name when protocol=rdma");
DEFINE_string(expect_tiers, "", "Expected enabled_tiers csv, e.g. DDR,SSD");
DEFINE_string(expect_impl, "spdk", "Expected nvmeof client impl");
DEFINE_bool(expect_client_init_fail, false,
            "Expect client initialization to fail (for fail-fast case)");
DEFINE_int32(client_port_base, 52000, "Base port for runner-created clients");
DEFINE_int32(segment_size_mb, 64, "Mount segment size in MB");
DEFINE_int32(batch_size, 32, "Batch size for TC-RW-02");
DEFINE_int32(dist_key_count, 300, "Key count for TC-SPDK-02");
DEFINE_int32(retry_ms, 200, "Retry interval in milliseconds");
DEFINE_int32(retry_count, 50, "Retry count for eventual consistency checks");

std::atomic<int> g_port_alloc{0};

UUID MakeRunnerUUID() {
    const auto now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const uint64_t rand_part =
        (static_cast<uint64_t>(std::rand()) << 32) ^ static_cast<uint64_t>(std::rand());
    return {now_ns, rand_part};
}

std::string NextHostname() {
    const int idx = g_port_alloc.fetch_add(1);
    return "127.0.0.1:" + std::to_string(FLAGS_client_port_base + idx);
}

std::string MakeUniqueKey(const std::string& prefix, int index) {
    static thread_local std::mt19937_64 rng([]() {
        std::random_device rd;
        const uint64_t seed =
            (static_cast<uint64_t>(rd()) << 32) ^
            static_cast<uint64_t>(rd()) ^
            static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        return std::mt19937_64(seed);
    }());

    const uint64_t random_part = rng();
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return prefix + "_" + std::to_string(now_ns) + "_" +
           std::to_string(random_part) + "_" + std::to_string(index);
}

std::string Trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(),
            std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> SplitCsv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        token = Trim(token);
        if (!token.empty()) {
            out.push_back(token);
        }
    }
    return out;
}

bool ExpectOk(bool cond, const std::string& msg) {
    if (!cond) {
        LOG(ERROR) << msg;
        return false;
    }
    return true;
}

std::optional<std::shared_ptr<Client>> CreateClient(const std::string& master_addr,
                                                    const std::string& metadata,
                                                    const std::string& protocol,
                                                    const std::string& device_name) {
    std::optional<std::string> devices = std::nullopt;
    if (!device_name.empty()) {
        devices = device_name;
    }
    return Client::Create(NextHostname(), metadata, protocol, devices, master_addr);
}

bool MountSegment(const std::shared_ptr<Client>& client,
                  std::unique_ptr<char[]>& segment_holder,
                  size_t segment_size) {
    segment_holder = std::make_unique<char[]>(segment_size);
    std::memset(segment_holder.get(), 0, segment_size);
    auto mount_ret = client->MountSegment(segment_holder.get(), segment_size);
    if (!mount_ret) {
        LOG(ERROR) << "MountSegment failed: " << mooncake::toString(mount_ret.error());
        return false;
    }
    return true;
}

bool UnmountSegmentSafe(const std::shared_ptr<Client>& client,
                        const std::unique_ptr<char[]>& segment_holder,
                        size_t segment_size) {
    if (segment_holder == nullptr) {
        return true;
    }
    auto unmount_ret = client->UnmountSegment(segment_holder.get(), segment_size);
    if (!unmount_ret) {
        LOG(WARNING) << "UnmountSegment failed: " << mooncake::toString(unmount_ret.error());
        return false;
    }
    return true;
}

bool PutString(const std::shared_ptr<Client>& client,
               const std::string& key,
               const std::string& value) {
    std::vector<char> payload(value.begin(), value.end());
    std::vector<Slice> slices{{payload.data(), payload.size()}};
    ReplicateConfig config;
    config.replica_num = 1;
    auto put_ret = client->Put(key, slices, config);
    if (!put_ret) {
        LOG(ERROR) << "Put failed key=" << key
                   << " err=" << mooncake::toString(put_ret.error());
        return false;
    }
    return true;
}

bool GetStringByQuery(const std::shared_ptr<Client>& client,
                      const std::string& key,
                      size_t expected_size,
                      std::string& value_out) {
    auto q = client->Query(key);
    if (!q) {
        LOG(ERROR) << "Query failed key=" << key
                   << " err=" << mooncake::toString(q.error());
        return false;
    }

    std::string recv(expected_size, '\0');
    std::vector<Slice> slices{{recv.data(), recv.size()}};
    auto get_ret = client->Get(key, q.value(), slices);
    if (!get_ret) {
        LOG(ERROR) << "Get failed key=" << key
                   << " err=" << mooncake::toString(get_ret.error());
        return false;
    }
    value_out = std::move(recv);
    return true;
}

std::optional<Replica::Descriptor> WaitForSsdReplica(const std::shared_ptr<Client>& client,
                                                     const std::string& key,
                                                     int retry_count,
                                                     int retry_ms) {
    for (int i = 0; i < retry_count; ++i) {
        auto q = client->Query(key);
        if (q) {
            for (const auto& rep : q.value().replicas) {
                if (rep.is_ssd_pool_replica()) {
                    return rep;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_ms));
    }
    return std::nullopt;
}

bool RunTcCfg01() {
    MasterClient master_client(MakeRunnerUUID());
    const auto connect_ret = master_client.Connect(FLAGS_master);
    if (connect_ret != ErrorCode::OK) {
        LOG(ERROR) << "Master connect failed: " << mooncake::toString(connect_ret);
        return false;
    }

    auto cfg = master_client.GetTieredStorageConfig();
    if (!cfg) {
        LOG(ERROR) << "GetTieredStorageConfig failed: "
                   << mooncake::toString(cfg.error());
        return false;
    }

    const auto expected_impl = Trim(FLAGS_expect_impl);
    if (!expected_impl.empty() && cfg->nvmeof_client_impl != expected_impl) {
        LOG(ERROR) << "nvmeof_client_impl mismatch, expected=" << expected_impl
                   << " actual=" << cfg->nvmeof_client_impl;
        return false;
    }

    if (!Trim(FLAGS_expect_tiers).empty()) {
        const auto expected = SplitCsv(FLAGS_expect_tiers);
        std::set<std::string> expected_set(expected.begin(), expected.end());
        std::set<std::string> actual_set(cfg->enabled_tiers.begin(), cfg->enabled_tiers.end());
        if (actual_set != expected_set) {
            std::ostringstream oss;
            oss << "enabled_tiers mismatch, expected={";
            for (const auto& t : expected_set) {
                oss << t << " ";
            }
            oss << "} actual={";
            for (const auto& t : actual_set) {
                oss << t << " ";
            }
            oss << "}";
            LOG(ERROR) << oss.str();
            return false;
        }
    }

    LOG(INFO) << "TC-CFG-01 passed";
    return true;
}

bool RunTcRw01() {
    const size_t segment_size = static_cast<size_t>(FLAGS_segment_size_mb) * 1024UL * 1024UL;
    auto client_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!client_opt.has_value()) {
        LOG(ERROR) << "Client::Create failed in TC-RW-01";
        return false;
    }
    auto client = client_opt.value();

    std::unique_ptr<char[]> seg;
    if (!MountSegment(client, seg, segment_size)) {
        return false;
    }

    const std::string key = "tc_rw01_key_" + std::to_string(std::time(nullptr));
    const std::string value = "hello_tiered_rw01";

    if (!PutString(client, key, value)) {
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }

    std::string got;
    if (!GetStringByQuery(client, key, value.size(), got)) {
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }

    if (!ExpectOk(got == value, "TC-RW-01 value mismatch")) {
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }

    UnmountSegmentSafe(client, seg, segment_size);
    LOG(INFO) << "TC-RW-01 passed";
    return true;
}

bool RunTcRw02() {
    const size_t segment_size = static_cast<size_t>(FLAGS_segment_size_mb) * 1024UL * 1024UL;
    auto client_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!client_opt.has_value()) {
        LOG(ERROR) << "Client::Create failed in TC-RW-02";
        return false;
    }
    auto client = client_opt.value();

    std::unique_ptr<char[]> seg;
    if (!MountSegment(client, seg, segment_size)) {
        return false;
    }

    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(static_cast<size_t>(FLAGS_batch_size));
    values.reserve(static_cast<size_t>(FLAGS_batch_size));

    for (int i = 0; i < FLAGS_batch_size; ++i) {
        keys.push_back("tc_rw02_key_" + std::to_string(std::time(nullptr)) + "_" +
                       std::to_string(i));
        values.push_back("rw02_value_" + std::to_string(i) + "_payload");
    }

    std::vector<std::vector<char>> put_payloads;
    put_payloads.reserve(keys.size());
    std::vector<std::vector<Slice>> batched_slices;
    batched_slices.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        put_payloads.emplace_back(values[i].begin(), values[i].end());
        batched_slices.push_back({Slice{put_payloads.back().data(), put_payloads.back().size()}});
    }

    ReplicateConfig config;
    config.replica_num = 1;

    auto batch_put_results = client->BatchPut(keys, batched_slices, config);
    if (batch_put_results.size() != keys.size()) {
        LOG(ERROR) << "BatchPut result size mismatch";
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }
    for (size_t i = 0; i < batch_put_results.size(); ++i) {
        if (!batch_put_results[i]) {
            LOG(ERROR) << "BatchPut failed key=" << keys[i]
                       << " err=" << mooncake::toString(batch_put_results[i].error());
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
    }

    auto batch_query_results = client->BatchQuery(keys);
    if (batch_query_results.size() != keys.size()) {
        LOG(ERROR) << "BatchQuery result size mismatch";
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }

    std::vector<QueryResult> query_results;
    query_results.reserve(keys.size());
    for (size_t i = 0; i < batch_query_results.size(); ++i) {
        if (!batch_query_results[i]) {
            LOG(ERROR) << "BatchQuery failed key=" << keys[i]
                       << " err=" << mooncake::toString(batch_query_results[i].error());
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
        query_results.push_back(batch_query_results[i].value());
    }

    std::unordered_map<std::string, std::string> recv_payloads;
    recv_payloads.reserve(keys.size());
    std::unordered_map<std::string, std::vector<Slice>> recv_slices;
    recv_slices.reserve(keys.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        recv_payloads.emplace(keys[i], std::string(values[i].size(), '\0'));
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        auto& buf = recv_payloads.at(keys[i]);
        recv_slices.emplace(keys[i], std::vector<Slice>{Slice{buf.data(), buf.size()}});
    }

    auto batch_get_results = client->BatchGet(keys, query_results, recv_slices);
    if (batch_get_results.size() != keys.size()) {
        LOG(ERROR) << "BatchGet result size mismatch";
        UnmountSegmentSafe(client, seg, segment_size);
        return false;
    }
    for (size_t i = 0; i < batch_get_results.size(); ++i) {
        if (!batch_get_results[i]) {
            LOG(ERROR) << "BatchGet failed key=" << keys[i]
                       << " err=" << mooncake::toString(batch_get_results[i].error());
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
        if (recv_payloads.at(keys[i]) != values[i]) {
            LOG(ERROR) << "BatchGet value mismatch key=" << keys[i];
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
    }

    UnmountSegmentSafe(client, seg, segment_size);
    LOG(INFO) << "TC-RW-02 passed";
    return true;
}

bool RunTcRfs01() {
    const size_t segment_size = static_cast<size_t>(FLAGS_segment_size_mb) * 1024UL * 1024UL;

    auto writer_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!writer_opt.has_value()) {
        LOG(ERROR) << "Writer client init failed in TC-RFS-01";
        return false;
    }
    auto writer = writer_opt.value();

    std::unique_ptr<char[]> writer_seg;
    if (!MountSegment(writer, writer_seg, segment_size)) {
        return false;
    }

    const std::string key = "tc_rfs01_key_" + std::to_string(std::time(nullptr));
    const std::string value = "tc_rfs01_value_persist";

    if (!PutString(writer, key, value)) {
        UnmountSegmentSafe(writer, writer_seg, segment_size);
        return false;
    }

    UnmountSegmentSafe(writer, writer_seg, segment_size);
    writer.reset();

    auto reader_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!reader_opt.has_value()) {
        LOG(ERROR) << "Reader client init failed in TC-RFS-01";
        return false;
    }
    auto reader = reader_opt.value();

    for (int i = 0; i < FLAGS_retry_count; ++i) {
        std::string got;
        if (GetStringByQuery(reader, key, value.size(), got) && got == value) {
            LOG(INFO) << "TC-RFS-01 passed";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(FLAGS_retry_ms));
    }

    LOG(ERROR) << "TC-RFS-01 failed: cannot read persisted value after restart";
    return false;
}

bool RunTcSpdk01() {
    const size_t segment_size = static_cast<size_t>(FLAGS_segment_size_mb) * 1024UL * 1024UL;

    auto client_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!client_opt.has_value()) {
        LOG(ERROR) << "Client init failed in TC-SPDK-01";
        return false;
    }
    auto client = client_opt.value();

    std::unique_ptr<char[]> seg;
    if (!MountSegment(client, seg, segment_size)) {
        return false;
    }

    std::set<std::string> ssd_endpoints;
    const int probe_keys = std::max(30, FLAGS_batch_size);
    for (int i = 0; i < probe_keys; ++i) {
        const std::string key = MakeUniqueKey("tc_spdk01_key", i);
        const std::string value = "spdk01_value_" + std::to_string(i);
        if (!PutString(client, key, value)) {
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
        auto ssd_rep = WaitForSsdReplica(client, key, FLAGS_retry_count, FLAGS_retry_ms);
        if (ssd_rep.has_value()) {
            ssd_endpoints.insert(ssd_rep->get_ssd_extent_descriptor().target_endpoint);
        }
    }

    UnmountSegmentSafe(client, seg, segment_size);

    if (!ExpectOk(ssd_endpoints.size() >= 3,
                  "TC-SPDK-01 failed: runner observed fewer than 3 SSD target endpoints")) {
        return false;
    }

    LOG(INFO) << "TC-SPDK-01 passed, endpoints=" << ssd_endpoints.size();
    return true;
}

bool RunTcSpdk02() {
    const size_t segment_size = static_cast<size_t>(FLAGS_segment_size_mb) * 1024UL * 1024UL;

    auto client_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    if (!client_opt.has_value()) {
        LOG(ERROR) << "Client init failed in TC-SPDK-02";
        return false;
    }
    auto client = client_opt.value();

    std::unique_ptr<char[]> seg;
    if (!MountSegment(client, seg, segment_size)) {
        return false;
    }

    std::unordered_map<std::string, int> hit_count;
    hit_count.reserve(4);

    for (int i = 0; i < FLAGS_dist_key_count; ++i) {
        const std::string key = MakeUniqueKey("tc_spdk02_key", i);
        const std::string value = "spdk02_value_" + std::to_string(i);
        if (!PutString(client, key, value)) {
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }

        auto ssd_rep = WaitForSsdReplica(client, key, FLAGS_retry_count, FLAGS_retry_ms);
        if (!ssd_rep.has_value()) {
            LOG(ERROR) << "No SSD replica observed for key=" << key;
            UnmountSegmentSafe(client, seg, segment_size);
            return false;
        }
        const auto endpoint = ssd_rep->get_ssd_extent_descriptor().target_endpoint;
        hit_count[endpoint] += 1;
    }

    UnmountSegmentSafe(client, seg, segment_size);

    if (!ExpectOk(hit_count.size() >= 3,
                  "TC-SPDK-02 failed: fewer than 3 target endpoints were hit")) {
        return false;
    }

    int min_hit = std::numeric_limits<int>::max();
    int max_hit = 0;
    for (const auto& [_, v] : hit_count) {
        min_hit = std::min(min_hit, v);
        max_hit = std::max(max_hit, v);
    }

    if (!ExpectOk(min_hit > 0, "TC-SPDK-02 failed: min hit is zero")) {
        return false;
    }

    const double skew = static_cast<double>(max_hit) / static_cast<double>(min_hit);
    if (!ExpectOk(skew <= 5.0,
                  "TC-SPDK-02 failed: target distribution skew is too high")) {
        LOG(ERROR) << "distribution skew=" << skew
                   << " max_hit=" << max_hit << " min_hit=" << min_hit;
        return false;
    }

    LOG(INFO) << "TC-SPDK-02 passed, skew=" << skew;
    return true;
}

bool RunTcSpdk03() {
    auto client_opt = CreateClient(FLAGS_master, FLAGS_metadata, FLAGS_protocol, FLAGS_device_name);
    const bool created = client_opt.has_value();

    if (FLAGS_expect_client_init_fail) {
        if (created) {
            LOG(ERROR) << "TC-SPDK-03 failed: expected Client::Create failure but succeeded";
            return false;
        }
        LOG(INFO) << "TC-SPDK-03 passed (client fail-fast observed)";
        return true;
    }

    if (!created) {
        LOG(ERROR) << "TC-SPDK-03 failed: client init failed unexpectedly";
        return false;
    }

    LOG(INFO) << "TC-SPDK-03 passed (client init succeeded)";
    return true;
}

bool RunCase(const std::string& case_id) {
    if (case_id == "TC-CFG-01") {
        return RunTcCfg01();
    }
    if (case_id == "TC-RW-01") {
        return RunTcRw01();
    }
    if (case_id == "TC-RW-02") {
        return RunTcRw02();
    }
    if (case_id == "TC-RFS-01") {
        return RunTcRfs01();
    }
    if (case_id == "TC-SPDK-01") {
        return RunTcSpdk01();
    }
    if (case_id == "TC-SPDK-02") {
        return RunTcSpdk02();
    }
    if (case_id == "TC-SPDK-03") {
        return RunTcSpdk03();
    }

    if (case_id == "all") {
        const std::vector<std::string> all_cases = {
            "TC-CFG-01", "TC-RW-01", "TC-RW-02", "TC-RFS-01", "TC-SPDK-01",
            "TC-SPDK-02"};
        for (const auto& id : all_cases) {
            if (!RunCase(id)) {
                return false;
            }
        }
        return true;
    }

    LOG(ERROR) << "Unknown case id: " << case_id;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;

    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    const bool ok = RunCase(FLAGS_case);
    return ok ? 0 : 1;
}
