# Mooncake Store V1 技术设计（NVMeoF + SSD Pooling，As-Built）

## 0. 文档目的与版本

本文是 `ssd_tier` 分支当前实现的完整设计文档（As-Built），用于统一研发、测试、运维认知，避免“设计描述”与“实际代码行为”漂移。

- 代码基线：`origin/ssd_tier`（当前头部提交 `e8067f1`）
- 设计范围：`mooncake-store` 内部 `DDR/SSD/remoteFS` 分层能力
- 不在范围：`TE` 层 SSD 数据路径、V2 自动回退状态机、reactor 自动扩缩容

---

## 1. 目标与非目标

### 1.1 目标

1. 在不变更业务 API（`Put/Get/Query/BatchQuery`）前提下，支持 `DDR/SSD/remoteFS` 分层。
2. SSD 路径由 Store 直管：设备发现、空间分配、读写、释放、evict 闭环。
3. 支持 `SSD-only`（`MC_DDR_POOL_ENABLED=0`, `MC_SSD_POOL_ENABLED=1`）可运行。
4. 支持 `SPDK 23.01.x + NVMeoF(TCP)` 真实 target 对接，且 fail-fast。

### 1.2 非目标

1. 不实现 V1 自动回退状态机（`auto_fallback_enabled` 仅兼容字段）。
2. 不引入 TE 对 SSD 的转发/提交流程。
3. 不实现 reactor 在线扩缩容与高级 QoS 调度。

---

## 2. 总体架构

### 2.1 控制面（Master）

- 核心组件：`MasterService` + `SSDPoolManager`
- 职责：
  - `GetTieredStorageConfig` 返回真实运行态 `enabled_tiers`
  - `PutStart` 分配 `MEMORY`/`SSD_POOL`/`DISK` descriptor
  - `PutEnd`/`PutRevoke` 维护对象副本状态机
  - `BatchReplicaClear`/`Remove*`/`evict` 触发 SSD extent 释放
  - `SSDPoolManager` 做 target 管理、hash 选路、extent 生命周期

### 2.2 数据面（Client）

- 核心组件：`Client` + `SsdIoEngine`
- 职责：
  - 同步确认层选择：`SelectSyncAckReplicaType`
  - 读路径降级：`ReadWithFallback`
  - 批量读两段式：memory 批量 + non-memory 回退
  - SSD 异步写完成后上报：`ReportSsdWriteResult`

### 2.3 边界约束（TE）

- `transfer_task.cpp` 明确禁止 SSD 路径进入 TE。
- `submitSsdOperation` 仅返回失败并提示“Use store-layer SsdIoEngine for SSD_POOL I/O”。

---

## 3. 接口与类型契约

### 3.1 对外 API（不变）

- `Put/Get/Query/BatchQuery` 签名不变。

### 3.2 对内扩展

1. `ReplicaType`：包含 `SSD_POOL`。
2. `SsdExtentDescriptor` 字段：
   - `target_endpoint`
   - `subsystem_nqn`
   - `nsid`
   - `block_size`
   - `lba_start`
   - `lba_count`
   - `object_size`
   - `extent_id`
3. RPC：
   - `GetTieredStorageConfig`
   - `ReportSsdWriteResult`

### 3.3 运行配置（关键）

1. Tier 开关：
   - `MC_DDR_POOL_ENABLED`（默认 `true`）
   - `MC_SSD_POOL_ENABLED`
2. NVMeoF 客户端实现：
   - `MC_NVMEOF_CLIENT_IMPL=spdk|legacy`
3. SPDK 队列/超时：
   - `MC_SSD_QUEUE_LIMIT`
   - `MC_SSD_IO_TIMEOUT_MS`
4. target 配置（推荐）：
   - `MC_SSD_TARGETS_JSON`（每 target 独立配置）
5. 兼容 fallback：
   - `MC_SSD_POOL_TARGETS` + `MC_SSD_POOL_SUBSYSTEM_NQN` + `MC_SSD_POOL_NSID`

---

## 4. 分层语义（V1 锁定）

### 4.1 读优先级

`DDR > SSD > remoteFS`

`Query` 接口不变，命中层由 descriptor 类型判定。

### 4.2 写确认语义

“最高可写层确认即返回”，通过 `SelectSyncAckReplicaType` 实现：

1. `MEMORY` 可写：同步确认 `PutEnd(MEMORY)`，SSD/DISK 异步。
2. `MEMORY` 不可写但 `SSD_POOL` 可写：同步写 SSD 并 `PutEnd(SSD_POOL)` 后返回。
3. 仅 `DISK`：同步 `PutEnd(DISK)` 后返回。

### 4.3 `PutStart` 语义

SSD 可分配时可返回 `MEMORY + SSD_POOL` 双 descriptor；SSD 分配失败不阻塞 DDR 路径。

---

## 5. 读写路径设计细节

### 5.1 单 key 读

`ReadWithFallback(key, query_result, slices)` 逐副本尝试，遇失败按层级继续降级。

### 5.2 Batch 读

两段式策略：

1. 优先处理 memory 首副本（批量 submit）
2. non-memory 或批量失败 key 回退到 `ReadWithFallback`

### 5.3 SSD 异步写结果闭环

`PutToSsdPool` 成功/失败都上报 `ReportSsdWriteResult`：

1. `success=true` -> `PutEnd(SSD_POOL)`
2. `success=false` -> `PutRevoke(SSD_POOL)` + target health 反馈

---

## 6. SSD 控制面与分配算法

### 6.1 target 健康状态

`HEALTHY / DEGRADED / UNAVAILABLE`

- 分配优先 `HEALTHY`
- `DEGRADED` 可降权使用
- `UNAVAILABLE` 不参与分配

### 6.2 一致性哈希（已替换轮询）

`SSDPoolManager` 使用加权 hash ring：

1. key hash：`FNV-1a 64-bit`
2. vnode：`clamp(weight,1,64) * 128`
3. ring token：`SplitMix64(...)`
4. 选路：`lower_bound(hash(key))` 环形扫描 + target 去重 + 健康过滤

---

## 7. SPDK 23.01.x 实现策略

### 7.1 构建闸门

`STORE_USE_SPDK` 默认 `OFF`。开启时要求：

1. `pkg-config` 可找到 `spdk_nvme` 和 `spdk_env_dpdk`
2. 版本必须 `23.01.x`
3. 否则 CMake 配置期直接失败

### 7.2 运行时 fail-fast

当 `nvmeof_client_impl=spdk`：

1. 未以 `STORE_USE_SPDK=ON` 编译 -> 启动失败
2. target 全不可达 -> 启动失败
3. 不自动降级到 file I/O

### 7.3 I/O 路径

- 写：`spdk_nvme_ns_cmd_write`
- 读：`spdk_nvme_ns_cmd_read`
- 支持 block 语义下的对齐处理与 `object_size` 截断
- `queue_limit` / `io_timeout_ms` 在提交层生效

---

## 8. 生命周期与 Evict 闭环

### 8.1 统一释放入口

以下路径都收敛到 `ReleaseExtent`：

1. `PutRevoke(SSD_POOL)`
2. `Remove` / `RemoveByRegex` / `RemoveAll`
3. `BatchReplicaClear(clear_all/segment)`
4. `BatchEvictSsd`

### 8.2 安全约束

evict SSD 前必须检查“最后 `COMPLETE` 副本保护”：

- 若该 SSD 副本是对象最后一个 `COMPLETE`，禁止回收。

### 8.3 Evict 算法

复用 DDR 两阶段 near-LRU：

1. 第一阶段：非 soft-pin
2. 第二阶段：按配置决定是否包含 soft-pin

触发条件：`ssd_used_ratio` 高水位或 `ssd_need_eviction=true`。

---

## 9. 可观测性

最小指标集合：

1. `ssd_spdk_io_submit_total`
2. `ssd_spdk_io_timeout_total`
3. `ssd_spdk_io_fail_total`
4. `ssd_connect_fail_total`
5. `ssd_target_health{target}`
6. `master_ssd_eviction_success`
7. `master_ssd_eviction_attempts`
8. `master_ssd_extent_release_fail_total`

---

## 10. Rollout 与 Rollback

### 10.1 灰度建议

1. 先 `legacy` 验证语义闭环
2. 再开 `spdk` + 单 target
3. 最后扩展多 target 与权重策略

### 10.2 快速回滚

1. `MC_SSD_POOL_ENABLED=0` 回到 DDR/remoteFS
2. 保持 API 不变，不影响上层业务调用

---

## 11. 当前验证结论（基于本轮提交）

### 11.1 已验证通过

1. `STORE_USE_SPDK=ON` 可编译。
2. `SSD-only + SPDK` 手工 `Put/Get` 通过。
3. target 侧 `bdev_get_iostat` 计数增长可证明 I/O 真实到达 NVMeoF target。

### 11.2 当前已知缺口

`client_integration_test` 在同进程多 client 场景下会触发：

- `Invalid arguments to reinitialize SPDK env`
- 根因：`spdk_env_init` 重复初始化缺少进程级单例/引用计数治理

该问题不影响单 client 手工链路验证，但会阻塞多 client 自动化测试稳定性。

---

## 12. V1 实施清单（最终）

1. `GetTieredStorageConfig.enabled_tiers` 必须是真实运行态。
2. SSD 路径必须仅走 Store `SsdIoEngine`。
3. `BatchGet` 必须具备 non-memory 回退闭环。
4. `SSDPoolManager` 必须保持 consistent hash 实现，不得回退轮询。
5. `BatchReplicaClear` 必须释放 SSD extent。
6. `ReportSsdWriteResult` 必须覆盖成功与失败两条路径。
7. `SSD-only` 必须可运行。
8. SPDK fail-fast 语义必须保持。
9. 多 client SPDK runtime 单例化问题需在后续修复并补回归。
