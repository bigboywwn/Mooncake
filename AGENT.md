# Mooncake 部署与 E2E 验证手册

本文档用于团队共享：在 Linux 构建机/虚拟机上部署 `mooncake`，并验证客户端 `Put/Get`（含 Batch）基础功能。

## 1. 适用范围

- 项目目录：`/Users/miaomili/Documents/Playground/MoonCake-personal`
- 目标：验证 `mooncake_master + client` 基础读写链路
- 当前推荐环境：Linux（macOS 直接运行 Linux 二进制会报 `exec format error`）

## 2. 部署前置

1. 进入 Linux VM（示例）：

```bash
ssh -F ~/.lima/ubuntu-build/ssh.config -o User=root -o ControlMaster=no -o ControlPath=none lima-ubuntu-build
```

2. 进入项目目录：

```bash
cd /Users/miaomili/Documents/Playground/MoonCake-personal
```

3. 编译最小运行与测试目标：

```bash
cmake --build build-remerge --target mooncake_master mooncake_client clientctl client_integration_test -j3
```

## 3. 最小部署（Master）

启动 master（后台）：

```bash
pkill -f mooncake_master || true
rm -f /tmp/mooncake_master.log
./build-remerge/mooncake-store/src/mooncake_master \
  --rpc_port=50051 \
  --rpc_thread_num=2 \
  --enable_ha=false \
  --enable_http_metadata_server=false \
  > /tmp/mooncake_master.log 2>&1 &
```

健康检查：

```bash
nc -z 127.0.0.1 50051
```

## 4. E2E 手工验证（clientctl）

执行 `create -> mount -> put -> get`：

```bash
cat <<'EOF' | ./build-remerge/mooncake-store/tests/e2e/clientctl \
  --master_server_entry=127.0.0.1:50051 \
  --engine_meta_url=P2PHANDSHAKE \
  --protocol=tcp
create c1 51001
mount c1 seg1 67108864
put c1 k1 hello_mooncake
get c1 k1
terminate
EOF
```

预期输出包含：

- `Successfully put value for key: k1`
- `Get value: hello_mooncake`

## 5. E2E 自动化验证（gtest）

### 5.1 单 key Put/Get

```bash
./build-remerge/mooncake-store/tests/client_integration_test \
  --gtest_filter=ClientIntegrationTest.BasicPutGetOperations
```

### 5.2 BatchPut/BatchGet

```bash
./build-remerge/mooncake-store/tests/client_integration_test \
  --gtest_filter=ClientIntegrationTest.BatchPutGetOperations
```

通过标准：测试进程退出码为 `0`，且无 `FAILED` 用例。

## 6. 常用清理

停止 master：

```bash
pkill -f mooncake_master || true
```

查看日志：

```bash
tail -n 200 /tmp/mooncake_master.log
```

## 7. 故障排查速查

1. `exec format error`：
- 原因：在 macOS 直接运行 Linux 二进制。
- 处理：切到 Linux VM 执行。

2. `clientctl` 创建客户端失败：
- 检查 `mooncake_master` 是否在 `127.0.0.1:50051` 监听。
- 检查参数 `--master_server_entry` 是否一致。

3. `put/get` 失败：
- 检查是否已先执行 `mount`。
- 查看 `/tmp/mooncake_master.log` 与 client 输出中的错误码。

## 8. SSD-only 专项验证（模板 + 一键执行）

目标：验证 `enabled_tiers={SSD}` 下客户端 `Put/Get` 与 `BatchPut/BatchGet` 可用。

### 8.1 环境变量模板（legacy，推荐先跑）

```bash
export MC_DDR_POOL_ENABLED=0
export MC_SSD_POOL_ENABLED=1
export MC_NVMEOF_CLIENT_IMPL=legacy
export MC_SSD_POOL_TARGETS=file:///tmp/mooncake-ssd-only.bin
export MC_SSD_POOL_CAPACITY_BYTES=$((8 * 1024 * 1024 * 1024))
export MC_SSD_POOL_BLOCK_SIZE=4096
export MC_SSD_QUEUE_LIMIT=1024
export MC_SSD_IO_TIMEOUT_MS=5000
```

说明：`legacy` 模式用于功能验证，不依赖真实 NVMeoF target。

### 8.2 环境变量模板（SPDK 23.01 + TCP target）

```bash
export MC_DDR_POOL_ENABLED=0
export MC_SSD_POOL_ENABLED=1
export MC_NVMEOF_CLIENT_IMPL=spdk
export MC_SSD_QUEUE_LIMIT=1024
export MC_SSD_IO_TIMEOUT_MS=5000
export MC_SSD_TARGETS_JSON='[
  {
    "name":"t1",
    "trtype":"tcp",
    "traddr":"<TARGET_IP>",
    "trsvcid":"4420",
    "subnqn":"nqn.2026-03.io.mooncake:ssdpool",
    "nsid":1,
    "capacity_bytes":8589934592,
    "weight":1
  }
]'
```

说明：
- 需以 `STORE_USE_SPDK=ON` 编译。
- `nvmeof_client_impl=spdk` 时若 target 不可达，客户端按 fail-fast 失败。

### 8.3 一键执行（SSD-only Put/Get + BatchPut/BatchGet）

```bash
bash -lc '
set -euo pipefail
cd /Users/miaomili/Documents/Playground/MoonCake-personal

# 1) 建议先用 legacy 模式跑通
export MC_DDR_POOL_ENABLED=0
export MC_SSD_POOL_ENABLED=1
export MC_NVMEOF_CLIENT_IMPL=legacy
export MC_SSD_POOL_TARGETS=file:///tmp/mooncake-ssd-only.bin
export MC_SSD_POOL_CAPACITY_BYTES=$((8 * 1024 * 1024 * 1024))
export MC_SSD_POOL_BLOCK_SIZE=4096
export MC_SSD_QUEUE_LIMIT=1024
export MC_SSD_IO_TIMEOUT_MS=5000

pkill -f mooncake_master || true
rm -f /tmp/mooncake_master.log /tmp/mooncake_clientctl_ssd_only.out

./build-remerge/mooncake-store/src/mooncake_master \
  --rpc_port=50051 \
  --rpc_thread_num=2 \
  --enable_ha=false \
  --enable_http_metadata_server=false \
  > /tmp/mooncake_master.log 2>&1 &
MASTER_PID=$!
trap "kill $MASTER_PID 2>/dev/null || true" EXIT

for i in $(seq 1 30); do
  nc -z 127.0.0.1 50051 && break || sleep 1
done
nc -z 127.0.0.1 50051

cat <<'"'"'EOF'"'"' | ./build-remerge/mooncake-store/tests/e2e/clientctl \
  --master_server_entry=127.0.0.1:50051 \
  --engine_meta_url=P2PHANDSHAKE \
  --protocol=tcp \
  > /tmp/mooncake_clientctl_ssd_only.out 2>&1
create c1 51001
mount c1 seg1 67108864
put c1 ssd_only_k1 hello_ssd_only
get c1 ssd_only_k1
terminate
EOF

cat /tmp/mooncake_clientctl_ssd_only.out

./build-remerge/mooncake-store/tests/client_integration_test \
  --gtest_filter=ClientIntegrationTest.BatchPutGetOperations
'
```

通过标准：
- `clientctl` 输出包含 `Get value: hello_ssd_only`
- `client_integration_test` 退出码 `0` 且无 `FAILED` 用例
