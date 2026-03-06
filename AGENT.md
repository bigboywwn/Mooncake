# Mooncake 部署与 E2E 执行手册（SPDK-Only）

## 0. 执行规则（最高优先级）

- 仅允许使用 `ubuntu-build` 虚拟机执行构建与联调。
- 禁止使用 `ubuntu-test`（该实例保留给测试 agent）。
- 测试角色仅执行真实环境 `e2e` 测试。
- 测试角色不执行 `gtest`。
- 不新增、不执行任何 `legacy target` 测试用例。
- SSD 场景固定 `MC_NVMEOF_CLIENT_IMPL=spdk` + 3 target。
- 若本文件其他条目与本节冲突，以本节为准。

## 1. 适用范围

- 项目目录：`/Users/miaomili/Documents/Playground/MoonCake-personal`
- 目标：执行 `SPDK-only` 的真实环境 E2E 回归（`T1~T7`）
- 当前推荐环境：Linux VM（macOS 直接运行 Linux 二进制会报 `exec format error`）

## 2. 快速准备

1. 登录 VM：

```bash
ssh -F ~/.lima/ubuntu-build/ssh.config -o User=root -o ControlMaster=no -o ControlPath=none lima-ubuntu-build
```

2. 切到项目目录：

```bash
cd /Users/miaomili/Documents/Playground/MoonCake-personal
```

3. 编译（固定 `-j3`）：

```bash
cmake --build build-spdk \
  --target mooncake_master mooncake_client clientctl tiered_e2e_runner \
  -j3
```

## 3. SPDK 3-target 初始化

```bash
./mooncake-store/tests/e2e/scripts/setup_spdk_ram_targets.sh --print-json
```

该命令会：

- 启动（或复用）`spdk_tgt`
- 创建 3 个内存 target（端口 `4420/4421/4422`）
- 输出可直接使用的 `MC_SSD_TARGETS_JSON`

## 4. 矩阵执行（T1~T7）

统一入口：

```bash
./mooncake-store/tests/e2e/scripts/run_realenv_matrix.sh
```

默认行为：

- 自动按 `T1 -> T7` 执行 `TC-CFG-01/TC-RW-01/TC-RW-02`
- remoteFS 组合执行 `TC-RFS-01`
- SSD 组合执行 `TC-SPDK-01/TC-SPDK-02`
- 额外执行 `TC-SPDK-03` fail-fast 场景
- 输出统一报告到 `artifacts/e2e-spdk-realenv-<sha>-<ts>/`

## 5. 单用例执行（tiered_e2e_runner）

```bash
./build-spdk/mooncake-store/tests/e2e/tiered_e2e_runner \
  --case TC-CFG-01 \
  --master 127.0.0.1:50051 \
  --metadata P2PHANDSHAKE \
  --protocol tcp \
  --expect-tiers DDR,SSD \
  --expect-impl spdk
```

支持用例：

- `TC-CFG-01`
- `TC-RW-01`
- `TC-RW-02`
- `TC-RFS-01`
- `TC-SPDK-01`
- `TC-SPDK-02`
- `TC-SPDK-03`

## 6. 报告与产物

产物目录：

- `artifacts/e2e-spdk-realenv-<sha>-<ts>/summary.csv`
- `artifacts/e2e-spdk-realenv-<sha>-<ts>/E2E_TEST_REPORT.md`
- `artifacts/e2e-spdk-realenv-<sha>-<ts>/logs`
- `artifacts/e2e-spdk-realenv-<sha>-<ts>/metrics`
- `artifacts/e2e-spdk-realenv-<sha>-<ts>/env`

## 7. 配置总表（测试视角）

| 变量名 | 默认值 | 生效侧 | 说明 |
|---|---|---|---|
| `MC_DDR_POOL_ENABLED` | `true` | master | DDR tier 开关 |
| `MC_SSD_POOL_ENABLED` | `false` | master | SSD tier 开关 |
| `MC_NVMEOF_CLIENT_IMPL` | `spdk` | master/client | 测试固定为 `spdk` |
| `MC_SPDK_REACTOR_CORES` | `1` | master | reactor 核数（钳制 `[1,6]`） |
| `MC_SSD_QUEUE_LIMIT` | `1024` | master/client | SSD 队列上限 |
| `MC_SSD_IO_TIMEOUT_MS` | `5000` | master/client | SSD IO 超时 |
| `MC_SSD_TARGETS_JSON` | 空 | master/client | SSD target 配置（推荐且测试必填） |

注意：`MC_SSD_POOL_TARGETS`/`legacy` 相关配置在测试策略中不再使用。

## 8. 故障排查

1. `exec format error`
- 原因：在 macOS 直接运行 Linux 二进制。
- 处理：切换到 Linux VM。

2. `tiered_e2e_runner` 启动失败
- 检查 `mooncake_master` 是否监听 `127.0.0.1:50051`。
- 检查 `--master` 参数与 master 一致。

3. SSD 场景 fail-fast
- 检查是否以 `STORE_USE_SPDK=ON` 构建。
- 检查 `MC_SSD_TARGETS_JSON` 是否可达且与 target 配置一致。

4. `TC-SPDK-02` 在长时运行中失败
- 常见原因：SPDK 连接超时或 target 不稳定。
- 处理：检查 target 健康状态、网络链路与 `MC_SSD_TARGETS_JSON` 配置一致性。
