#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "replica.h"
#include "types.h"

namespace mooncake {

struct SsdIoTargetConfig {
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

struct SsdIoEngineConfig {
    std::string client_impl{"spdk"};
    int32_t reactor_cores{1};
    uint32_t queue_limit{1024};
    uint32_t io_timeout_ms{5000};
    bool auto_fallback_enabled{true};
};

class SsdIoEngine {
   public:
    static std::unique_ptr<SsdIoEngine> Create(const SsdIoEngineConfig& config);
    ~SsdIoEngine();

    ErrorCode Write(const SsdExtentDescriptor& extent,
                    std::span<const Slice> slices);
    ErrorCode Read(const SsdExtentDescriptor& extent, std::span<Slice> slices);

    const SsdIoEngineConfig& config() const { return config_; }
    const std::string& active_mode() const { return active_mode_; }

   private:
    struct Impl;

    explicit SsdIoEngine(SsdIoEngineConfig config);
    bool Initialize();

    ErrorCode DoWrite(const SsdExtentDescriptor& extent,
                      std::span<const Slice> slices);
    ErrorCode DoRead(const SsdExtentDescriptor& extent, std::span<Slice> slices);
    ErrorCode DoSpdkWrite(const SsdExtentDescriptor& extent,
                          std::span<const Slice> slices);
    ErrorCode DoSpdkRead(const SsdExtentDescriptor& extent,
                         std::span<Slice> slices);
    const SsdIoTargetConfig* FindTargetConfig(const std::string& endpoint,
                                              const std::string& subnqn,
                                              uint32_t nsid) const;
    ErrorCode DoLegacyWrite(const SsdExtentDescriptor& extent,
                            std::span<const Slice> slices);
    ErrorCode DoLegacyRead(const SsdExtentDescriptor& extent,
                           std::span<Slice> slices);
    bool EnsureSpdkSessionForTarget(const SsdIoTargetConfig& target,
                                    std::string& session_key);
    bool EnsureSpdkSessionForExtent(const SsdExtentDescriptor& extent,
                                    std::string& session_key);
    ErrorCode ValidateIoRange(const SsdExtentDescriptor& extent,
                              uint64_t requested_bytes) const;

    static std::string NormalizeEndpointPath(const std::string& endpoint);
    static uint64_t TotalBytes(std::span<const Slice> slices);
    static std::vector<SsdIoTargetConfig> ParseTargetConfigFromEnv();
    static std::string BuildSessionKey(const std::string& endpoint,
                                       const std::string& subnqn, uint32_t nsid);

    SsdIoEngineConfig config_;
    std::string active_mode_;
    std::atomic<uint32_t> inflight_io_{0};
    std::unique_ptr<Impl> impl_;
};

}  // namespace mooncake
