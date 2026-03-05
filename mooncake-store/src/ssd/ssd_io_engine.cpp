#include "ssd/ssd_io_engine.h"

#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <glog/logging.h>

#include "master_metric_manager.h"
#include "tent/thirdparty/nlohmann/json.h"

#if defined(MOONCAKE_STORE_USE_SPDK) && MOONCAKE_STORE_USE_SPDK
#include "spdk/env.h"
#include "spdk/nvme.h"
#endif

namespace mooncake {

namespace {

class InflightGuard {
   public:
    explicit InflightGuard(std::atomic<uint32_t>& inflight) : inflight_(inflight) {}
    ~InflightGuard() { inflight_.fetch_sub(1, std::memory_order_relaxed); }

   private:
    std::atomic<uint32_t>& inflight_;
};

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    return value;
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    if (align == 0) {
        return value;
    }
    const uint64_t rem = value % align;
    return rem == 0 ? value : (value + align - rem);
}

std::vector<std::string> SplitCommaSeparated(const std::string& input) {
    std::vector<std::string> values;
    std::string item;
    for (char ch : input) {
        if (ch == ',') {
            if (!item.empty()) {
                values.push_back(item);
                item.clear();
            }
            continue;
        }
        item.push_back(ch);
    }
    if (!item.empty()) {
        values.push_back(item);
    }
    return values;
}

bool SplitEndpoint(const std::string& endpoint, std::string& host,
                   std::string& port) {
    const auto pos = endpoint.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= endpoint.size()) {
        return false;
    }
    host = endpoint.substr(0, pos);
    port = endpoint.substr(pos + 1);
    return !host.empty() && !port.empty();
}

void CopySlicesToBuffer(std::span<const Slice> slices, char* buffer,
                        size_t bytes) {
    size_t copied = 0;
    for (const auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0 || copied >= bytes) {
            continue;
        }
        const size_t to_copy = std::min(slice.size, bytes - copied);
        std::memcpy(buffer + copied, slice.ptr, to_copy);
        copied += to_copy;
    }
}

void CopyBufferToSlices(const char* buffer, size_t bytes, std::span<Slice> slices) {
    size_t copied = 0;
    for (auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0 || copied >= bytes) {
            continue;
        }
        const size_t to_copy = std::min(slice.size, bytes - copied);
        std::memcpy(slice.ptr, buffer + copied, to_copy);
        copied += to_copy;
    }
}

void ZeroSlicesTail(std::span<Slice> slices, size_t offset) {
    size_t cursor = 0;
    for (auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0) {
            continue;
        }
        if (offset >= cursor + slice.size) {
            cursor += slice.size;
            continue;
        }
        const size_t start = offset > cursor ? (offset - cursor) : 0;
        std::memset(static_cast<char*>(slice.ptr) + start, 0, slice.size - start);
        cursor += slice.size;
    }
}

bool EnsureSpdkSockPosixLoaded() {
    static std::once_flag once;
    static bool loaded = false;
    std::call_once(once, [] {
        void* handle = dlopen("libspdk_sock_posix.so", RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            handle = dlopen("libspdk_sock_posix.so.4.0", RTLD_NOW | RTLD_GLOBAL);
        }
        loaded = (handle != nullptr);
        if (!loaded) {
            const char* err = dlerror();
            LOG(ERROR) << "Failed to load libspdk_sock_posix.so: "
                       << (err == nullptr ? "unknown error" : err);
        }
    });
    return loaded;
}

#if defined(MOONCAKE_STORE_USE_SPDK) && MOONCAKE_STORE_USE_SPDK
struct SpdkIoCompletion {
    std::atomic<bool> done{false};
    bool success{false};
};

void SpdkIoCompletionCb(void* arg, const spdk_nvme_cpl* cpl) {
    auto* ctx = static_cast<SpdkIoCompletion*>(arg);
    ctx->success = spdk_nvme_cpl_is_success(cpl);
    ctx->done.store(true, std::memory_order_release);
}

ErrorCode WaitSpdkCompletion(spdk_nvme_qpair* qpair, SpdkIoCompletion& completion,
                             uint32_t io_timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(io_timeout_ms == 0 ? 5000 : io_timeout_ms);

    while (!completion.done.load(std::memory_order_acquire)) {
        const int rc = spdk_nvme_qpair_process_completions(qpair, 0);
        if (rc < 0) {
            return ErrorCode::TRANSFER_FAIL;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return ErrorCode::TRANSFER_FAIL;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    return completion.success ? ErrorCode::OK : ErrorCode::TRANSFER_FAIL;
}
#endif

}  // namespace

struct SsdIoEngine::Impl {
#if defined(MOONCAKE_STORE_USE_SPDK) && MOONCAKE_STORE_USE_SPDK
    struct SpdkSession {
        std::string session_key;
        std::string target_endpoint;
        std::string subnqn;
        uint32_t nsid{1};
        spdk_nvme_ctrlr* ctrlr{nullptr};
        spdk_nvme_ns* ns{nullptr};
        spdk_nvme_qpair* qpair{nullptr};
        uint32_t sector_size{4096};
        std::mutex io_mutex;
    };

    bool spdk_env_ready{false};
    std::unordered_map<std::string, std::unique_ptr<SpdkSession>> sessions;
#endif
    std::vector<SsdIoTargetConfig> target_configs;
    std::mutex mutex;
};

std::unique_ptr<SsdIoEngine> SsdIoEngine::Create(
    const SsdIoEngineConfig& config) {
    auto engine = std::unique_ptr<SsdIoEngine>(new SsdIoEngine(config));
    if (!engine->Initialize()) {
        return nullptr;
    }
    return engine;
}

SsdIoEngine::SsdIoEngine(SsdIoEngineConfig config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>()) {
    std::transform(config_.client_impl.begin(), config_.client_impl.end(),
                   config_.client_impl.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    if (config_.client_impl != "spdk" && config_.client_impl != "legacy") {
        config_.client_impl = "spdk";
    }
    if (config_.reactor_cores < 1) {
        config_.reactor_cores = 1;
    } else if (config_.reactor_cores > 6) {
        config_.reactor_cores = 6;
    }
    if (config_.queue_limit == 0) {
        config_.queue_limit = 1024;
    }
    if (config_.io_timeout_ms == 0) {
        config_.io_timeout_ms = 5000;
    }
    active_mode_ = config_.client_impl;
}

SsdIoEngine::~SsdIoEngine() {
#if defined(MOONCAKE_STORE_USE_SPDK) && MOONCAKE_STORE_USE_SPDK
    if (!impl_ || !impl_->spdk_env_ready) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& [_, session] : impl_->sessions) {
        if (!session) {
            continue;
        }
        if (session->qpair != nullptr) {
            spdk_nvme_ctrlr_free_io_qpair(session->qpair);
            session->qpair = nullptr;
        }
        if (session->ctrlr != nullptr) {
            spdk_nvme_detach(session->ctrlr);
            session->ctrlr = nullptr;
        }
    }
    impl_->sessions.clear();
    spdk_env_fini();
    impl_->spdk_env_ready = false;
#endif
}

bool SsdIoEngine::Initialize() {
    impl_->target_configs = ParseTargetConfigFromEnv();

    if (active_mode_ == "legacy") {
        LOG(INFO) << "SsdIoEngine initialized in legacy mode";
        return true;
    }

    if (active_mode_ != "spdk") {
        LOG(ERROR) << "Unsupported SsdIoEngine mode: " << active_mode_;
        return false;
    }

#if !defined(MOONCAKE_STORE_USE_SPDK) || !MOONCAKE_STORE_USE_SPDK
    LOG(ERROR) << "nvmeof_client_impl=spdk requires build option "
                  "STORE_USE_SPDK=ON (fail-fast)";
    return false;
#else
    if (impl_->target_configs.empty()) {
        LOG(ERROR) << "SPDK mode requires MC_SSD_TARGETS_JSON (or legacy "
                      "fallback env) to configure NVMeoF targets";
        return false;
    }

    if (!EnsureSpdkSockPosixLoaded()) {
        return false;
    }

    spdk_env_opts opts;
    spdk_env_opts_init(&opts);
    opts.name = "mooncake-store-spdk";
    opts.mem_channel = std::max(1, config_.reactor_cores);
    if (spdk_env_init(&opts) < 0) {
        LOG(ERROR) << "spdk_env_init failed";
        return false;
    }
    impl_->spdk_env_ready = true;

    bool has_healthy_target = false;
    for (const auto& target : impl_->target_configs) {
        if (ToLower(target.trtype) != "tcp") {
            continue;
        }
        std::string session_key;
        if (EnsureSpdkSessionForTarget(target, session_key)) {
            MasterMetricManager::instance().set_ssd_target_health(
                target.target_endpoint, 1.0);
            has_healthy_target = true;
        } else {
            MasterMetricManager::instance().set_ssd_target_health(
                target.target_endpoint, 0.0);
            MasterMetricManager::instance().inc_ssd_connect_fail_total();
        }
    }

    if (!has_healthy_target) {
        LOG(ERROR) << "All configured SPDK NVMeoF(TCP) targets are unavailable "
                      "(fail-fast)";
        return false;
    }

    LOG(INFO) << "SsdIoEngine initialized, mode=spdk, reactor_cores="
              << config_.reactor_cores
              << ", queue_limit=" << config_.queue_limit
              << ", io_timeout_ms=" << config_.io_timeout_ms
              << ", target_count=" << impl_->target_configs.size();
    return true;
#endif
}

ErrorCode SsdIoEngine::Write(const SsdExtentDescriptor& extent,
                             std::span<const Slice> slices) {
    const auto current_inflight =
        inflight_io_.fetch_add(1, std::memory_order_relaxed);
    if (current_inflight >= config_.queue_limit) {
        inflight_io_.fetch_sub(1, std::memory_order_relaxed);
        return ErrorCode::TRANSFER_FAIL;
    }
    InflightGuard guard(inflight_io_);
    return DoWrite(extent, slices);
}

ErrorCode SsdIoEngine::Read(const SsdExtentDescriptor& extent,
                            std::span<Slice> slices) {
    const auto current_inflight =
        inflight_io_.fetch_add(1, std::memory_order_relaxed);
    if (current_inflight >= config_.queue_limit) {
        inflight_io_.fetch_sub(1, std::memory_order_relaxed);
        return ErrorCode::TRANSFER_FAIL;
    }
    InflightGuard guard(inflight_io_);
    return DoRead(extent, slices);
}

ErrorCode SsdIoEngine::DoWrite(const SsdExtentDescriptor& extent,
                               std::span<const Slice> slices) {
    if (active_mode_ == "legacy") {
        return DoLegacyWrite(extent, slices);
    }
    if (active_mode_ == "spdk") {
        return DoSpdkWrite(extent, slices);
    }
    return ErrorCode::TRANSFER_FAIL;
}

ErrorCode SsdIoEngine::DoRead(const SsdExtentDescriptor& extent,
                              std::span<Slice> slices) {
    if (active_mode_ == "legacy") {
        return DoLegacyRead(extent, slices);
    }
    if (active_mode_ == "spdk") {
        return DoSpdkRead(extent, slices);
    }
    return ErrorCode::TRANSFER_FAIL;
}

ErrorCode SsdIoEngine::DoLegacyWrite(const SsdExtentDescriptor& extent,
                                     std::span<const Slice> slices) {
    const uint64_t requested_bytes = TotalBytes(slices);
    const uint64_t object_bytes =
        extent.object_size == 0 ? requested_bytes : extent.object_size;
    if (requested_bytes < object_bytes) {
        return ErrorCode::INVALID_PARAMS;
    }
    auto range_err = ValidateIoRange(extent, object_bytes);
    if (range_err != ErrorCode::OK) {
        return range_err;
    }

    const std::string path = NormalizeEndpointPath(extent.target_endpoint);
    if (path.empty()) {
        LOG(ERROR) << "Legacy SSD write failed: unsupported endpoint="
                   << extent.target_endpoint;
        return ErrorCode::TRANSFER_FAIL;
    }

    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        LOG(ERROR) << "Legacy SSD write open failed, path=" << path
                   << ", errno=" << errno;
        return ErrorCode::TRANSFER_FAIL;
    }

    uint64_t remaining = object_bytes;
    uint64_t offset =
        extent.lba_start * static_cast<uint64_t>(extent.block_size);
    for (const auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0 || remaining == 0) {
            continue;
        }
        const size_t write_size =
            static_cast<size_t>(std::min<uint64_t>(slice.size, remaining));
        const ssize_t rc =
            ::pwrite(fd, slice.ptr, write_size, static_cast<off_t>(offset));
        if (rc < 0 || static_cast<size_t>(rc) != write_size) {
            LOG(ERROR) << "Legacy SSD write failed, path=" << path
                       << ", offset=" << offset << ", size=" << write_size
                       << ", rc=" << rc << ", errno=" << errno;
            ::close(fd);
            return ErrorCode::TRANSFER_FAIL;
        }
        offset += write_size;
        remaining -= write_size;
    }

    ::close(fd);
    return remaining == 0 ? ErrorCode::OK : ErrorCode::INVALID_PARAMS;
}

ErrorCode SsdIoEngine::DoLegacyRead(const SsdExtentDescriptor& extent,
                                    std::span<Slice> slices) {
    const uint64_t requested_bytes = TotalBytes(slices);
    const uint64_t object_bytes =
        extent.object_size == 0 ? requested_bytes : extent.object_size;
    const uint64_t copy_bytes = std::min(requested_bytes, object_bytes);
    auto range_err = ValidateIoRange(extent, object_bytes);
    if (range_err != ErrorCode::OK) {
        return range_err;
    }

    const std::string path = NormalizeEndpointPath(extent.target_endpoint);
    if (path.empty()) {
        LOG(ERROR) << "Legacy SSD read failed: unsupported endpoint="
                   << extent.target_endpoint;
        return ErrorCode::TRANSFER_FAIL;
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG(ERROR) << "Legacy SSD read open failed, path=" << path
                   << ", errno=" << errno;
        return ErrorCode::TRANSFER_FAIL;
    }

    uint64_t remaining = copy_bytes;
    uint64_t offset =
        extent.lba_start * static_cast<uint64_t>(extent.block_size);
    for (auto& slice : slices) {
        if (slice.ptr == nullptr || slice.size == 0 || remaining == 0) {
            continue;
        }
        const size_t read_size =
            static_cast<size_t>(std::min<uint64_t>(slice.size, remaining));
        const ssize_t rc =
            ::pread(fd, slice.ptr, read_size, static_cast<off_t>(offset));
        if (rc < 0 || static_cast<size_t>(rc) != read_size) {
            LOG(ERROR) << "Legacy SSD read failed, path=" << path
                       << ", offset=" << offset << ", size=" << read_size
                       << ", rc=" << rc << ", errno=" << errno;
            ::close(fd);
            return ErrorCode::TRANSFER_FAIL;
        }
        offset += read_size;
        remaining -= read_size;
    }

    ::close(fd);
    if (remaining != 0) {
        return ErrorCode::TRANSFER_FAIL;
    }
    if (requested_bytes > copy_bytes) {
        ZeroSlicesTail(slices, static_cast<size_t>(copy_bytes));
    }
    return ErrorCode::OK;
}

bool SsdIoEngine::EnsureSpdkSessionForTarget(const SsdIoTargetConfig& target,
                                             std::string& session_key) {
    session_key = BuildSessionKey(target.target_endpoint, target.subnqn, target.nsid);

#if !defined(MOONCAKE_STORE_USE_SPDK) || !MOONCAKE_STORE_USE_SPDK
    (void)target;
    return false;
#else
    if (ToLower(target.trtype) != "tcp") {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->sessions.find(session_key) != impl_->sessions.end()) {
            return true;
        }
    }

    spdk_nvme_transport_id trid = {};
    spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_TCP);
    const bool is_ipv6 = target.traddr.find(':') != std::string::npos &&
                         target.traddr.find('.') == std::string::npos;
    trid.adrfam = is_ipv6 ? SPDK_NVMF_ADRFAM_IPV6 : SPDK_NVMF_ADRFAM_IPV4;
    std::snprintf(trid.traddr, sizeof(trid.traddr), "%s", target.traddr.c_str());
    std::snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s",
                  target.trsvcid.c_str());
    std::snprintf(trid.subnqn, sizeof(trid.subnqn), "%s",
                  target.subnqn.c_str());

    spdk_nvme_ctrlr_opts ctrlr_opts = {};
    spdk_nvme_ctrlr_get_default_ctrlr_opts(&ctrlr_opts, sizeof(ctrlr_opts));

    spdk_nvme_ctrlr* ctrlr =
        spdk_nvme_connect(&trid, &ctrlr_opts, sizeof(ctrlr_opts));
    if (ctrlr == nullptr) {
        LOG(ERROR) << "spdk_nvme_connect failed, target=" << target.target_endpoint
                   << ", traddr=" << trid.traddr << ", trsvcid=" << trid.trsvcid
                   << ", adrfam=" << static_cast<int>(trid.adrfam)
                   << ", subnqn=" << target.subnqn << ", nsid=" << target.nsid;
        return false;
    }

    spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, target.nsid);
    if (ns == nullptr || !spdk_nvme_ns_is_active(ns)) {
        LOG(ERROR) << "spdk namespace unavailable, target=" << target.target_endpoint
                   << ", nsid=" << target.nsid;
        spdk_nvme_detach(ctrlr);
        return false;
    }

    spdk_nvme_io_qpair_opts qpair_opts = {};
    spdk_nvme_ctrlr_get_default_io_qpair_opts(ctrlr, &qpair_opts,
                                               sizeof(qpair_opts));
    qpair_opts.io_queue_size =
        std::min<uint32_t>(qpair_opts.io_queue_size, config_.queue_limit);
    spdk_nvme_qpair* qpair =
        spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, &qpair_opts, sizeof(qpair_opts));
    if (qpair == nullptr) {
        LOG(ERROR) << "spdk alloc io qpair failed, target=" << target.target_endpoint;
        spdk_nvme_detach(ctrlr);
        return false;
    }

    auto session = std::make_unique<Impl::SpdkSession>();
    session->session_key = session_key;
    session->target_endpoint = target.target_endpoint;
    session->subnqn = target.subnqn;
    session->nsid = target.nsid;
    session->ctrlr = ctrlr;
    session->ns = ns;
    session->qpair = qpair;
    session->sector_size = spdk_nvme_ns_get_sector_size(ns);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto [it, inserted] =
            impl_->sessions.emplace(session_key, std::move(session));
        if (!inserted) {
            if (qpair != nullptr) {
                spdk_nvme_ctrlr_free_io_qpair(qpair);
            }
            spdk_nvme_detach(ctrlr);
        }
    }
    return true;
#endif
}

bool SsdIoEngine::EnsureSpdkSessionForExtent(const SsdExtentDescriptor& extent,
                                             std::string& session_key) {
#if !defined(MOONCAKE_STORE_USE_SPDK) || !MOONCAKE_STORE_USE_SPDK
    (void)extent;
    session_key.clear();
    return false;
#else
    session_key =
        BuildSessionKey(extent.target_endpoint, extent.subsystem_nqn, extent.nsid);

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->sessions.find(session_key) != impl_->sessions.end()) {
            return true;
        }
    }

    SsdIoTargetConfig target;
    if (const auto* cfg =
            FindTargetConfig(extent.target_endpoint, extent.subsystem_nqn,
                             extent.nsid);
        cfg != nullptr) {
        target = *cfg;
    } else {
        std::string traddr;
        std::string trsvcid;
        if (!SplitEndpoint(extent.target_endpoint, traddr, trsvcid)) {
            return false;
        }
        target.trtype = "tcp";
        target.traddr = traddr;
        target.trsvcid = trsvcid;
        target.subnqn = extent.subsystem_nqn;
        target.nsid = extent.nsid;
        target.target_endpoint = extent.target_endpoint;
    }

    const bool ok = EnsureSpdkSessionForTarget(target, session_key);
    MasterMetricManager::instance().set_ssd_target_health(target.target_endpoint,
                                                          ok ? 1.0 : 0.0);
    if (!ok) {
        MasterMetricManager::instance().inc_ssd_connect_fail_total();
    }
    return ok;
#endif
}

ErrorCode SsdIoEngine::DoSpdkWrite(const SsdExtentDescriptor& extent,
                                   std::span<const Slice> slices) {
#if !defined(MOONCAKE_STORE_USE_SPDK) || !MOONCAKE_STORE_USE_SPDK
    (void)extent;
    (void)slices;
    return ErrorCode::TRANSFER_FAIL;
#else
    const uint64_t requested_bytes = TotalBytes(slices);
    const uint64_t object_bytes =
        extent.object_size == 0 ? requested_bytes : extent.object_size;
    if (requested_bytes < object_bytes || object_bytes == 0) {
        return ErrorCode::INVALID_PARAMS;
    }

    const uint64_t aligned_bytes = AlignUp(object_bytes, extent.block_size);
    auto range_err = ValidateIoRange(extent, aligned_bytes);
    if (range_err != ErrorCode::OK) {
        return range_err;
    }

    std::string session_key;
    if (!EnsureSpdkSessionForExtent(extent, session_key)) {
        return ErrorCode::TRANSFER_FAIL;
    }

    Impl::SpdkSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->sessions.find(session_key);
        if (it == impl_->sessions.end()) {
            return ErrorCode::TRANSFER_FAIL;
        }
        session = it->second.get();
    }

    if (session->sector_size != extent.block_size) {
        LOG(ERROR) << "SSD block size mismatch, descriptor=" << extent.block_size
                   << ", namespace=" << session->sector_size;
        return ErrorCode::INVALID_PARAMS;
    }

    const uint64_t lba_count_u64 = aligned_bytes / extent.block_size;
    if (lba_count_u64 > std::numeric_limits<uint32_t>::max()) {
        return ErrorCode::INVALID_PARAMS;
    }
    const uint32_t lba_count = static_cast<uint32_t>(lba_count_u64);

    void* dma_buf = spdk_dma_malloc(aligned_bytes, extent.block_size, nullptr);
    if (dma_buf == nullptr) {
        return ErrorCode::TRANSFER_FAIL;
    }

    std::lock_guard<std::mutex> io_lock(session->io_mutex);
    if (aligned_bytes > object_bytes) {
        SpdkIoCompletion rmw_read_completion;
        MasterMetricManager::instance().inc_ssd_spdk_io_submit_total();
        const int read_rc = spdk_nvme_ns_cmd_read(
            session->ns, session->qpair, dma_buf, extent.lba_start, lba_count,
            SpdkIoCompletionCb, &rmw_read_completion, 0);
        if (read_rc != 0) {
            spdk_dma_free(dma_buf);
            MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
            return ErrorCode::TRANSFER_FAIL;
        }
        const auto wait_rc = WaitSpdkCompletion(
            session->qpair, rmw_read_completion, config_.io_timeout_ms);
        if (wait_rc != ErrorCode::OK) {
            spdk_dma_free(dma_buf);
            MasterMetricManager::instance().inc_ssd_spdk_io_timeout_total();
            MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
            return wait_rc;
        }
    }

    CopySlicesToBuffer(slices, static_cast<char*>(dma_buf),
                       static_cast<size_t>(object_bytes));

    SpdkIoCompletion write_completion;
    MasterMetricManager::instance().inc_ssd_spdk_io_submit_total();
    const int write_rc = spdk_nvme_ns_cmd_write(
        session->ns, session->qpair, dma_buf, extent.lba_start, lba_count,
        SpdkIoCompletionCb, &write_completion, 0);
    if (write_rc != 0) {
        spdk_dma_free(dma_buf);
        MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
        return ErrorCode::TRANSFER_FAIL;
    }

    const auto wait_rc =
        WaitSpdkCompletion(session->qpair, write_completion, config_.io_timeout_ms);
    spdk_dma_free(dma_buf);
    if (wait_rc != ErrorCode::OK) {
        MasterMetricManager::instance().inc_ssd_spdk_io_timeout_total();
        MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
        return wait_rc;
    }
    return ErrorCode::OK;
#endif
}

ErrorCode SsdIoEngine::DoSpdkRead(const SsdExtentDescriptor& extent,
                                  std::span<Slice> slices) {
#if !defined(MOONCAKE_STORE_USE_SPDK) || !MOONCAKE_STORE_USE_SPDK
    (void)extent;
    (void)slices;
    return ErrorCode::TRANSFER_FAIL;
#else
    const uint64_t requested_bytes = TotalBytes(slices);
    const uint64_t object_bytes =
        extent.object_size == 0 ? requested_bytes : extent.object_size;
    if (object_bytes == 0) {
        return ErrorCode::OK;
    }

    const uint64_t aligned_bytes = AlignUp(object_bytes, extent.block_size);
    auto range_err = ValidateIoRange(extent, aligned_bytes);
    if (range_err != ErrorCode::OK) {
        return range_err;
    }

    std::string session_key;
    if (!EnsureSpdkSessionForExtent(extent, session_key)) {
        return ErrorCode::TRANSFER_FAIL;
    }

    Impl::SpdkSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->sessions.find(session_key);
        if (it == impl_->sessions.end()) {
            return ErrorCode::TRANSFER_FAIL;
        }
        session = it->second.get();
    }

    if (session->sector_size != extent.block_size) {
        LOG(ERROR) << "SSD block size mismatch, descriptor=" << extent.block_size
                   << ", namespace=" << session->sector_size;
        return ErrorCode::INVALID_PARAMS;
    }

    const uint64_t lba_count_u64 = aligned_bytes / extent.block_size;
    if (lba_count_u64 > std::numeric_limits<uint32_t>::max()) {
        return ErrorCode::INVALID_PARAMS;
    }
    const uint32_t lba_count = static_cast<uint32_t>(lba_count_u64);

    void* dma_buf = spdk_dma_malloc(aligned_bytes, extent.block_size, nullptr);
    if (dma_buf == nullptr) {
        return ErrorCode::TRANSFER_FAIL;
    }

    std::lock_guard<std::mutex> io_lock(session->io_mutex);
    SpdkIoCompletion read_completion;
    MasterMetricManager::instance().inc_ssd_spdk_io_submit_total();
    const int read_rc = spdk_nvme_ns_cmd_read(
        session->ns, session->qpair, dma_buf, extent.lba_start, lba_count,
        SpdkIoCompletionCb, &read_completion, 0);
    if (read_rc != 0) {
        spdk_dma_free(dma_buf);
        MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
        return ErrorCode::TRANSFER_FAIL;
    }

    const auto wait_rc =
        WaitSpdkCompletion(session->qpair, read_completion, config_.io_timeout_ms);
    if (wait_rc != ErrorCode::OK) {
        spdk_dma_free(dma_buf);
        MasterMetricManager::instance().inc_ssd_spdk_io_timeout_total();
        MasterMetricManager::instance().inc_ssd_spdk_io_fail_total();
        return wait_rc;
    }

    const uint64_t copy_bytes = std::min(requested_bytes, object_bytes);
    CopyBufferToSlices(static_cast<const char*>(dma_buf),
                       static_cast<size_t>(copy_bytes), slices);
    spdk_dma_free(dma_buf);

    if (requested_bytes > copy_bytes) {
        ZeroSlicesTail(slices, static_cast<size_t>(copy_bytes));
    }
    return ErrorCode::OK;
#endif
}

ErrorCode SsdIoEngine::ValidateIoRange(const SsdExtentDescriptor& extent,
                                       uint64_t requested_bytes) const {
    if (extent.block_size == 0 || extent.lba_count == 0) {
        return ErrorCode::INVALID_PARAMS;
    }
    const uint64_t extent_bytes =
        extent.lba_count * static_cast<uint64_t>(extent.block_size);
    if (requested_bytes > extent_bytes) {
        return ErrorCode::INVALID_PARAMS;
    }
    return ErrorCode::OK;
}

std::string SsdIoEngine::NormalizeEndpointPath(const std::string& endpoint) {
    constexpr const char* kFilePrefix = "file://";
    if (endpoint.rfind(kFilePrefix, 0) == 0) {
        return endpoint.substr(std::strlen(kFilePrefix));
    }
    if (!endpoint.empty() && endpoint[0] == '/') {
        return endpoint;
    }
    return {};
}

uint64_t SsdIoEngine::TotalBytes(std::span<const Slice> slices) {
    uint64_t total = 0;
    for (const auto& slice : slices) {
        total += slice.size;
    }
    return total;
}

std::vector<SsdIoTargetConfig> SsdIoEngine::ParseTargetConfigFromEnv() {
    std::vector<SsdIoTargetConfig> targets;

    const char* targets_json_env = std::getenv("MC_SSD_TARGETS_JSON");
    if (targets_json_env != nullptr && std::strlen(targets_json_env) > 0) {
        using nlohmann::json;
        json doc = json::parse(targets_json_env, nullptr, false);
        if (doc.is_array()) {
            for (const auto& item : doc) {
                if (!item.is_object()) {
                    continue;
                }
                SsdIoTargetConfig cfg;
                if (item.contains("name") && item["name"].is_string()) {
                    cfg.name = item["name"].get<std::string>();
                }
                if (item.contains("trtype") && item["trtype"].is_string()) {
                    cfg.trtype = ToLower(item["trtype"].get<std::string>());
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
                if (item.contains("weight") &&
                    item["weight"].is_number_unsigned()) {
                    cfg.weight = static_cast<uint32_t>(item["weight"].get<uint64_t>());
                }

                if (cfg.traddr.empty() || cfg.trsvcid.empty() || cfg.subnqn.empty()) {
                    continue;
                }
                cfg.target_endpoint = cfg.traddr + ":" + cfg.trsvcid;
                targets.emplace_back(std::move(cfg));
            }
        }
    }

    if (!targets.empty()) {
        return targets;
    }

    const char* fallback_targets = std::getenv("MC_SSD_POOL_TARGETS");
    if (fallback_targets == nullptr || std::strlen(fallback_targets) == 0) {
        return targets;
    }

    const char* nqn_env = std::getenv("MC_SSD_POOL_SUBSYSTEM_NQN");
    const std::string nqn = (nqn_env == nullptr)
                                ? "nqn.2026-03.io.mooncake:ssdpool"
                                : std::string(nqn_env);
    const uint32_t nsid = static_cast<uint32_t>(
        std::strtoul(std::getenv("MC_SSD_POOL_NSID") == nullptr
                         ? "1"
                         : std::getenv("MC_SSD_POOL_NSID"),
                     nullptr, 10));

    for (const auto& endpoint : SplitCommaSeparated(fallback_targets)) {
        if (endpoint.empty()) {
            continue;
        }
        SsdIoTargetConfig cfg;
        cfg.name = endpoint;
        cfg.subnqn = nqn;
        cfg.nsid = nsid == 0 ? 1 : nsid;
        cfg.target_endpoint = endpoint;

        if (endpoint.rfind("file://", 0) == 0 ||
            (!endpoint.empty() && endpoint[0] == '/')) {
            cfg.trtype = "legacy";
        } else {
            std::string traddr;
            std::string trsvcid;
            if (!SplitEndpoint(endpoint, traddr, trsvcid)) {
                continue;
            }
            cfg.trtype = "tcp";
            cfg.traddr = traddr;
            cfg.trsvcid = trsvcid;
        }
        targets.emplace_back(std::move(cfg));
    }
    return targets;
}

std::string SsdIoEngine::BuildSessionKey(const std::string& endpoint,
                                         const std::string& subnqn,
                                         uint32_t nsid) {
    return endpoint + "|" + subnqn + "|" + std::to_string(nsid);
}

const SsdIoTargetConfig* SsdIoEngine::FindTargetConfig(const std::string& endpoint,
                                                       const std::string& subnqn,
                                                       uint32_t nsid) const {
    for (const auto& target : impl_->target_configs) {
        if (target.target_endpoint == endpoint && target.subnqn == subnqn &&
            target.nsid == nsid) {
            return &target;
        }
    }
    return nullptr;
}

}  // namespace mooncake
