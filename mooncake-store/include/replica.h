#pragma once

#include <glog/logging.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>
#include <optional>
#include <string_view>
#include <ostream>

#include "types.h"
#include "allocator.h"
#include "master_metric_manager.h"

namespace mooncake {

/**
 * @brief Globally unique replica identification.
 */
using ReplicaID = uint64_t;

/**
 * @brief Type of buffer allocator used in the system
 */
enum class ReplicaType {
    MEMORY,     // Memory replica
    DISK,       // Disk replica
    SSD_POOL,   // NVMeoF SSD pool replica (block extent)
    LOCAL_DISK  // Local disk replica
};

/**
 * @brief Stream operator for ReplicaType
 */
inline std::ostream& operator<<(std::ostream& os,
                                const ReplicaType& replicaType) noexcept {
    static const std::unordered_map<ReplicaType, std::string_view>
        replica_type_strings{{ReplicaType::MEMORY, "MEMORY"},
                             {ReplicaType::DISK, "DISK"},
                             {ReplicaType::SSD_POOL, "SSD_POOL"},
                             {ReplicaType::LOCAL_DISK, "LOCAL_DISK"}};

    os << (replica_type_strings.count(replicaType)
               ? replica_type_strings.at(replicaType)
               : "UNKNOWN");
    return os;
}

/**
 * @brief Status of a replica in the system
 */
enum class ReplicaStatus {
    UNDEFINED = 0,  // Uninitialized
    INITIALIZED,    // Space allocated, waiting for write
    PROCESSING,     // Write in progress
    COMPLETE,       // Write complete, replica is available
    REMOVED,        // Replica has been removed
    FAILED,         // Failed state (can be used for reassignment)
};

/**
 * @brief Stream operator for ReplicaStatus
 */
inline std::ostream& operator<<(std::ostream& os,
                                const ReplicaStatus& status) noexcept {
    static const std::unordered_map<ReplicaStatus, std::string_view>
        status_strings{{ReplicaStatus::UNDEFINED, "UNDEFINED"},
                       {ReplicaStatus::INITIALIZED, "INITIALIZED"},
                       {ReplicaStatus::PROCESSING, "PROCESSING"},
                       {ReplicaStatus::COMPLETE, "COMPLETE"},
                       {ReplicaStatus::REMOVED, "REMOVED"},
                       {ReplicaStatus::FAILED, "FAILED"}};

    os << (status_strings.count(status) ? status_strings.at(status)
                                        : "UNKNOWN");
    return os;
}

/**
 * @brief Configuration for replica management
 */
struct ReplicateConfig {
    size_t replica_num{1};
    bool with_soft_pin{false};
    std::vector<std::string>
        preferred_segments{};         // Preferred segments for allocation
    std::string preferred_segment{};  // Deprecated: Single preferred segment
                                      // for backward compatibility
    bool prefer_alloc_in_same_node{false};

    friend std::ostream& operator<<(std::ostream& os,
                                    const ReplicateConfig& config) noexcept {
        os << "ReplicateConfig: { replica_num: " << config.replica_num
           << ", with_soft_pin: " << config.with_soft_pin
           << ", preferred_segments: [";
        for (size_t i = 0; i < config.preferred_segments.size(); ++i) {
            os << config.preferred_segments[i];
            if (i < config.preferred_segments.size() - 1) os << ", ";
        }
        os << "]";
        if (!config.preferred_segment.empty()) {
            os << ", preferred_segment (deprecated): "
               << config.preferred_segment;
        }
        os << ", prefer_alloc_in_same_node: "
           << config.prefer_alloc_in_same_node << " }";
        return os;
    }
};

struct MemoryReplicaData {
    std::unique_ptr<AllocatedBuffer> buffer;
};

struct DiskReplicaData {
    std::string file_path;
    uint64_t object_size = 0;
};

struct LocalDiskReplicaData {
    UUID client_id;
    uint64_t object_size = 0;
    std::string transport_endpoint;
};

struct SsdExtentReplicaData {
    std::string target_endpoint;
    std::string subsystem_nqn;
    uint32_t nsid = 0;
    uint32_t block_size = 4096;
    uint64_t lba_start = 0;
    uint64_t lba_count = 0;
    uint64_t object_size = 0;
    std::string extent_id;
};

struct MemoryDescriptor {
    AllocatedBuffer::Descriptor buffer_descriptor;
    YLT_REFL(MemoryDescriptor, buffer_descriptor);
};

struct DiskDescriptor {
    std::string file_path{};
    uint64_t object_size = 0;
    YLT_REFL(DiskDescriptor, file_path, object_size);
};

struct LocalDiskDescriptor {
    UUID client_id;
    uint64_t object_size = 0;
    std::string transport_endpoint;
    YLT_REFL(LocalDiskDescriptor, client_id, object_size, transport_endpoint);
};

struct SsdExtentDescriptor {
    std::string target_endpoint;
    std::string subsystem_nqn;
    uint32_t nsid = 0;
    uint32_t block_size = 4096;
    uint64_t lba_start = 0;
    uint64_t lba_count = 0;
    uint64_t object_size = 0;
    std::string extent_id;
    YLT_REFL(SsdExtentDescriptor, target_endpoint, subsystem_nqn, nsid,
             block_size, lba_start, lba_count, object_size, extent_id);
};

class Replica {
   public:
    struct Descriptor;

    // memory replica constructor
    Replica(std::unique_ptr<AllocatedBuffer> buffer, ReplicaStatus status)
        : id_(next_id_.fetch_add(1)),
          data_(MemoryReplicaData{std::move(buffer)}),
          status_(status),
          refcnt_(0) {}

    // disk replica constructor
    Replica(std::string file_path, uint64_t object_size, ReplicaStatus status)
        : id_(next_id_.fetch_add(1)),
          data_(DiskReplicaData{std::move(file_path), object_size}),
          status_(status),
          refcnt_(0) {
        // Automatic update allocated_file_size via RAII
        MasterMetricManager::instance().inc_allocated_file_size(object_size);
    }

    Replica(UUID client_id, uint64_t object_size,
            std::string transport_endpoint, ReplicaStatus status)
        : id_(next_id_.fetch_add(1)),
          data_(LocalDiskReplicaData{client_id, object_size,
                                     std::move(transport_endpoint)}),
          status_(status),
          refcnt_(0) {}

    // SSD pool replica constructor
    Replica(SsdExtentDescriptor extent, ReplicaStatus status)
        : id_(next_id_.fetch_add(1)),
          data_(SsdExtentReplicaData{
              std::move(extent.target_endpoint), std::move(extent.subsystem_nqn),
              extent.nsid, extent.block_size, extent.lba_start, extent.lba_count,
              extent.object_size,
              std::move(extent.extent_id)}),
          status_(status),
          refcnt_(0) {}

    ~Replica() {
        if (status_ != ReplicaStatus::UNDEFINED && is_disk_replica()) {
            const auto& disk_data = std::get<DiskReplicaData>(data_);
            MasterMetricManager::instance().dec_allocated_file_size(
                disk_data.object_size);
        }
    }

    // Copy-construction is not allowed.
    Replica(const Replica&) = delete;
    Replica& operator=(const Replica&) = delete;

    // Move-construction is allowed.
    Replica(Replica&& src) noexcept
        : id_(src.id_),
          data_(std::move(src.data_)),
          status_(src.status_),
          refcnt_(src.refcnt_.exchange(0)) {
        // Mark the source as moved-from so its destructor doesn't
        // double-decrement metrics.
        src.status_ = ReplicaStatus::UNDEFINED;
    }

    Replica& operator=(Replica&& src) noexcept {
        if (this == &src) {
            // Same object, skip moving.
            return *this;
        }

        // Decrement metric for the current object before overwriting.
        if (status_ != ReplicaStatus::UNDEFINED && is_disk_replica()) {
            const auto& disk_data = std::get<DiskReplicaData>(data_);
            MasterMetricManager::instance().dec_allocated_file_size(
                disk_data.object_size);
        }

        id_ = src.id_;
        data_ = std::move(src.data_);
        status_ = src.status_;
        refcnt_.store(src.refcnt_.exchange(0));
        // Mark src as moved-from.
        src.status_ = ReplicaStatus::UNDEFINED;

        return *this;
    }

    [[nodiscard]] Descriptor get_descriptor() const;

    [[nodiscard]] ReplicaID id() const { return id_; }

    [[nodiscard]] ReplicaStatus status() const { return status_; }

    [[nodiscard]] bool is_completed() const {
        return status_ == ReplicaStatus::COMPLETE;
    }

    [[nodiscard]] static bool fn_is_completed(const Replica& replica) {
        return replica.is_completed();
    }

    [[nodiscard]] bool is_processing() const {
        return status_ == ReplicaStatus::PROCESSING;
    }

    [[nodiscard]] static bool fn_is_processing(const Replica& replica) {
        return replica.is_processing();
    }

    [[nodiscard]] ReplicaType type() const {
        return std::visit(ReplicaTypeVisitor{}, data_);
    }

    [[nodiscard]] bool is_memory_replica() const {
        return std::holds_alternative<MemoryReplicaData>(data_);
    }

    [[nodiscard]] static bool fn_is_memory_replica(const Replica& replica) {
        return replica.is_memory_replica();
    }

    [[nodiscard]] bool is_disk_replica() const {
        return std::holds_alternative<DiskReplicaData>(data_);
    }

    [[nodiscard]] static bool fn_is_disk_replica(const Replica& replica) {
        return replica.is_disk_replica();
    }

    [[nodiscard]] bool is_local_disk_replica() const {
        return std::holds_alternative<LocalDiskReplicaData>(data_);
    }

    [[nodiscard]] static bool fn_is_local_disk_replica(const Replica& replica) {
        return replica.is_local_disk_replica();
    }

    [[nodiscard]] bool is_ssd_pool_replica() const {
        return std::holds_alternative<SsdExtentReplicaData>(data_);
    }

    [[nodiscard]] bool has_invalid_mem_handle() const {
        if (is_memory_replica()) {
            const auto& mem_data = std::get<MemoryReplicaData>(data_);
            return !mem_data.buffer->isAllocatorValid();
        }
        return false;  // DiskReplicaData does not have handles
    }

    [[nodiscard]] size_t get_memory_buffer_size() const {
        if (is_memory_replica()) {
            const auto& mem_data = std::get<MemoryReplicaData>(data_);
            return mem_data.buffer->size();
        } else {
            LOG(ERROR) << "Invalid replica type: " << type();
            return 0;
        }
    }

    [[nodiscard]] std::vector<std::optional<std::string>> get_segment_names()
        const;

    void mark_complete() {
        if (status_ == ReplicaStatus::PROCESSING) {
            status_ = ReplicaStatus::COMPLETE;
        } else if (status_ == ReplicaStatus::COMPLETE) {
            LOG(WARNING) << "Replica already marked as complete";
        } else {
            LOG(ERROR) << "Invalid replica status: " << status_;
        }
    }

    void inc_refcnt() { refcnt_.fetch_add(1); }

    void dec_refcnt() { refcnt_.fetch_sub(1); }

    uint32_t get_refcnt() const { return refcnt_.load(); }

    friend std::ostream& operator<<(std::ostream& os, const Replica& replica);

    struct ReplicaTypeVisitor {
        ReplicaType operator()(const MemoryReplicaData&) const {
            return ReplicaType::MEMORY;
        }
        ReplicaType operator()(const DiskReplicaData&) const {
            return ReplicaType::DISK;
        }
        ReplicaType operator()(const LocalDiskReplicaData&) const {
            return ReplicaType::LOCAL_DISK;
        }
        ReplicaType operator()(const SsdExtentReplicaData&) const {
            return ReplicaType::SSD_POOL;
        }
    };

    struct Descriptor {
        ReplicaID id;
        std::variant<MemoryDescriptor, DiskDescriptor, LocalDiskDescriptor,
                     SsdExtentDescriptor>
            descriptor_variant;
        ReplicaStatus status;
        YLT_REFL(Descriptor, id, descriptor_variant, status);

        // Helper functions
        bool is_memory_replica() noexcept {
            return std::holds_alternative<MemoryDescriptor>(descriptor_variant);
        }

        bool is_memory_replica() const noexcept {
            return std::holds_alternative<MemoryDescriptor>(descriptor_variant);
        }

        bool is_disk_replica() noexcept {
            return std::holds_alternative<DiskDescriptor>(descriptor_variant);
        }

        bool is_disk_replica() const noexcept {
            return std::holds_alternative<DiskDescriptor>(descriptor_variant);
        }

        bool is_local_disk_replica() noexcept {
            return std::holds_alternative<LocalDiskDescriptor>(
                descriptor_variant);
        }

        bool is_local_disk_replica() const noexcept {
            return std::holds_alternative<LocalDiskDescriptor>(
                descriptor_variant);
        }

        bool is_ssd_pool_replica() noexcept {
            return std::holds_alternative<SsdExtentDescriptor>(
                descriptor_variant);
        }

        bool is_ssd_pool_replica() const noexcept {
            return std::holds_alternative<SsdExtentDescriptor>(
                descriptor_variant);
        }

        MemoryDescriptor& get_memory_descriptor() {
            if (auto* desc =
                    std::get_if<MemoryDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected MemoryDescriptor");
        }

        DiskDescriptor& get_disk_descriptor() {
            if (auto* desc = std::get_if<DiskDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected DiskDescriptor");
        }

        LocalDiskDescriptor& get_local_disk_descriptor() {
            if (auto* desc =
                    std::get_if<LocalDiskDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected LocalDiskDescriptor");
        }

        const MemoryDescriptor& get_memory_descriptor() const {
            if (auto* desc =
                    std::get_if<MemoryDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected MemoryDescriptor");
        }

        const DiskDescriptor& get_disk_descriptor() const {
            if (auto* desc = std::get_if<DiskDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected DiskDescriptor");
        }

        const LocalDiskDescriptor& get_local_disk_descriptor() const {
            if (auto* desc =
                    std::get_if<LocalDiskDescriptor>(&descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected LocalDiskDescriptor");
        }

        SsdExtentDescriptor& get_ssd_extent_descriptor() {
            if (auto* desc = std::get_if<SsdExtentDescriptor>(
                    &descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected SsdExtentDescriptor");
        }

        const SsdExtentDescriptor& get_ssd_extent_descriptor() const {
            if (auto* desc = std::get_if<SsdExtentDescriptor>(
                    &descriptor_variant)) {
                return *desc;
            }
            throw std::runtime_error("Expected SsdExtentDescriptor");
        }
    };

   private:
    inline static std::atomic<ReplicaID> next_id_{1};

    ReplicaID id_;
    std::variant<MemoryReplicaData, DiskReplicaData, LocalDiskReplicaData,
                 SsdExtentReplicaData>
        data_;
    ReplicaStatus status_{ReplicaStatus::UNDEFINED};

    friend class Serializer<Replica>;
    friend class MasterService;  // For MetadataSerializer to access next_id_
    std::atomic<uint32_t> refcnt_{0};
};

inline Replica::Descriptor Replica::get_descriptor() const {
    Replica::Descriptor desc;
    desc.id = id_;
    desc.status = status_;

    if (is_memory_replica()) {
        const auto& mem_data = std::get<MemoryReplicaData>(data_);
        MemoryDescriptor mem_desc;
        if (mem_data.buffer) {
            mem_desc.buffer_descriptor = mem_data.buffer->get_descriptor();
        } else {
            mem_desc.buffer_descriptor.size_ = 0;
            mem_desc.buffer_descriptor.buffer_address_ = 0;
            mem_desc.buffer_descriptor.transport_endpoint_ = "";
            LOG(ERROR) << "Trying to get invalid memory replica descriptor";
        }
        desc.descriptor_variant = std::move(mem_desc);
    } else if (is_disk_replica()) {
        const auto& disk_data = std::get<DiskReplicaData>(data_);
        DiskDescriptor disk_desc;
        disk_desc.file_path = disk_data.file_path;
        disk_desc.object_size = disk_data.object_size;
        desc.descriptor_variant = std::move(disk_desc);
    } else if (is_local_disk_replica()) {
        const auto& disk_data = std::get<LocalDiskReplicaData>(data_);
        LocalDiskDescriptor local_disk_desc;
        local_disk_desc.client_id = disk_data.client_id;
        local_disk_desc.object_size = disk_data.object_size;
        local_disk_desc.transport_endpoint = disk_data.transport_endpoint;
        desc.descriptor_variant = std::move(local_disk_desc);
    } else if (is_ssd_pool_replica()) {
        const auto& ssd_data = std::get<SsdExtentReplicaData>(data_);
        SsdExtentDescriptor ssd_desc;
        ssd_desc.target_endpoint = ssd_data.target_endpoint;
        ssd_desc.subsystem_nqn = ssd_data.subsystem_nqn;
        ssd_desc.nsid = ssd_data.nsid;
        ssd_desc.block_size = ssd_data.block_size;
        ssd_desc.lba_start = ssd_data.lba_start;
        ssd_desc.lba_count = ssd_data.lba_count;
        ssd_desc.object_size = ssd_data.object_size;
        ssd_desc.extent_id = ssd_data.extent_id;
        desc.descriptor_variant = std::move(ssd_desc);
    }

    return desc;
}

inline std::vector<std::optional<std::string>> Replica::get_segment_names()
    const {
    if (is_memory_replica()) {
        const auto& mem_data = std::get<MemoryReplicaData>(data_);
        std::vector<std::optional<std::string>> segment_names;
        if (mem_data.buffer && mem_data.buffer->isAllocatorValid()) {
            segment_names.push_back(mem_data.buffer->getSegmentName());
        } else {
            segment_names.push_back(std::nullopt);
        }
        return segment_names;
    }
    if (is_ssd_pool_replica()) {
        const auto& ssd_data = std::get<SsdExtentReplicaData>(data_);
        if (!ssd_data.target_endpoint.empty()) {
            return {ssd_data.target_endpoint};
        }
    }
    return std::vector<std::optional<std::string>>();
}

inline std::ostream& operator<<(std::ostream& os, const Replica& replica) {
    os << "Replica: { id: " << replica.id_ << ", status: " << replica.status_
       << ", ";

    if (replica.is_memory_replica()) {
        const auto& mem_data = std::get<MemoryReplicaData>(replica.data_);
        os << "type: MEMORY, buffers: [";
        if (mem_data.buffer) {
            os << *mem_data.buffer;
        }
        os << "]";
    } else if (replica.is_disk_replica()) {
        const auto& disk_data = std::get<DiskReplicaData>(replica.data_);
        os << "type: DISK, file_path: " << disk_data.file_path
           << ", object_size: " << disk_data.object_size;
    } else if (replica.is_ssd_pool_replica()) {
        const auto& ssd_data = std::get<SsdExtentReplicaData>(replica.data_);
        os << "type: SSD_POOL, target_endpoint: " << ssd_data.target_endpoint
           << ", subsystem_nqn: " << ssd_data.subsystem_nqn
           << ", nsid: " << ssd_data.nsid
           << ", block_size: " << ssd_data.block_size
           << ", lba_start: " << ssd_data.lba_start
           << ", lba_count: " << ssd_data.lba_count
           << ", object_size: " << ssd_data.object_size
           << ", extent_id: " << ssd_data.extent_id;
    } else if (replica.is_local_disk_replica()) {
        const auto& local_disk_data =
            std::get<LocalDiskReplicaData>(replica.data_);
        os << "type: LOCAL_DISK, client_id: " << local_disk_data.client_id
           << ", object_size: " << local_disk_data.object_size
           << ", transport_endpoint: " << local_disk_data.transport_endpoint;
    }

    os << ", refcnt: " << replica.refcnt_.load() << " }";
    return os;
}

}  // namespace mooncake
