# MoonCake v0.3.8 -> main(`ssd_tier`) 语义迁移报告

## 基线与分支
- 目标基线: `origin/ssd_tier`
- 集成分支: `codex/ssd-tier-remerge-v038-port`
- 来源补丁: `artifacts/merge-recovery/20260305-201601-v038-patches/0001..0006`

## 迁移提交序列
1. `f19588e` docs: requirements/design/test design 文档迁移
2. `fafa03f` store types/config: `Replica`/RPC/config 语义迁移
3. `91ae493` replica variant 合并修复
4. `37f3130` master control-plane: SSD pool + tiered RPC
5. `a7df2d1` client data-path: DDR 同步确认 + SSD 异步/降级
6. `802dd6b` build/SPDK gate: `STORE_USE_SPDK` 与外部依赖闸门
7. `5b03b13` metrics/tests: SSD 指标与测试补强
8. `2bd6f33` remerge 修复: `BatchEvictSsd` 适配 main 元数据 API

## 主要冲突点与处理
- `master_service.*`: 保留 main 的 snapshot/task 结构，手工移植 SSD 控制面与生命周期释放逻辑。
- `client_service.cpp`: 保留 main 的 hot-cache 路径，同时补齐 B1 的 BatchGet 降级逻辑。
- `CMakeLists.txt`: 保留 main 现有链接项，叠加 `STORE_USE_SPDK` 与 `ssd_io_engine.cpp`。

## 门禁结果（VM）
- 编译（通过）
  - `mooncake_store`
  - `mooncake_master`
  - `mooncake_client`
- 基础功能（通过）
  - `client_integration_test --gtest_filter=ClientIntegrationTest.BasicPutGetOperations`
- SSD 关键回归（通过）
  - `master_service_ssd_test` 子集:
    - `PutStartReturnsSsdPoolDescriptorWhenEnabled`
    - `ConsistentHashSelectsStableTargetForSameKey`
    - `ConsistentHashHonorsWeight`
    - `BatchReplicaClearAllReleasesSsdExtent`
    - `TieredConfigReflectsSsdOnlyWhenDdrDisabled`

## 备注
- SPDK 仍采用外部依赖模式，未 vendor `extern/spdk-23.01`。
- 本报告对应分支状态: `ahead 8`（相对 `origin/ssd_tier`）。
