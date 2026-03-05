# Mooncake DDR+SSD 合并门禁测试集（V1）

## 1. 目的

本测试集用于验收以下闭环是否成立：

1. 审查阻塞项 `B1/B2/B3/B4` 全部关闭。
2. `SSD-only`（`MC_DDR_POOL_ENABLED=0, MC_SSD_POOL_ENABLED=1`）可运行。
3. `Put/Get/Query/BatchQuery` 接口兼容不变。
4. SSD 路径仍满足 Store-only（不进入 TE）。

## 2. 测试范围

覆盖范围：

1. 语义正确性：写确认点、读降级、副本排序、回收一致性。
2. 资源闭环：extent 分配/释放、evict 触发路径可观测。
3. 启动与容错：fail-fast、异步写结果上报闭环。

不覆盖：

1. 极限性能压测。
2. 多租户 QoS。
3. 自动回退状态机（V1 未实现）。

## 3. 门禁用例矩阵

| 用例 ID | 目标 | 前置条件 | 核心步骤 | 期望结果 | 证据类型 | DoD 映射 |
|---|---|---|---|---|---|---|
| TC-B1-01 | BatchGet 首副本为 SSD 可读 | `DDR+SSD`，构造 SSD 首副本 key | 执行 `BatchQuery + BatchGet` | 读取成功，无 `INVALID_REPLICA` | client log + 返回码 | DoD-B1 |
| TC-B1-02 | BatchGet memory 失败后降级 | 人工制造 memory 读失败 | 执行 `BatchGet` | 自动回退到 SSD/remoteFS，返回成功 | client log + fallback trace | DoD-B1 |
| TC-B2-01 | 同 key 选路稳定 | 多 target，consistent hash 启用 | 重启前后重复 `PutStart` 同 key | `target_endpoint` 稳定 | `PutStart` 返回值 | DoD-B2 |
| TC-B2-02 | 权重生效 | target weight 不同 | 多 key 分配统计 | 分布近似权重比例，重权重 target 命中更多 | 分配统计日志/脚本输出 | DoD-B2 |
| TC-B3-01 | clear_all 释放闭环 | 小容量 SSD 池 | `PutStart/PutEnd` 后 `BatchReplicaClear(clear_all)` | extent 可复用，后续分配成功 | master log + 后续 PutStart | DoD-B3 |
| TC-B3-02 | segment 清理释放精确 | 指定 segment 清理 | `BatchReplicaClear(segment)` | 仅命中副本释放，账本不泄漏 | master log + metric | DoD-B3 |
| TC-B4-01 | fail-fast 启动语义 | SSD tier 启用且 SsdIoEngine 初始化失败 | 启动 client | 启动失败，不允许静默继续 | 启动日志 + 退出码 | DoD-B4 |
| TC-B4-02 | 异步写失败回收闭环 | 注入 SSD 异步写失败 | 触发 `PutToSsdPool` 失败上报 | `PutRevoke(SSD_POOL)` 生效，无残留 PROCESSING | master metadata + log | DoD-B4 |
| TC-SSDONLY-01 | SSD-only 单 key 可用 | `MC_DDR_POOL_ENABLED=0` + SSD 启用 | 单 key `Put/Get` | 成功且值一致 | 返回码 + 数据校验 | DoD-SSDONLY |
| TC-SSDONLY-02 | SSD-only 批量可用 | 同上 | `BatchPut/BatchGet` | 全量成功，无静默错误 | 返回码 + 批量校验 | DoD-SSDONLY |
| TC-OBS-01 | 关键指标可观测 | 完成上述读写/失败路径 | 拉取 metrics | `ssd_spdk_io_*`、`ssd_connect_fail_total`、`master_ssd_extent_release_fail_total`、`ssd_queue_full_total`、`ssd_reactor_cpu_usage_pct`、`ssd_async_sink_queue_depth`、`ssd_async_sink_queue_lag_ms` 有暴露且可变化 | metrics 抓取结果 | DoD-OBS |
| TC-BOUNDARY-01 | TE 边界约束 | 构造 SSD 读写请求 | 执行读写并检查日志 | SSD 请求不进入 `TransferSubmitter` SSD 分支 | transfer log/assert | DoD-BOUNDARY |

## 4. DoD 定义

1. **DoD-B1**：批量读取在 SSD/non-memory 场景不再回归失败。
2. **DoD-B2**：`policy=consistent_hash` 与实现一致，选路稳定且可表达权重。
3. **DoD-B3**：`BatchReplicaClear` 不产生 SSD extent 容量泄漏。
4. **DoD-B4**：启动与异步写失败路径均闭环，无静默降级。
5. **DoD-SSDONLY**：在关闭 DDR 时仍可完成核心读写。
6. **DoD-OBS**：关键故障与容量信号可观测。
7. **DoD-BOUNDARY**：SSD I/O 不进入 TE。

## 5. 执行建议

1. 先跑单测：`master_service_ssd_test`（B2/B3 基线）。
2. 再跑功能联调：`DDR+SSD` 与 `SSD-only` 两套配置。
3. 最后跑门禁回归：覆盖 `BatchGet`、fail-fast、指标拉取。

## 6. 当前实现态参考结果（2026-03-06，ubuntu-build）

已完成的验证样例：

1. `master_service_ssd_test`：20/20 PASS（覆盖 B2/B3、SSD-only config、query priority、report/revoke）。
2. `master_metrics_test`：5/5 PASS（覆盖新增 SSD 可观测指标更新与读取）。
3. `client_integration_test`：14/14 PASS（覆盖 Batch 路径、SSD-only、fail-fast）。
4. `SsdOnlyClientIntegrationTest.SsdOnlyPutGet`：PASS。
5. `SsdOnlyClientIntegrationTest.SsdOnlyBatchPutBatchGet`：PASS。
6. `ClientFailFastSsdTest.CreateFailsWhenSpdkEnabledWithoutReachableTarget`：PASS。

说明：
1. 本轮为功能与契约门禁，不含极限性能压测。
2. `TC-B2-01` 的“重启前后稳定性”使用同 key 重复 `PutStart` + 重建服务验证；更大样本统计可作为后续性能专项脚本。

## 7. 复现实验命令（与代码一致）

1. 编译（SPDK on）：
`cmake --build /Users/miaomili/Documents/Playground/MoonCake-personal/build-spdk --target mooncake_store mooncake_master mooncake_client clientctl master_service_ssd_test master_metrics_test client_integration_test -j3`
2. 运行 `master_service_ssd_test`：
`/Users/miaomili/Documents/Playground/MoonCake-personal/build-spdk/mooncake-store/tests/master_service_ssd_test`
3. 运行 `master_metrics_test`：
`/Users/miaomili/Documents/Playground/MoonCake-personal/build-spdk/mooncake-store/tests/master_metrics_test`
4. 运行 `client_integration_test`（需 SPDK 动态库路径）：
`LD_LIBRARY_PATH=/Users/miaomili/Documents/Playground/Mooncake/extern/spdk-23.01/build/lib:/Users/miaomili/Documents/Playground/Mooncake/extern/spdk-23.01/dpdk/build-tmp/lib:$LD_LIBRARY_PATH /Users/miaomili/Documents/Playground/MoonCake-personal/build-spdk/mooncake-store/tests/client_integration_test`
