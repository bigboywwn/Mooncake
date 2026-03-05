#include "master_service.h"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "types.h"

namespace mooncake::test {

std::unique_ptr<MasterService> CreateMasterServiceWithSSDFeat(
    const std::string& root_fs_dir) {
    return std::make_unique<MasterService>(
        MasterServiceConfig::builder().set_root_fs_dir(root_fs_dir).build());
}

class MasterServiceSSDTest : public ::testing::Test {
   protected:
    void SetUp() override {
        google::InitGoogleLogging("MasterServiceTest");
        FLAGS_logtostderr = true;
    }

    void TearDown() override { google::ShutdownGoogleLogging(); }
};

class ScopedEnvVar {
   public:
    ScopedEnvVar(const char* key, const char* value) : key_(key) {
        const char* old = std::getenv(key);
        if (old != nullptr) {
            had_old_ = true;
            old_value_ = old;
        }
        setenv(key_, value, 1);
    }

    ~ScopedEnvVar() {
        if (had_old_) {
            setenv(key_, old_value_.c_str(), 1);
        } else {
            unsetenv(key_);
        }
    }

   private:
    const char* key_;
    bool had_old_{false};
    std::string old_value_;
};

TEST_F(MasterServiceSSDTest, PutStartReturnsSsdPoolDescriptorWhenEnabled) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "file:///tmp/mooncake-ssd-a.bin");

    auto service = std::make_unique<MasterService>(MasterServiceConfig::builder().build());

    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = 0x300000000;
    segment.size = 1024 * 1024 * 64;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig config;
    config.replica_num = 1;
    auto put_start = service->PutStart(client_id, "ssd_enabled_key", 4096, config);
    ASSERT_TRUE(put_start.has_value());

    bool has_memory = false;
    bool has_ssd = false;
    for (const auto& rep : put_start.value()) {
        if (rep.is_memory_replica()) {
            has_memory = true;
        }
        if (rep.is_ssd_pool_replica()) {
            has_ssd = true;
        }
    }
    EXPECT_TRUE(has_memory);
    EXPECT_TRUE(has_ssd);
}

TEST_F(MasterServiceSSDTest, ConsistentHashSelectsStableTargetForSameKey) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar ddr_enabled("MC_DDR_POOL_ENABLED", "0");
    ScopedEnvVar targets(
        "MC_SSD_TARGETS_JSON",
        R"([{"name":"t1","trtype":"tcp","traddr":"10.10.10.1","trsvcid":"4420","subnqn":"nqn.2026-03.io.mooncake:ssdpool","nsid":1,"capacity_bytes":10485760,"weight":1},{"name":"t2","trtype":"tcp","traddr":"10.10.10.2","trsvcid":"4420","subnqn":"nqn.2026-03.io.mooncake:ssdpool","nsid":1,"capacity_bytes":10485760,"weight":1}])");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder().set_root_fs_dir("").build());
    UUID client_id = generate_uuid();
    ReplicateConfig config;
    config.replica_num = 1;

    auto first = service->PutStart(client_id, "stable-key", 4096, config);
    ASSERT_TRUE(first.has_value());
    std::string first_endpoint;
    for (const auto& rep : first.value()) {
        if (rep.is_ssd_pool_replica()) {
            first_endpoint = rep.get_ssd_extent_descriptor().target_endpoint;
            break;
        }
    }
    ASSERT_FALSE(first_endpoint.empty());
    ASSERT_TRUE(
        service->PutRevoke(client_id, "stable-key", ReplicaType::SSD_POOL)
            .has_value());

    auto second = service->PutStart(client_id, "stable-key", 4096, config);
    ASSERT_TRUE(second.has_value());
    std::string second_endpoint;
    for (const auto& rep : second.value()) {
        if (rep.is_ssd_pool_replica()) {
            second_endpoint = rep.get_ssd_extent_descriptor().target_endpoint;
            break;
        }
    }
    ASSERT_FALSE(second_endpoint.empty());
    EXPECT_EQ(first_endpoint, second_endpoint);
}

TEST_F(MasterServiceSSDTest, ConsistentHashHonorsWeight) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar ddr_enabled("MC_DDR_POOL_ENABLED", "0");
    ScopedEnvVar targets(
        "MC_SSD_TARGETS_JSON",
        R"([{"name":"w1","trtype":"tcp","traddr":"10.10.20.1","trsvcid":"4420","subnqn":"nqn.2026-03.io.mooncake:ssdpool","nsid":1,"capacity_bytes":536870912,"weight":1},{"name":"w3","trtype":"tcp","traddr":"10.10.20.2","trsvcid":"4420","subnqn":"nqn.2026-03.io.mooncake:ssdpool","nsid":1,"capacity_bytes":536870912,"weight":3}])");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder().set_root_fs_dir("").build());
    UUID client_id = generate_uuid();
    ReplicateConfig config;
    config.replica_num = 1;

    int count_w1 = 0;
    int count_w3 = 0;
    for (int i = 0; i < 128; ++i) {
        const std::string key = "weight-key-" + std::to_string(i);
        auto put_start = service->PutStart(client_id, key, 4096, config);
        ASSERT_TRUE(put_start.has_value());
        std::string endpoint;
        for (const auto& rep : put_start.value()) {
            if (rep.is_ssd_pool_replica()) {
                endpoint = rep.get_ssd_extent_descriptor().target_endpoint;
                break;
            }
        }
        ASSERT_FALSE(endpoint.empty());
        if (endpoint == "10.10.20.1:4420") {
            ++count_w1;
        } else if (endpoint == "10.10.20.2:4420") {
            ++count_w3;
        }
        ASSERT_TRUE(service->PutRevoke(client_id, key, ReplicaType::SSD_POOL)
                        .has_value());
    }
    EXPECT_GT(count_w3, count_w1);
}

TEST_F(MasterServiceSSDTest, BatchReplicaClearAllReleasesSsdExtent) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "10.10.30.1:4420");
    ScopedEnvVar capacity("MC_SSD_POOL_CAPACITY_BYTES", "4096");
    ScopedEnvVar block_size("MC_SSD_POOL_BLOCK_SIZE", "4096");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder()
            .set_root_fs_dir("")
            .set_default_kv_lease_ttl(0)
            .build());

    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = 0x3A0000000;
    segment.size = 1024 * 1024 * 64;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig config;
    config.replica_num = 1;

    auto first = service->PutStart(client_id, "clear-all-key-1", 1024, config);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(
        service->PutEnd(client_id, "clear-all-key-1", ReplicaType::MEMORY)
            .has_value());
    ASSERT_TRUE(
        service->PutEnd(client_id, "clear-all-key-1", ReplicaType::SSD_POOL)
            .has_value());

    auto clear_result =
        service->BatchReplicaClear({"clear-all-key-1"}, client_id, "");
    ASSERT_TRUE(clear_result.has_value());
    ASSERT_EQ(clear_result.value().size(), 1);

    auto second = service->PutStart(client_id, "clear-all-key-2", 1024, config);
    ASSERT_TRUE(second.has_value());
    bool has_ssd = false;
    for (const auto& rep : second.value()) {
        if (rep.is_ssd_pool_replica()) {
            has_ssd = true;
            break;
        }
    }
    EXPECT_TRUE(has_ssd);
}

TEST_F(MasterServiceSSDTest, BatchReplicaClearSegmentReleasesMatchingSsdExtent) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar ddr_enabled("MC_DDR_POOL_ENABLED", "0");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "10.10.35.1:4420");
    ScopedEnvVar capacity("MC_SSD_POOL_CAPACITY_BYTES", "4096");
    ScopedEnvVar block_size("MC_SSD_POOL_BLOCK_SIZE", "4096");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder()
            .set_root_fs_dir("")
            .set_default_kv_lease_ttl(0)
            .build());

    UUID client_id = generate_uuid();
    ReplicateConfig config;
    config.replica_num = 1;

    auto put_start = service->PutStart(client_id, "clear-seg-key", 1024, config);
    ASSERT_TRUE(put_start.has_value());
    std::string target_endpoint;
    for (const auto& rep : put_start.value()) {
        if (rep.is_ssd_pool_replica()) {
            target_endpoint = rep.get_ssd_extent_descriptor().target_endpoint;
            break;
        }
    }
    ASSERT_FALSE(target_endpoint.empty());
    ASSERT_TRUE(
        service->PutEnd(client_id, "clear-seg-key", ReplicaType::SSD_POOL)
            .has_value());

    auto clear_result =
        service->BatchReplicaClear({"clear-seg-key"}, client_id, target_endpoint);
    ASSERT_TRUE(clear_result.has_value());
    ASSERT_EQ(clear_result.value().size(), 1);

    auto second = service->PutStart(client_id, "clear-seg-key-2", 1024, config);
    ASSERT_TRUE(second.has_value());
    bool has_ssd = false;
    for (const auto& rep : second.value()) {
        if (rep.is_ssd_pool_replica()) {
            has_ssd = true;
            break;
        }
    }
    EXPECT_TRUE(has_ssd);
}

TEST_F(MasterServiceSSDTest, ReportSsdWriteResultFailureRevokesProcessingReplica) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar ddr_enabled("MC_DDR_POOL_ENABLED", "0");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "10.10.36.1:4420");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder()
            .set_root_fs_dir("")
            .set_default_kv_lease_ttl(0)
            .build());

    UUID client_id = generate_uuid();
    ReplicateConfig config;
    config.replica_num = 1;
    auto put_start =
        service->PutStart(client_id, "ssd-report-fail-key", 2048, config);
    ASSERT_TRUE(put_start.has_value());

    std::string extent_id;
    for (const auto& rep : put_start.value()) {
        if (rep.is_ssd_pool_replica()) {
            extent_id = rep.get_ssd_extent_descriptor().extent_id;
            break;
        }
    }
    ASSERT_FALSE(extent_id.empty());

    auto report = service->ReportSsdWriteResult(client_id, "ssd-report-fail-key",
                                                extent_id, false,
                                                ErrorCode::SSD_IO_TIMEOUT);
    ASSERT_TRUE(report.has_value());

    auto list = service->GetReplicaList("ssd-report-fail-key");
    ASSERT_FALSE(list.has_value());
    EXPECT_EQ(list.error(), ErrorCode::OBJECT_NOT_FOUND);
}

TEST_F(MasterServiceSSDTest, TieredConfigReflectsSsdOnlyWhenDdrDisabled) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar ddr_enabled("MC_DDR_POOL_ENABLED", "0");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "10.10.40.1:4420");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder().set_root_fs_dir("").build());
    UUID client_id = generate_uuid();
    ReplicateConfig config;
    config.replica_num = 1;

    auto put_start = service->PutStart(client_id, "ssd-only-key", 1024, config);
    ASSERT_TRUE(put_start.has_value());
    bool has_memory = false;
    bool has_ssd = false;
    for (const auto& rep : put_start.value()) {
        has_memory = has_memory || rep.is_memory_replica();
        has_ssd = has_ssd || rep.is_ssd_pool_replica();
    }
    EXPECT_FALSE(has_memory);
    EXPECT_TRUE(has_ssd);

    auto tiered = service->GetTieredStorageConfig();
    ASSERT_TRUE(tiered.has_value());
    auto tiers = tiered.value().enabled_tiers;
    EXPECT_TRUE(std::find(tiers.begin(), tiers.end(), "SSD") != tiers.end());
    EXPECT_TRUE(std::find(tiers.begin(), tiers.end(), "DDR") == tiers.end());
}

TEST_F(MasterServiceSSDTest, QueryPrefersMemoryThenSsdThenDisk) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "file:///tmp/mooncake-ssd-b.bin");

    auto service = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = 0x310000000;
    segment.size = 1024 * 1024 * 64;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig config;
    config.replica_num = 1;
    const std::string key = "query_priority_key";
    ASSERT_TRUE(service->PutStart(client_id, key, 1024, config).has_value());

    ASSERT_TRUE(service->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());
    ASSERT_TRUE(service->PutEnd(client_id, key, ReplicaType::SSD_POOL).has_value());
    ASSERT_TRUE(service->PutEnd(client_id, key, ReplicaType::DISK).has_value());

    auto query = service->GetReplicaList(key);
    ASSERT_TRUE(query.has_value());
    ASSERT_FALSE(query.value().replicas.empty());
    EXPECT_TRUE(query.value().replicas.front().is_memory_replica());

    ASSERT_TRUE(service->PutRevoke(client_id, key, ReplicaType::MEMORY).has_value());
    query = service->GetReplicaList(key);
    ASSERT_TRUE(query.has_value());
    ASSERT_FALSE(query.value().replicas.empty());
    EXPECT_TRUE(query.value().replicas.front().is_ssd_pool_replica());
}

TEST_F(MasterServiceSSDTest, PutRevokeSsdReleasesExtentCapacity) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "file:///tmp/mooncake-ssd-c.bin");
    ScopedEnvVar capacity("MC_SSD_POOL_CAPACITY_BYTES", "4096");
    ScopedEnvVar block_size("MC_SSD_POOL_BLOCK_SIZE", "4096");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder().set_root_fs_dir("").build());

    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = 0x320000000;
    segment.size = 1024 * 1024 * 64;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig config;
    config.replica_num = 1;
    auto put_start_1 = service->PutStart(client_id, "ssd_revoke_key_1", 1024, config);
    ASSERT_TRUE(put_start_1.has_value());
    bool has_ssd_1 = false;
    for (const auto& rep : put_start_1.value()) {
        if (rep.is_ssd_pool_replica()) {
            has_ssd_1 = true;
            break;
        }
    }
    ASSERT_TRUE(has_ssd_1);

    ASSERT_TRUE(service->PutRevoke(client_id, "ssd_revoke_key_1",
                                   ReplicaType::SSD_POOL)
                    .has_value());
    ASSERT_TRUE(service->PutRevoke(client_id, "ssd_revoke_key_1",
                                   ReplicaType::MEMORY)
                    .has_value());

    auto put_start_2 = service->PutStart(client_id, "ssd_revoke_key_2", 1024, config);
    ASSERT_TRUE(put_start_2.has_value());
    bool has_ssd_2 = false;
    for (const auto& rep : put_start_2.value()) {
        if (rep.is_ssd_pool_replica()) {
            has_ssd_2 = true;
            break;
        }
    }
    EXPECT_TRUE(has_ssd_2);
}

TEST_F(MasterServiceSSDTest, RemoveReleasesSsdExtentCapacity) {
    ScopedEnvVar enabled("MC_SSD_POOL_ENABLED", "1");
    ScopedEnvVar targets("MC_SSD_POOL_TARGETS", "file:///tmp/mooncake-ssd-d.bin");
    ScopedEnvVar capacity("MC_SSD_POOL_CAPACITY_BYTES", "4096");
    ScopedEnvVar block_size("MC_SSD_POOL_BLOCK_SIZE", "4096");

    auto service = std::make_unique<MasterService>(
        MasterServiceConfig::builder()
            .set_root_fs_dir("")
            .set_default_kv_lease_ttl(0)
            .build());

    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = 0x330000000;
    segment.size = 1024 * 1024 * 64;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    ASSERT_TRUE(service->MountSegment(segment, client_id).has_value());

    ReplicateConfig config;
    config.replica_num = 1;
    auto put_start_1 = service->PutStart(client_id, "ssd_remove_key_1", 1024, config);
    ASSERT_TRUE(put_start_1.has_value());
    ASSERT_TRUE(service->PutEnd(client_id, "ssd_remove_key_1", ReplicaType::MEMORY)
                    .has_value());
    ASSERT_TRUE(service->PutEnd(client_id, "ssd_remove_key_1", ReplicaType::SSD_POOL)
                    .has_value());

    ASSERT_TRUE(service->Remove("ssd_remove_key_1").has_value());

    auto put_start_2 = service->PutStart(client_id, "ssd_remove_key_2", 1024, config);
    ASSERT_TRUE(put_start_2.has_value());
    bool has_ssd = false;
    for (const auto& rep : put_start_2.value()) {
        if (rep.is_ssd_pool_replica()) {
            has_ssd = true;
            break;
        }
    }
    EXPECT_TRUE(has_ssd);
}

TEST_F(MasterServiceSSDTest, PutEndBothReplica) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "disk_key";
    uint64_t slice_length = 1024;
    ReplicateConfig config;
    config.replica_num = 1;

    auto put_start_result =
        service_->PutStart(client_id, key, slice_length, config);
    ASSERT_TRUE(put_start_result.has_value());
    auto replicas = put_start_result.value();
    ASSERT_EQ(2, replicas.size());

    bool has_mem = false, has_disk = false;
    for (const auto& r : replicas) {
        if (r.is_memory_replica()) has_mem = true;
        if (r.is_disk_replica()) has_disk = true;
    }
    EXPECT_TRUE(has_mem);
    EXPECT_TRUE(has_disk);

    auto get_result = service_->GetReplicaList(key);
    ASSERT_FALSE(get_result.has_value());
    EXPECT_EQ(ErrorCode::REPLICA_IS_NOT_READY, get_result.error());

    // PutEnd for both memory and disk
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::DISK).has_value());

    get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(2, get_result.value().replicas.size());

    for (const auto& r : get_result.value().replicas) {
        EXPECT_EQ(ReplicaStatus::COMPLETE, r.status);
    }
}

TEST_F(MasterServiceSSDTest, PutRevokeDiskReplica) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "revoke_key";
    uint64_t slice_length = 1024;
    ReplicateConfig config;
    config.replica_num = 1;

    ASSERT_TRUE(
        service_->PutStart(client_id, key, slice_length, config).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());

    auto get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(1, get_result.value().replicas.size());
    ASSERT_TRUE(get_result.value().replicas[0].is_memory_replica());

    EXPECT_TRUE(
        service_->PutRevoke(client_id, key, ReplicaType::DISK).has_value());

    get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(1, get_result.value().replicas.size());
    ASSERT_TRUE(get_result.value().replicas[0].is_memory_replica());
}

TEST_F(MasterServiceSSDTest, PutRevokeMemoryReplica) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "revoke_key";
    uint64_t slice_length = 1024;
    ReplicateConfig config;
    config.replica_num = 1;

    ASSERT_TRUE(
        service_->PutStart(client_id, key, slice_length, config).has_value());
    EXPECT_TRUE(
        service_->PutRevoke(client_id, key, ReplicaType::MEMORY).has_value());

    auto get_result = service_->GetReplicaList(key);
    ASSERT_FALSE(get_result.has_value());
    EXPECT_EQ(ErrorCode::REPLICA_IS_NOT_READY, get_result.error());

    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::DISK).has_value());
    get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(1, get_result.value().replicas.size());
    ASSERT_TRUE(get_result.value().replicas[0].is_disk_replica());
}

TEST_F(MasterServiceSSDTest, PutRevokeBothReplica) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "revoke_key";
    uint64_t slice_length = 1024;
    ReplicateConfig config;
    config.replica_num = 1;

    ASSERT_TRUE(
        service_->PutStart(client_id, key, slice_length, config).has_value());
    EXPECT_TRUE(
        service_->PutRevoke(client_id, key, ReplicaType::DISK).has_value());

    auto get_result = service_->GetReplicaList(key);
    ASSERT_FALSE(get_result.has_value());
    EXPECT_EQ(ErrorCode::REPLICA_IS_NOT_READY, get_result.error());

    EXPECT_TRUE(
        service_->PutRevoke(client_id, key, ReplicaType::MEMORY).has_value());
    get_result = service_->GetReplicaList(key);
    ASSERT_FALSE(get_result.has_value());
    EXPECT_EQ(ErrorCode::OBJECT_NOT_FOUND, get_result.error());
}

TEST_F(MasterServiceSSDTest, RemoveKey) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "remove_key";
    uint64_t slice_length = 1024;
    ReplicateConfig config;
    config.replica_num = 1;

    ASSERT_TRUE(
        service_->PutStart(client_id, key, slice_length, config).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::DISK).has_value());

    EXPECT_TRUE(service_->Remove(key).has_value());

    auto get_result = service_->GetReplicaList(key);
    EXPECT_FALSE(get_result.has_value());
    EXPECT_EQ(ErrorCode::OBJECT_NOT_FOUND, get_result.error());
}

TEST_F(MasterServiceSSDTest, EvictObject) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");
    // Mount a segment that can hold about 1024 * 16 objects.
    // As the eviction is processed separately for each shard,
    // we need to fill each shard with enough objects to thoroughly
    // test the eviction process.
    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 16 * 15;
    constexpr size_t object_size = 1024 * 15;
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();
    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    // Verify if we can put objects more than the segment can hold
    int success_puts = 0;
    for (int i = 0; i < 1024 * 16 + 50; ++i) {
        std::string key = "test_key" + std::to_string(i);
        uint64_t slice_length = object_size;
        ReplicateConfig config;
        config.replica_num = 1;
        auto put_start_result =
            service_->PutStart(client_id, key, slice_length, config);
        if (put_start_result.has_value()) {
            auto put_end_mem_result =
                service_->PutEnd(client_id, key, ReplicaType::MEMORY);
            auto put_end_disk_result =
                service_->PutEnd(client_id, key, ReplicaType::DISK);
            ASSERT_TRUE(put_end_mem_result.has_value());
            ASSERT_TRUE(put_end_disk_result.has_value());
            success_puts++;
        } else {
            // wait for eviction to work
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    ASSERT_GT(success_puts, 1024 * 16);

    // Verify if we can get objects more than the segment can hold
    int success_gets = 0;
    for (int i = 0; i < 1024 * 16 + 50; ++i) {
        std::string key = "test_key" + std::to_string(i);
        auto get_result = service_->GetReplicaList(key);
        if (get_result.has_value()) {
            success_gets++;
        }
    }
    ASSERT_GT(success_gets, 1024 * 16);

    std::this_thread::sleep_for(
        std::chrono::milliseconds(DEFAULT_DEFAULT_KV_LEASE_TTL));
    service_->RemoveAll();
}

TEST_F(MasterServiceSSDTest, PutStartExpires) {
    // Reset storage space metrics.
    MasterMetricManager::instance().reset_allocated_mem_size();
    MasterMetricManager::instance().reset_total_mem_capacity();

    MasterServiceConfig master_config;
    master_config.root_fs_dir = "/mnt/ssd";
    master_config.put_start_discard_timeout_sec = 3;
    master_config.put_start_release_timeout_sec = 5;
    master_config.default_kv_lease_ttl = 15000;
    std::unique_ptr<MasterService> service_(new MasterService(master_config));

    constexpr size_t kReplicaCnt = 2;  // 1 memory replica + 1 disk replica
    constexpr size_t kBaseAddr = 0x300000000;
    constexpr size_t kSegmentSize = 1024 * 1024 * 16;  // 16MB

    // Mount a segment.
    std::string segment_name = "test_segment";
    Segment segment;
    segment.id = generate_uuid();
    segment.name = segment_name;
    segment.base = kBaseAddr;
    segment.size = kSegmentSize;
    segment.te_endpoint = segment.name;
    auto client_id = generate_uuid();
    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "test_key";
    uint64_t value_length = 16 * 1024 * 1024;  // 16MB
    uint64_t slice_length = value_length;
    ReplicateConfig config;

    auto test_discard_replica = [&](ReplicaType discard_type) {
        const auto reserve_type = discard_type == ReplicaType::MEMORY
                                      ? ReplicaType::DISK
                                      : ReplicaType::MEMORY;

        // Put key, should success.
        auto put_start_result =
            service_->PutStart(client_id, key, slice_length, config);
        EXPECT_TRUE(put_start_result.has_value());
        auto replica_list = put_start_result.value();
        EXPECT_EQ(replica_list.size(), kReplicaCnt);
        for (size_t i = 0; i < kReplicaCnt; i++) {
            EXPECT_EQ(ReplicaStatus::PROCESSING, replica_list[i].status);
        }

        // Complete the reserved replica.
        auto put_end_result = service_->PutEnd(client_id, key, reserve_type);
        EXPECT_TRUE(put_end_result.has_value());

        // Wait for a while until the put-start expired.
        for (size_t i = 0; i <= master_config.put_start_discard_timeout_sec;
             i++) {
            // Keep mounted segments alive.
            auto result = service_->Ping(client_id);
            EXPECT_TRUE(result.has_value());
            // Protect the key from eviction.
            auto get_result = service_->GetReplicaList(key);
            EXPECT_TRUE(get_result.has_value());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Put key again, should fail because the object has had an completed
        // replica.
        put_start_result =
            service_->PutStart(client_id, key, slice_length, config);
        EXPECT_FALSE(put_start_result.has_value());
        EXPECT_EQ(put_start_result.error(), ErrorCode::OBJECT_ALREADY_EXISTS);

        // Wait for a while until the discarded replicas are released.
        for (size_t i = 0; i <= master_config.put_start_release_timeout_sec;
             i++) {
            // Keep mounted segments alive.
            auto result = service_->Ping(client_id);
            EXPECT_TRUE(result.has_value());
            // Protect the key from eviction.
            auto get_result = service_->GetReplicaList(key);
            EXPECT_TRUE(get_result.has_value());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Try PutEnd the discarded replica.
        put_end_result = service_->PutEnd(client_id, key, discard_type);
        EXPECT_TRUE(put_end_result.has_value());

        // Check that the key has only one replica.
        auto get_result = service_->GetReplicaList(key);
        EXPECT_TRUE(get_result.has_value());
        EXPECT_EQ(get_result.value().replicas.size(), 1);
        if (reserve_type == ReplicaType::MEMORY) {
            EXPECT_TRUE(get_result.value().replicas[0].is_memory_replica());
        } else {
            EXPECT_TRUE(get_result.value().replicas[0].is_disk_replica());
        }

        // Wait for key lease expiry before RemoveAll to keep the next round clean.
        for (size_t i = 0; i <= master_config.default_kv_lease_ttl / 1000; i++) {
            auto result = service_->Ping(client_id);
            EXPECT_TRUE(result.has_value());
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        service_->RemoveAll();
    };

    test_discard_replica(ReplicaType::DISK);
    test_discard_replica(ReplicaType::MEMORY);
}

TEST_F(MasterServiceSSDTest, EvictDiskReplica_RemovesDiskReplica) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "evict_disk_key";
    auto put_result =
        service_->PutStart(client_id, key, 1024, {.replica_num = 1});
    ASSERT_TRUE(put_result.has_value());

    // Complete both replicas
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::DISK).has_value());

    // Verify we have 2 replicas (MEM + DISK)
    auto get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(2, get_result.value().replicas.size());

    // Evict disk replica
    auto evict_result =
        service_->EvictDiskReplica(client_id, key, ReplicaType::DISK);
    ASSERT_TRUE(evict_result.has_value());

    // Verify only memory replica remains
    get_result = service_->GetReplicaList(key);
    ASSERT_TRUE(get_result.has_value());
    EXPECT_EQ(1, get_result.value().replicas.size());
    EXPECT_TRUE(get_result.value().replicas[0].is_memory_replica());
}

TEST_F(MasterServiceSSDTest, EvictDiskReplica_NonExistentKeyReturnsError) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    UUID client_id = generate_uuid();
    auto evict_result = service_->EvictDiskReplica(client_id, "nonexistent_key",
                                                   ReplicaType::DISK);
    EXPECT_FALSE(evict_result.has_value());
    EXPECT_EQ(evict_result.error(), ErrorCode::OBJECT_NOT_FOUND);
}

TEST_F(MasterServiceSSDTest, EvictDiskReplica_InvalidReplicaTypeReturnsError) {
    auto service_ = CreateMasterServiceWithSSDFeat("/mnt/ssd");

    constexpr size_t buffer = 0x300000000;
    constexpr size_t size = 1024 * 1024 * 64;
    Segment segment;
    segment.id = generate_uuid();
    segment.name = "test_segment";
    segment.base = buffer;
    segment.size = size;
    segment.te_endpoint = segment.name;
    UUID client_id = generate_uuid();

    auto mount_result = service_->MountSegment(segment, client_id);
    ASSERT_TRUE(mount_result.has_value());

    std::string key = "evict_invalid_type_key";
    auto put_result =
        service_->PutStart(client_id, key, 1024, {.replica_num = 1});
    ASSERT_TRUE(put_result.has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::MEMORY).has_value());
    EXPECT_TRUE(
        service_->PutEnd(client_id, key, ReplicaType::DISK).has_value());

    // Attempting to evict with MEMORY type should fail
    auto evict_result =
        service_->EvictDiskReplica(client_id, key, ReplicaType::MEMORY);
    EXPECT_FALSE(evict_result.has_value());
    EXPECT_EQ(evict_result.error(), ErrorCode::INVALID_PARAMS);
}

}  // namespace mooncake::test

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
