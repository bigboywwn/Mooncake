# Mooncake SPDK-Only 真实环境 E2E 测试计划（V2）

## 1. 输入与基线

本计划基于以下输入：

1. 需求文档：`docs/requirements/requirements-nvmeof-ssd-pooling.md`
2. 设计文档：`docs/source/design/mooncake-store-tiered-storage-v1.md`
3. 门禁测试集：`docs/source/design/mooncake-store-tiered-storage-ddr-ssd-v1-test-suite.md`

代码基线：`ssd_tier` 分支当前提交。

## 2. 策略锁定（测试角色）

1. 仅执行真实环境 `e2e` 测试（`master/client/store + target` 真实进程编排）。
2. 不执行 `gtest`。
3. 不新增、不执行 `legacy target` 场景。
4. SSD 相关场景固定使用 `SPDK target`，且固定 3 target（内存盘模拟）。

说明：产品能力仍保留 `legacy` 兼容开关；测试策略不覆盖该路径。

## 3. 目标

1. 覆盖 `DDR/SSD/remoteFS` 全组合（7 组合）语义一致性。
2. 覆盖 `Put/Get/Query/BatchQuery/BatchGet/BatchPut` 关键接口行为。
3. 覆盖 SPDK 主路径的 3-target 可用性、分布与 fail-fast。
4. 覆盖关键可观测指标的可判定性。

## 4. 执行矩阵（T1~T7）

| Profile | DDR | SSD | remoteFS | SPDK target |
|---|---|---|---|---|
| T1 | On | Off | Off | N/A |
| T2 | Off | On | Off | 3 targets |
| T3 | Off | Off | On | N/A |
| T4 | On | On | Off | 3 targets |
| T5 | On | Off | On | N/A |
| T6 | Off | On | On | 3 targets |
| T7 | On | On | On | 3 targets |

默认参数：

- `MC_NVMEOF_CLIENT_IMPL=spdk`
- SSD 场景通过 `MC_SSD_TARGETS_JSON` 注入 3 target
- remoteFS 场景 master 启动带 `--root_fs_dir=/dev/shm/mooncake-remotefs/<profile>`

## 5. 用例集（E2E-Only）

| ID | 覆盖 | 核心步骤 | 通过标准 |
|---|---|---|---|
| TC-CFG-01 | T1~T7 | 调 `GetTieredStorageConfig` | `enabled_tiers` 与 profile 一致，`nvmeof_client_impl=spdk` |
| TC-RW-01 | T1~T7 | 单 key `Put/Get` | 值一致，返回成功 |
| TC-RW-02 | T1~T7 | `BatchPut/BatchQuery/BatchGet` | 批量全成功，无静默错误 |
| TC-RFS-01 | T3/T5/T6/T7 | client 重启后读取已写入 key | remoteFS 组合数据可读 |
| TC-SPDK-01 | T2/T4/T6/T7 | 启动后检查 target 注册 | runner 识别到 3 个 target |
| TC-SPDK-02 | T2/T4/T6/T7 | 300 keys 分布统计 | 3 target 均命中，分布不过度倾斜 |
| TC-SPDK-03 | 代表 SSD 场景 | 注入不可达 target | client fail-fast（非 0） |
| TC-OBS-01 | 全阶段 | 抓取 metrics | 关键指标存在，且随场景变化 |

## 6. 执行流程

1. 预检：专用测试 VM、`STORE_USE_SPDK=ON`、`-j3` 编译、禁用 legacy/gtest 执行入口。
2. SPDK target 启动：`spdk_tgt + rpc.py` 创建 3 个内存 target（4420/4421/4422）。
3. 矩阵执行：按 `T1 -> T7` 启动 master + `tiered_e2e_runner` 执行 `TC-CFG/RW/RFS/SPDK`。
4. 观测归档：抓取 metrics/log/env，输出统一报告。

建议直接使用：

- `mooncake-store/tests/e2e/scripts/run_realenv_matrix.sh`

## 7. 产物结构

产物目录：`artifacts/e2e-spdk-realenv-<sha>-<ts>/`

- `summary.csv`：`case,profile,status,duration_sec,detail`
- `E2E_TEST_REPORT.md`
- `logs/`
- `metrics/`
- `env/`

## 8. 准入/准出

### 8.1 准入

1. `spdk_tgt` 与 3 target 就绪。
2. `tiered_e2e_runner` 可连通 master。
3. 基础 smoke（`TC-CFG-01`,`TC-RW-01`）可执行。

### 8.2 准出

1. P0 用例（`TC-CFG-01`,`TC-RW-01`,`TC-SPDK-01`,`TC-SPDK-03`,`TC-OBS-01`）100% 通过。
2. 非 P0 用例通过率 >= 95%，且无阻塞缺陷。
3. 输出 `Go / Conditional Go / No-Go` 结论。

## 9. 风险与缓解

1. 风险：DDR+SSD 场景下 SSD 异步下沉有时序波动。  
   缓解：runner 对 SSD descriptor 检查采用重试窗口。
2. 风险：target 不稳定造成误报。  
   缓解：先执行 target 健康预检，再进入矩阵。
