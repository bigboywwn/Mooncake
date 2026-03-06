# Mooncake SPDK-Only 合并门禁 E2E 测试集（V2）

## 1. 目的

本门禁集用于验证以下闭环：

1. `DDR/SSD/remoteFS` 组合语义在真实环境保持一致。
2. `SPDK + 3 target` 主路径稳定可用。
3. `Put/Get/Query/BatchQuery/BatchGet/BatchPut` 无回归。
4. fail-fast、可观测性具备可判定结果。

## 2. 强约束

1. 仅执行 `e2e`（真实部署 `master/client/store/target`）。
2. 不执行 `gtest`。
3. 不覆盖 `legacy target`。
4. SSD 相关场景固定 3 target。

## 3. 门禁矩阵

| Case | Profile | Priority | 预期 |
|---|---|---|---|
| TC-CFG-01 | T1~T7 | P0 | `enabled_tiers` 与 profile 一致，`impl=spdk` |
| TC-RW-01 | T1~T7 | P0 | 单 key `Put/Get` 成功且值一致 |
| TC-RW-02 | T1~T7 | P1 | `BatchPut/BatchQuery/BatchGet` 全成功 |
| TC-RFS-01 | T3/T5/T6/T7 | P1 | client 重启后数据可读 |
| TC-SPDK-01 | T2/T4/T6/T7 | P0 | runner 识别到 3 个 target |
| TC-SPDK-02 | T2/T4/T6/T7 | P1 | 300 keys 分布命中 3 target，分布可接受 |
| TC-SPDK-03 | SSD 代表场景 | P0 | 不可达 target 下 client fail-fast |
| TC-OBS-01 | 全阶段 | P0 | 关键指标可抓取且可变化 |

## 4. DoD

1. **DoD-CFG**：配置面结果与 profile 严格一致。
2. **DoD-RW**：核心 API 在全组合无功能回归。
3. **DoD-SPDK**：SPDK 3-target 场景可运行、可分布、可 fail-fast。
4. **DoD-OBS**：关键指标可用于定位问题。

## 5. 执行命令（统一入口）

1. 编译（固定 `-j3`）：

```bash
cmake --build /Users/miaomili/Documents/Playground/MoonCake-personal/build-spdk \
  --target mooncake_master mooncake_client clientctl tiered_e2e_runner \
  -j3
```

2. 执行矩阵：

```bash
/Users/miaomili/Documents/Playground/MoonCake-personal/mooncake-store/tests/e2e/scripts/run_realenv_matrix.sh
```

3. 报告输出：

- `artifacts/e2e-spdk-realenv-<sha>-<ts>/summary.csv`
- `artifacts/e2e-spdk-realenv-<sha>-<ts>/E2E_TEST_REPORT.md`

## 6. Blocked 规则

以下情况标记 `BLOCKED`，不计入功能失败：

1. target 进程不可用或网络阻塞导致环境无法建链。
2. 监控端口不可达导致 `TC-OBS-01` 无法抓取。

## 7. 已删除项（V1 -> V2）

1. 删除所有 `legacy` 用例与命令。
2. 删除 `master_service_ssd_test/master_metrics_test/client_integration_test` 作为测试角色门禁执行项。
3. 删除 `MC_SSD_POOL_TARGETS=file://...` legacy 3-target 专项执行方式。
