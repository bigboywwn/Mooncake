# Mooncake DDR+SSD 组合场景测试计划与用例集（V1）

## 1. 输入文档与基线

本测试计划基于以下输入：

1. 需求分析文档：`docs/requirements/requirements-nvmeof-ssd-pooling.md`
2. 技术设计文档：`docs/source/design/mooncake-store-tiered-storage-v1.md`
3. 现有门禁测试集：`docs/source/design/mooncake-store-tiered-storage-ddr-ssd-v1-test-suite.md`

代码与功能基线：`ssd_tier` 当前实现态（As-Built）。

## 2. 测试目标

1. 验证 DDR 与 SSD 两类存储在不同启停组合下的语义一致性。
2. 验证读写路径规则：
   - 读：`DDR > SSD`
   - 写：最高可写层确认即返回（DDR 可写时优先 DDR；无 DDR 时由 SSD 确认）。
3. 验证 SSD 控制面闭环：分配、写入、上报、回收、evict、可观测。
4. 验证 NVMeoF 客户端实现约束：`legacy` 与 `spdk` 行为符合设计，`spdk` 失败时 fail-fast。

## 3. 范围定义

### 3.1 In Scope

1. 组合场景：
   - `DDR-only`（`MC_DDR_POOL_ENABLED=1`, `MC_SSD_POOL_ENABLED=0`）
   - `SSD-only`（`MC_DDR_POOL_ENABLED=0`, `MC_SSD_POOL_ENABLED=1`）
   - `DDR+SSD`（`MC_DDR_POOL_ENABLED=1`, `MC_SSD_POOL_ENABLED=1`）
2. 接口覆盖：`Put/Get/Query/BatchQuery`。
3. 异常覆盖：SSD 写失败、target 不可达、队列过载超时、容量回收。
4. 观测覆盖：关键指标与日志可用于定位问题。

### 3.2 Out of Scope

1. `remoteFS` 深层存储语义。
2. 跨地域容灾、多租户 QoS、高级自动重平衡。
3. 纯性能极限压测（仅保留基础稳定性和过载行为验证）。

## 4. 执行矩阵（组合 x 实现）

| Profile | DDR | SSD | NVMeoF 实现 | 目标 |
|---|---|---|---|---|
| P1 | On | Off | N/A | 验证 DDR 单层语义基线 |
| P2 | Off | On | `legacy` | 验证 SSD-only 功能闭环（无真实 target 依赖） |
| P3 | Off | On | `spdk` | 验证 SSD-only + SPDK/NVMeoF 链路 |
| P4 | On | On | `legacy` | 验证 DDR+SSD 组合语义 |
| P5 | On | On | `spdk` | 验证 DDR+SSD + SPDK 生产主路径 |

建议门禁最小集：`P1 + P2 + P4`；发布前全量集：`P1~P5`。

## 5. 环境与前置条件

1. Linux 构建机（建议使用 Ubuntu VM）。
2. 已编译：`mooncake_master`、`mooncake_client`、`clientctl`、`client_integration_test`。
3. SPDK 场景要求：
   - 编译时开启 `STORE_USE_SPDK=ON`
   - 可达 NVMeoF target
4. 关键环境变量：
   - `MC_DDR_POOL_ENABLED`
   - `MC_SSD_POOL_ENABLED`
   - `MC_NVMEOF_CLIENT_IMPL=legacy|spdk`
   - `MC_SSD_QUEUE_LIMIT`
   - `MC_SSD_IO_TIMEOUT_MS`
   - `MC_SSD_TARGETS_JSON`（SPDK 场景）

## 6. 测试阶段与节奏

1. Phase A（Smoke，0.5 天）：启动、配置、单 key `Put/Get`。
2. Phase B（语义，1 天）：写确认点、读优先级、批量读回退。
3. Phase C（故障，1 天）：fail-fast、异步失败回收、过载超时。
4. Phase D（观测与回归，0.5 天）：指标核对、回归复跑、测试报告。

## 7. 测试用例集（DDR+SSD 组合主线）

### 7.1 配置与启动

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-CFG-01 | P1 | P0 | 启动后调用 `GetTieredStorageConfig` | 仅返回 `DDR` | FR-01 |
| TC-CFG-02 | P2 | P0 | 启动后调用 `GetTieredStorageConfig` | 仅返回 `SSD` | FR-01 |
| TC-CFG-03 | P4/P5 | P0 | 启动后调用 `GetTieredStorageConfig` | 返回 `DDR,SSD` | FR-01 |
| TC-CFG-04 | P3/P5 | P0 | `spdk` 模式下 target 不可达启动 | client fail-fast 退出，不静默降级 | FR-05 |
| TC-CFG-05 | P3/P5 | P1 | `spdk_reactor_cores=1/6` 分别启动 | 均可启动且参数生效 | FR-06 |

### 7.2 写确认与异步下沉语义

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-WR-01 | P1 | P0 | `Put` 后立即返回 | 由 `MEMORY` 确认成功 | FR-04 |
| TC-WR-02 | P2/P3 | P0 | `Put` 后立即返回 | 由 `SSD_POOL` 确认成功 | FR-04 |
| TC-WR-03 | P4/P5 | P0 | 正常 `Put` | 由 `MEMORY` 确认；SSD 异步写后 `PutEnd(SSD_POOL)` | FR-04 |
| TC-WR-04 | P4/P5 | P0 | 注入 SSD 异步写失败 | 触发 `PutRevoke(SSD_POOL)`，无残留 PROCESSING | FR-08 |

### 7.3 读优先级与回退

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-RD-01 | P4/P5 | P0 | 同 key 在 DDR 与 SSD 都存在时 `Get` | 优先命中 DDR | FR-03 |
| TC-RD-02 | P4/P5 | P0 | 构造 DDR 未命中、SSD 命中后 `Get` | 自动回退命中 SSD | FR-03 |
| TC-RD-03 | P2/P3 | P0 | `SSD-only` 场景 `Get` | SSD 命中返回正确值 | FR-03 |
| TC-RD-04 | P4/P5 | P0 | `BatchQuery/BatchGet` 含 memory 与 non-memory 混合 key | non-memory key 回退链路成功，无批量回归失败 | FR-03 |
| TC-RD-05 | P1 | P1 | DDR-only 不存在 key `Get` | 返回可判定未命中错误 | FR-10 |

### 7.4 资源生命周期与回收

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-LC-01 | P2/P3/P4/P5 | P0 | `BatchReplicaClear(clear_all)` | SSD extent 被释放且可复用 | FR-02 |
| TC-LC-02 | P2/P3/P4/P5 | P1 | `Remove/RemoveByRegex` 删除对象 | 对应 extent 释放、账本一致 | FR-02 |
| TC-LC-03 | P4/P5 | P1 | 触发高水位 evict | 不回收“最后 COMPLETE 副本” | FR-09 |

### 7.5 过载、故障与可恢复性

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-ER-01 | P3/P5 | P0 | 压测触发队列上限与超时 | 出现可判定 timeout，系统可恢复 | FR-08 |
| TC-ER-02 | P3/P5 | P1 | target 健康从 `HEALTHY` -> `UNAVAILABLE` | 分配跳过不可用 target，无静默错误 | FR-09 |
| TC-ER-03 | P3/P5 | P1 | 多 target + 一致性哈希，重启前后同 key 分配 | 同 key 选路稳定 | FR-07 |
| TC-ER-04 | P3/P5 | P1 | 多 target 不同权重写入统计 | 分布近似权重比例 | FR-07 |

### 7.6 可观测性

| 用例 ID | 组合 | 级别 | 核心步骤 | 预期结果 | 需求映射 |
|---|---|---|---|---|---|
| TC-MET-01 | P2~P5 | P0 | 执行成功/失败/超时路径后抓取 metrics | `ssd_spdk_io_*`、`ssd_connect_fail_total`、`master_ssd_extent_release_fail_total` 可见且数值变化符合场景 | FR-09 |
| TC-MET-02 | P3/P5 | P1 | 调整 `spdk_reactor_cores` 压测并观测 | reactor CPU 与超时指标可用于区分 1 核与 6 核行为 | FR-06, FR-09 |

## 8. 准入与准出标准

### 8.1 准入（Entry）

1. 构建成功且基础 smoke 通过（`P1/P2` 单 key `Put/Get` 通过）。
2. SPDK 场景所需 target 与网络就绪（执行 `P3/P5` 时）。
3. 指标采集端可访问 metrics 端点。

### 8.2 准出（Exit）

1. 全部 P0 用例通过率 100%。
2. P1 用例通过率 >= 95%，且无 P1 级阻塞缺陷。
3. 无“静默数据错误”与“不可判定失败”问题。
4. 已输出测试报告（覆盖率、失败分析、风险结论）。

## 9. 风险与缓解

1. 风险：SPDK 多 client 同进程初始化冲突导致自动化不稳定。  
   缓解：门禁阶段采用单 client 基线；并行补充进程级单例治理专项验证。
2. 风险：测试环境 target 不稳定导致误报。  
   缓解：将环境探活作为前置检查，失败时标记为环境阻塞而非用例失败。
3. 风险：仅跑 `legacy` 导致生产主路径覆盖不足。  
   缓解：发布前强制执行 `P3/P5`（SPDK）全量回归。

## 10. 交付物

1. 测试执行记录（按 `P1~P5` profile）。
2. 用例结果矩阵（Pass/Fail/Blocked）。
3. 缺陷清单与影响评估（按 P0/P1/P2 分级）。
4. 发布结论：`Go / Conditional Go / No-Go`。
