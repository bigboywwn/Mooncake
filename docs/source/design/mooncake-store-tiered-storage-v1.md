# Mooncake Store V1 技术设计（审查闭环版）

## 1. 设计目标

本版本目标是将 `NVMeoF + SSD_POOL` 实现推进到可合并状态，并关闭审查阻塞项 `B1~B4`，同时纳入 `SSD-only` 运行能力。

核心约束：

1. 业务 API 不变：`Put/Get/Query/BatchQuery` 签名保持不变。
2. SSD I/O 仅在 Store 层执行，`transfer-engine` 不参与 `SSD_POOL` 路径。
3. 写确认点采用“最高可写层确认即返回”。
4. `enabled_tiers` 必须反映真实运行态，而不是声明态。

## 2. 审查问题闭环（B1~B4）

### 2.1 B1：BatchGet 在 SSD 首副本场景失败

问题：批量读路径原先直接走 `TransferSubmitter`，遇到 `SSD_POOL` 描述符会失败，且无降级。

闭环决策：

1. 引入 `ReadWithFallback()`，复用单 key `Get` 的多副本降级读取语义。
2. `BatchGet` 两段式执行：
   - 段 A：仅 memory 首副本走批量 transfer 提交。
   - 段 B：非 memory 首副本，或段 A 失败的 key，逐 key 回退 `ReadWithFallback()`。
3. `BatchGetWhenPreferSameNode` 同步应用该降级策略。

结果：批量读在 `DDR/SSD/remoteFS` 混合副本下保持正确性优先，不再出现 SSD 首副本即失败。

### 2.2 B2：策略声明是 consistent_hash，实现却是轮询

问题：`AllocateSsdExtent(policy=consistent_hash)` 名义一致性哈希，但实现使用 `rr_index_`。

闭环决策：

1. `SSDPoolManager` 替换为“加权一致性哈希环”。
2. 哈希规则：
   - key 哈希：`FNV-1a 64-bit`
   - ring token：`SplitMix64(endpoint_hash ^ vnode_seed)`
3. 虚拟节点规则：`vnodes = clamp(weight, 1, 64) * 128`。
4. 选路规则：`lower_bound(hash(key))` 环形遍历，去重 target，按健康状态分层（`HEALTHY -> DEGRADED`）。
5. `UNAVAILABLE` target 不参与分配。

结果：`policy=consistent_hash` 与真实分配算法一致，且支持权重表达能力。

### 2.3 B3：BatchReplicaClear 路径未释放 SSD extent

问题：清理 metadata 时存在“元数据删除但 extent 未释放”的容量泄漏风险。

闭环决策：

1. `BatchReplicaClear(clear_all)` 在 `accessor.Erase()` 前调用 `ReleaseSsdReplicasForObject(...)`。
2. `BatchReplicaClear(segment)` 删除命中的 `SSD_POOL` 副本时，逐条调用 `ReleaseExtent(extent_id, reason)`。
3. 释放失败统一累计 `master_ssd_extent_release_fail_total`。

结果：`PutRevoke/Remove/RemoveByRegex/RemoveAll/BatchReplicaClear` 均满足统一 extent 生命周期约束。

### 2.4 B4：客户端启动未 fail-fast，异步写失败未闭环

问题：

1. 获取 tiered config 失败时客户端仍继续启动。
2. SSD 异步写失败时历史路径可能“跳过处理”。

闭环决策：

1. `InitSsdIoEngine()` 规则调整：
   - 获取 `GetTieredStorageConfig` 失败：启动失败。
   - `SSD` 启用且引擎初始化失败：启动失败（fail-fast）。
2. 新增 `ReportSsdWriteResult` RPC 闭环（client -> rpc -> master）。
3. 语义固定：
   - `success=true`：`PutEnd(SSD_POOL)`
   - `success=false`：`PutRevoke(SSD_POOL)` 并反馈 target health
4. `PutToSsdPool` 不再“失败静默跳过”。

结果：异步 SSD 写入状态可回收、可观测，不残留 `PROCESSING`。

## 3. Tier 语义与配置模型

## 3.1 Tier 开关与真实态返回

新增环境变量：

1. `MC_DDR_POOL_ENABLED`（默认 `true`）
2. `MC_SSD_POOL_ENABLED`
3. `root_fs_dir` 非空时启用 `remoteFS`

`GetTieredStorageConfig.enabled_tiers` 按真实开关返回：

1. `DDR`：仅当 `MC_DDR_POOL_ENABLED=true`
2. `SSD`：仅当 `MC_SSD_POOL_ENABLED=true` 且 SSDPoolManager 可用
3. `remoteFS`：仅当 `root_fs_dir` 启用

## 3.2 `auto_fallback_enabled` 兼容语义

字段保留兼容，但 V1 不实现自动回退状态机：

1. 默认值为 `false`
2. 若配置为 `true`，仅记录兼容日志，不改变实际行为

## 4. 写路径语义（支持 SSD-only）

## 4.1 同步确认层选择

新增 `SelectSyncAckReplicaType(replicas)`：

1. 优先级：`MEMORY > SSD_POOL > DISK`
2. 同步确认点固定为“最高可写层”

适配场景：

1. `DDR+SSD`：DDR 同步确认后返回，SSD 异步下沉。
2. `SSD-only`：SSD 同步写 + `PutEnd(SSD_POOL)` 后返回。
3. `DISK-only`：DISK 同步写 + `PutEnd(DISK)` 后返回。

## 4.2 异步下沉规则

仅处理“低于同步确认层”的副本：

1. 同步层为 `MEMORY` 时，异步处理 `SSD_POOL`/`DISK`。
2. 同步层为 `SSD_POOL` 时，仅异步处理 `DISK`。
3. 同步层为 `DISK` 时，无异步下沉任务。

## 4.3 批量写路径

`BatchPut` 同步确认改为按 key 分组：

1. `MEMORY` 组：继续 `BatchPutEnd`。
2. `SSD_POOL`/`DISK` 组：逐 key `PutEnd`。
3. 失败清理按 key 的真实副本类型执行 `PutRevoke`，不再仅撤销 `MEMORY`。

## 5. 读路径语义（Query 接口不变）

## 5.1 优先级

仍为：`DDR > SSD > remoteFS`。

## 5.2 降级策略

1. 单 key：`Get(key, query_result, slices)` 已支持逐层降级读取。
2. 批量：
   - memory 首副本优先走批量 transfer；
   - 非 memory 或 transfer 失败自动回退单 key 降级逻辑。

## 6. 数据模型与状态机

## 6.1 SSD extent

`SsdExtentDescriptor` 字段：

1. `target_endpoint`
2. `subsystem_nqn`
3. `nsid`
4. `block_size`
5. `lba_start`
6. `lba_count`
7. `object_size`
8. `extent_id`

## 6.2 结果上报状态机

1. `PutStart` 分配 extent -> `PROCESSING`
2. 异步写成功 -> `ReportSsdWriteResult(success=true)` -> `COMPLETE`
3. 异步写失败 -> `ReportSsdWriteResult(success=false)` -> `PutRevoke(SSD_POOL)` -> 回收 extent

## 7. 可观测性

门禁指标最小集合：

1. `ssd_spdk_io_submit_total`
2. `ssd_spdk_io_timeout_total`
3. `ssd_spdk_io_fail_total`
4. `ssd_connect_fail_total`
5. `ssd_target_health{target}`
6. `master_ssd_eviction_attempts`
7. `master_ssd_eviction_success`
8. `master_ssd_extent_release_fail_total`

## 8. 回滚策略

1. 快速回滚：关闭 `MC_SSD_POOL_ENABLED`，系统回到 DDR/remoteFS。
2. 关闭 DDR：`MC_DDR_POOL_ENABLED=0` 可验证 SSD-only 模式。
3. 边界门禁：`transfer_task` 保持 `SSD_POOL` 禁入断言，防止回退到 TE 路径。

## 9. 实施检查清单（Implementation Checklist）

1. `GetTieredStorageConfig.enabled_tiers` 与运行态一致。
2. `MC_DDR_POOL_ENABLED` 生效且默认兼容（true）。
3. `BatchGet` 具备 SSD/non-memory 降级闭环。
4. `SSDPoolManager` 使用加权 consistent hash ring，而非轮询。
5. `BatchReplicaClear` 全路径释放 SSD extent。
6. `InitSsdIoEngine` 与 tiered config 获取均为 fail-fast 语义。
7. `ReportSsdWriteResult` RPC 全链路生效。
8. `SSD-only` 下单 key 与 batch 读写可运行。
9. 文档与实现同 PR 同步更新，无契约漂移。
