#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "replica.h"
#include "tent/thirdparty/nlohmann/json.h"

namespace mooncake {

class SSDPoolManager {
   public:
    enum class TargetHealth {
        HEALTHY = 0,
        DEGRADED = 1,
        UNAVAILABLE = 2,
    };

    struct SsdTargetConfig {
        std::string name;
        std::string trtype{"tcp"};
        std::string traddr;
        std::string trsvcid{"4420"};
        std::string subnqn;
        uint32_t nsid{1};
        uint64_t capacity_bytes{0};
        uint32_t weight{1};
        std::string target_endpoint;
    };

    struct TargetStatus {
        SsdTargetConfig config;
        TargetHealth health{TargetHealth::UNAVAILABLE};
        uint64_t allocated_bytes{0};
        uint64_t total_capacity_bytes{0};
    };

    struct Usage {
        uint64_t total_bytes{0};
        uint64_t allocated_bytes{0};
        uint64_t free_bytes{0};
        double used_ratio{0.0};
    };

    SSDPoolManager() { InitFromEnv(); }

    bool enabled() const { return enabled_; }

    std::optional<SsdExtentDescriptor> AllocateExtent(const std::string& key,
                                                      uint64_t object_size) {
        if (!enabled_ || object_size == 0) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (targets_.empty()) {
            return std::nullopt;
        }

        const uint64_t required_bytes = AlignUp(object_size, block_size_);
        const uint64_t lba_count = required_bytes / block_size_;
        if (lba_count == 0) {
            return std::nullopt;
        }

        if (auto desc = AllocateByHealth(key, object_size, required_bytes,
                                         lba_count, TargetHealth::HEALTHY);
            desc.has_value()) {
            return desc;
        }
        return AllocateByHealth(key, object_size, required_bytes, lba_count,
                                TargetHealth::DEGRADED);
    }

    bool ReleaseExtent(const std::string& extent_id, const std::string& reason) {
        if (!enabled_ || extent_id.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = extent_records_.find(extent_id);
        if (it == extent_records_.end()) {
            return false;
        }

        const auto record = it->second;
        if (record.target_index >= targets_.size()) {
            extent_records_.erase(it);
            return false;
        }

        auto& target = targets_[record.target_index];
        if (record.lba_count > 0) {
            InsertFreeRange(target, record.lba_start, record.lba_count);
        }
        if (target.allocated_bytes >= record.bytes) {
            target.allocated_bytes -= record.bytes;
        } else {
            target.allocated_bytes = 0;
        }
        extent_records_.erase(it);
        VLOG(1) << "ssd_extent_release extent_id=" << extent_id
                << ", reason=" << reason
                << ", target_endpoint=" << target.config.target_endpoint;
        return true;
    }

    bool ReportExtentIoResult(const std::string& extent_id, bool success) {
        if (!enabled_ || extent_id.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = extent_records_.find(extent_id);
        if (it == extent_records_.end() || it->second.target_index >= targets_.size()) {
            return false;
        }
        UpdateTargetHealthLocked(targets_[it->second.target_index], success);
        return true;
    }

    bool UpdateTargetHealth(const std::string& target_endpoint,
                            TargetHealth health) {
        if (!enabled_ || target_endpoint.empty()) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& target : targets_) {
            if (target.config.target_endpoint == target_endpoint) {
                target.health = health;
                if (health == TargetHealth::HEALTHY) {
                    target.consecutive_io_failures = 0;
                }
                return true;
            }
        }
        return false;
    }

    std::vector<TargetStatus> GetTargetStatuses() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<TargetStatus> statuses;
        statuses.reserve(targets_.size());
        for (const auto& target : targets_) {
            statuses.push_back(TargetStatus{target.config, target.health,
                                            target.allocated_bytes,
                                            target.total_capacity_bytes});
        }
        return statuses;
    }

    Usage GetUsage() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Usage usage;
        for (const auto& target : targets_) {
            usage.total_bytes += target.total_capacity_bytes;
            usage.allocated_bytes += target.allocated_bytes;
        }
        usage.free_bytes =
            (usage.total_bytes >= usage.allocated_bytes)
                ? (usage.total_bytes - usage.allocated_bytes)
                : 0;
        if (usage.total_bytes > 0) {
            usage.used_ratio =
                static_cast<double>(usage.allocated_bytes) /
                static_cast<double>(usage.total_bytes);
        }
        return usage;
    }

    double GetUsedRatio() const { return GetUsage().used_ratio; }

   private:
    struct LbaRange {
        uint64_t start_lba{0};
        uint64_t lba_count{0};
    };

    struct PoolTarget {
        SsdTargetConfig config;
        TargetHealth health{TargetHealth::HEALTHY};
        uint32_t consecutive_io_failures{0};
        uint64_t next_lba{0};
        uint64_t total_capacity_bytes{0};
        uint64_t total_lba_capacity{0};
        uint64_t allocated_bytes{0};
        std::vector<LbaRange> free_ranges;
    };

    struct ExtentRecord {
        size_t target_index{0};
        uint64_t lba_start{0};
        uint64_t lba_count{0};
        uint64_t bytes{0};
    };

    static bool ParseBoolEnv(const char* value, bool default_value) {
        if (value == nullptr) {
            return default_value;
        }
        std::string v(value);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            return true;
        }
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            return false;
        }
        return default_value;
    }

    static uint64_t ParseUint64Env(const char* value, uint64_t default_value) {
        if (value == nullptr) {
            return default_value;
        }
        try {
            return std::stoull(value);
        } catch (...) {
            return default_value;
        }
    }

    static std::vector<std::string> SplitCommaSeparated(
        const std::string& input) {
        std::vector<std::string> values;
        std::stringstream ss(input);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item.erase(item.begin(),
                       std::find_if(item.begin(), item.end(),
                                    [](unsigned char ch) { return !std::isspace(ch); }));
            item.erase(
                std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(),
                item.end());
            if (!item.empty()) {
                values.emplace_back(std::move(item));
            }
        }
        return values;
    }

    static std::string ToLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return std::tolower(ch); });
        return value;
    }

    static uint64_t AlignUp(uint64_t value, uint64_t align) {
        if (align == 0) {
            return value;
        }
        const uint64_t rem = value % align;
        return rem == 0 ? value : (value + align - rem);
    }

    static uint64_t Fnv1a64(std::string_view input) {
        constexpr uint64_t kOffsetBasis = 14695981039346656037ULL;
        constexpr uint64_t kPrime = 1099511628211ULL;
        uint64_t hash = kOffsetBasis;
        for (unsigned char c : input) {
            hash ^= static_cast<uint64_t>(c);
            hash *= kPrime;
        }
        return hash;
    }

    static uint64_t SplitMix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::string BuildExtentId(const std::string& key, size_t target_index,
                              uint64_t lba_start, uint64_t seq) const {
        std::ostringstream oss;
        oss << key << ":" << target_index << ":" << lba_start << ":" << seq;
        return oss.str();
    }

    std::vector<size_t> SelectTargetOrderByHash(
        const std::string& key) const {
        std::vector<size_t> ordered;
        ordered.reserve(targets_.size());
        if (targets_.empty()) {
            return ordered;
        }
        if (hash_ring_.empty()) {
            for (size_t i = 0; i < targets_.size(); ++i) {
                ordered.push_back(i);
            }
            return ordered;
        }

        const uint64_t key_hash = Fnv1a64(key);
        auto it = std::lower_bound(
            hash_ring_.begin(), hash_ring_.end(), key_hash,
            [](const std::pair<uint64_t, size_t>& token_pair, uint64_t hash) {
                return token_pair.first < hash;
            });

        std::vector<bool> visited(targets_.size(), false);
        for (size_t scanned = 0;
             scanned < hash_ring_.size() && ordered.size() < targets_.size();
             ++scanned) {
            if (it == hash_ring_.end()) {
                it = hash_ring_.begin();
            }
            const size_t target_idx = it->second;
            if (target_idx < targets_.size() && !visited[target_idx]) {
                visited[target_idx] = true;
                ordered.push_back(target_idx);
            }
            ++it;
        }
        if (ordered.size() < targets_.size()) {
            for (size_t i = 0; i < targets_.size(); ++i) {
                if (!visited[i]) {
                    ordered.push_back(i);
                }
            }
        }
        return ordered;
    }

    void RebuildHashRing() {
        hash_ring_.clear();
        for (size_t target_idx = 0; target_idx < targets_.size(); ++target_idx) {
            const auto& target = targets_[target_idx];
            const uint32_t clamped_weight =
                std::min<uint32_t>(64, std::max<uint32_t>(1, target.config.weight));
            const uint32_t vnode_count = clamped_weight * 128;
            const uint64_t endpoint_hash =
                Fnv1a64(target.config.target_endpoint);
            for (uint32_t vnode = 0; vnode < vnode_count; ++vnode) {
                const uint64_t vnode_seed =
                    endpoint_hash ^
                    (static_cast<uint64_t>(vnode + 1) *
                     0x9e3779b97f4a7c15ULL);
                hash_ring_.emplace_back(SplitMix64(vnode_seed), target_idx);
            }
        }
        std::sort(hash_ring_.begin(), hash_ring_.end(),
                  [](const std::pair<uint64_t, size_t>& a,
                     const std::pair<uint64_t, size_t>& b) {
                      return (a.first == b.first) ? (a.second < b.second)
                                                  : (a.first < b.first);
                  });
    }

    std::optional<SsdExtentDescriptor> AllocateByHealth(
        const std::string& key, uint64_t object_size, uint64_t required_bytes,
        uint64_t lba_count, TargetHealth health) {
        const auto ordered_targets = SelectTargetOrderByHash(key);
        for (size_t idx : ordered_targets) {
            auto& target = targets_[idx];
            if (target.health != health) {
                continue;
            }

            auto lba_start = AllocateFromTarget(target, lba_count);
            if (!lba_start.has_value()) {
                continue;
            }

            target.allocated_bytes += required_bytes;

            SsdExtentDescriptor desc;
            desc.target_endpoint = target.config.target_endpoint;
            desc.subsystem_nqn = target.config.subnqn;
            desc.nsid = target.config.nsid;
            desc.block_size = block_size_;
            desc.lba_start = *lba_start;
            desc.lba_count = lba_count;
            desc.object_size = object_size;
            desc.extent_id =
                BuildExtentId(key, idx, desc.lba_start, extent_seq_++);
            extent_records_.emplace(desc.extent_id,
                                    ExtentRecord{idx, desc.lba_start, lba_count,
                                                 required_bytes});
            return desc;
        }
        return std::nullopt;
    }

    std::optional<uint64_t> AllocateFromTarget(PoolTarget& target,
                                               uint64_t required_lba_count) {
        if (required_lba_count == 0 || target.total_lba_capacity == 0) {
            return std::nullopt;
        }

        if (auto from_free = AllocateFromFreeList(target, required_lba_count);
            from_free.has_value()) {
            return from_free;
        }

        if (target.next_lba > target.total_lba_capacity ||
            required_lba_count > target.total_lba_capacity - target.next_lba) {
            return std::nullopt;
        }

        const uint64_t lba_start = target.next_lba;
        target.next_lba += required_lba_count;
        return lba_start;
    }

    std::optional<uint64_t> AllocateFromFreeList(PoolTarget& target,
                                                 uint64_t required_lba_count) {
        for (size_t i = 0; i < target.free_ranges.size(); ++i) {
            auto& range = target.free_ranges[i];
            if (range.lba_count < required_lba_count) {
                continue;
            }
            const uint64_t lba_start = range.start_lba;
            range.start_lba += required_lba_count;
            range.lba_count -= required_lba_count;
            if (range.lba_count == 0) {
                target.free_ranges.erase(target.free_ranges.begin() + i);
            }
            return lba_start;
        }
        return std::nullopt;
    }

    static void InsertFreeRange(PoolTarget& target, uint64_t start_lba,
                                uint64_t lba_count) {
        if (lba_count == 0) {
            return;
        }
        target.free_ranges.push_back({start_lba, lba_count});
        std::sort(target.free_ranges.begin(), target.free_ranges.end(),
                  [](const LbaRange& a, const LbaRange& b) {
                      return a.start_lba < b.start_lba;
                  });

        std::vector<LbaRange> merged;
        merged.reserve(target.free_ranges.size());
        for (const auto& range : target.free_ranges) {
            if (merged.empty()) {
                merged.push_back(range);
                continue;
            }
            auto& last = merged.back();
            const uint64_t last_end = last.start_lba + last.lba_count;
            if (range.start_lba <= last_end) {
                const uint64_t range_end = range.start_lba + range.lba_count;
                if (range_end > last_end) {
                    last.lba_count = range_end - last.start_lba;
                }
            } else {
                merged.push_back(range);
            }
        }
        target.free_ranges.swap(merged);
    }

    bool AddTargetFromConfig(SsdTargetConfig config,
                             uint64_t default_capacity_bytes) {
        if (config.target_endpoint.empty()) {
            if (!config.traddr.empty()) {
                config.target_endpoint = config.traddr + ":" + config.trsvcid;
            } else {
                return false;
            }
        }

        if (config.weight == 0) {
            config.weight = 1;
        }
        if (config.capacity_bytes == 0) {
            config.capacity_bytes = default_capacity_bytes;
        }

        PoolTarget target;
        target.config = std::move(config);
        target.health = TargetHealth::HEALTHY;
        target.consecutive_io_failures = 0;
        target.next_lba = 0;
        target.total_capacity_bytes = AlignUp(target.config.capacity_bytes, block_size_);
        target.total_lba_capacity = target.total_capacity_bytes / block_size_;
        target.allocated_bytes = 0;
        targets_.push_back(std::move(target));
        return true;
    }

    bool ParseTargetsFromJson(const std::string& json_text,
                              uint64_t default_capacity_bytes) {
        using nlohmann::json;

        json doc = json::parse(json_text, nullptr, false);
        if (doc.is_discarded() || !doc.is_array()) {
            LOG(ERROR) << "MC_SSD_TARGETS_JSON parse failed or not array";
            return false;
        }

        for (const auto& item : doc) {
            if (!item.is_object()) {
                continue;
            }

            SsdTargetConfig cfg;
            if (item.contains("name") && item["name"].is_string()) {
                cfg.name = item["name"].get<std::string>();
            }
            if (item.contains("trtype") && item["trtype"].is_string()) {
                cfg.trtype = ToLower(item["trtype"].get<std::string>());
            }
            if (cfg.trtype != "tcp") {
                LOG(WARNING) << "MC_SSD_TARGETS_JSON target skipped: trtype="
                             << cfg.trtype << " (V1 only supports tcp)";
                continue;
            }

            if (item.contains("traddr") && item["traddr"].is_string()) {
                cfg.traddr = item["traddr"].get<std::string>();
            }
            if (item.contains("trsvcid") && item["trsvcid"].is_string()) {
                cfg.trsvcid = item["trsvcid"].get<std::string>();
            }
            if (item.contains("subnqn") && item["subnqn"].is_string()) {
                cfg.subnqn = item["subnqn"].get<std::string>();
            }
            if (item.contains("nsid") && item["nsid"].is_number_unsigned()) {
                cfg.nsid = static_cast<uint32_t>(item["nsid"].get<uint64_t>());
            }
            if (item.contains("capacity_bytes") &&
                item["capacity_bytes"].is_number_unsigned()) {
                cfg.capacity_bytes = item["capacity_bytes"].get<uint64_t>();
            }
            if (item.contains("weight") && item["weight"].is_number_unsigned()) {
                cfg.weight = static_cast<uint32_t>(item["weight"].get<uint64_t>());
            }

            if (cfg.traddr.empty() || cfg.trsvcid.empty() || cfg.subnqn.empty()) {
                LOG(WARNING) << "MC_SSD_TARGETS_JSON target skipped due to "
                                "missing traddr/trsvcid/subnqn";
                continue;
            }

            cfg.target_endpoint = cfg.traddr + ":" + cfg.trsvcid;
            if (!AddTargetFromConfig(std::move(cfg), default_capacity_bytes)) {
                LOG(WARNING) << "Failed to add SSD target from JSON";
            }
        }

        return !targets_.empty();
    }

    void UpdateTargetHealthLocked(PoolTarget& target, bool success) {
        if (success) {
            target.consecutive_io_failures = 0;
            target.health = TargetHealth::HEALTHY;
            return;
        }

        target.consecutive_io_failures++;
        if (target.consecutive_io_failures >= 3) {
            target.health = TargetHealth::UNAVAILABLE;
        } else {
            target.health = TargetHealth::DEGRADED;
        }
    }

    void InitFromEnv() {
        enabled_ = ParseBoolEnv(std::getenv("MC_SSD_POOL_ENABLED"), false);
        block_size_ = static_cast<uint32_t>(
            ParseUint64Env(std::getenv("MC_SSD_POOL_BLOCK_SIZE"), 4096));
        if (block_size_ == 0) {
            block_size_ = 4096;
        }

        const uint64_t per_target_capacity_bytes = ParseUint64Env(
            std::getenv("MC_SSD_POOL_CAPACITY_BYTES"), 1ULL << 40);

        if (!enabled_) {
            return;
        }

        const char* targets_json_env = std::getenv("MC_SSD_TARGETS_JSON");
        if (targets_json_env != nullptr && std::strlen(targets_json_env) > 0) {
            if (ParseTargetsFromJson(targets_json_env, per_target_capacity_bytes)) {
                RebuildHashRing();
                LOG(INFO) << "SSDPoolManager initialized from MC_SSD_TARGETS_JSON,"
                          << " targets=" << targets_.size()
                          << ", block_size=" << block_size_;
                return;
            }
            LOG(WARNING) << "MC_SSD_TARGETS_JSON provided but no valid target, "
                            "fallback to legacy MC_SSD_POOL_TARGETS";
            targets_.clear();
        }

        const char* targets_env = std::getenv("MC_SSD_POOL_TARGETS");
        if (targets_env == nullptr) {
            LOG(WARNING) << "MC_SSD_POOL_ENABLED=true but target config is empty. "
                            "Disable SSD pool for this process.";
            enabled_ = false;
            return;
        }

        const auto target_endpoints = SplitCommaSeparated(targets_env);
        if (target_endpoints.empty()) {
            LOG(WARNING) << "MC_SSD_POOL_TARGETS has no valid endpoint. Disable "
                            "SSD pool for this process.";
            enabled_ = false;
            return;
        }

        const char* nqn_env = std::getenv("MC_SSD_POOL_SUBSYSTEM_NQN");
        std::string subsystem_nqn =
            (nqn_env == nullptr) ? "nqn.2026-03.io.mooncake:ssdpool"
                                 : std::string(nqn_env);
        const uint32_t nsid = static_cast<uint32_t>(
            ParseUint64Env(std::getenv("MC_SSD_POOL_NSID"), 1));

        for (const auto& endpoint : target_endpoints) {
            SsdTargetConfig cfg;
            cfg.name = endpoint;
            cfg.target_endpoint = endpoint;
            cfg.subnqn = subsystem_nqn;
            cfg.nsid = nsid;
            cfg.capacity_bytes = per_target_capacity_bytes;
            cfg.weight = 1;

            const auto colon_pos = endpoint.find(':');
            if (endpoint.rfind("file://", 0) == 0 ||
                (!endpoint.empty() && endpoint[0] == '/')) {
                cfg.trtype = "legacy";
            } else if (colon_pos != std::string::npos) {
                cfg.trtype = "tcp";
                cfg.traddr = endpoint.substr(0, colon_pos);
                cfg.trsvcid = endpoint.substr(colon_pos + 1);
            } else {
                cfg.trtype = "tcp";
                cfg.traddr = endpoint;
                cfg.trsvcid = "4420";
            }

            if (!AddTargetFromConfig(std::move(cfg), per_target_capacity_bytes)) {
                LOG(WARNING) << "Failed to add legacy SSD target endpoint="
                             << endpoint;
            }
        }

        if (targets_.empty()) {
            LOG(WARNING) << "No SSD target initialized. Disable SSD pool.";
            enabled_ = false;
            return;
        }

        RebuildHashRing();
        LOG(INFO) << "SSDPoolManager initialized, targets=" << targets_.size()
                  << ", block_size=" << block_size_
                  << ", per_target_capacity_bytes=" << per_target_capacity_bytes;
    }

    bool enabled_{false};
    uint32_t block_size_{4096};
    uint64_t extent_seq_{0};
    std::vector<PoolTarget> targets_;
    std::vector<std::pair<uint64_t, size_t>> hash_ring_;
    std::unordered_map<std::string, ExtentRecord> extent_records_;
    mutable std::mutex mutex_;
};

}  // namespace mooncake
