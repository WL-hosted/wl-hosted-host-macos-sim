# WL-hosted Host macOS Simulator

`wl-hosted-host-macos-sim` 是 WL-hosted 协议栈在 macOS 上的 Host 端模拟器。它基于 POSIX 线程、条件变量和单调时钟，把平台相关的部分适配到 WL-hosted Host Core，并提供场景化（scenario）运行器，用于在桌面端验证 Host Core 的链路建立、Wi-Fi 扫描、连接、Ethernet 透传、服务调用以及故障恢复等行为。

本仓库仅包含 macOS/POSIX 适配层与可执行程序，平台无关的核心逻辑位于 `core/host-core`；`core/` 是 `wl-hosted-core` 单一子模块。

## 1. 仓库定位

在 WL-hosted 多仓库工作区中，各仓库的职责边界如下：

```text
wl-hosted-host-macos-sim -> wl-hosted-core/host-core
                           -> wl-hosted-core/protocol
                           -> wl-hosted-core/common
```

- `core/host-core`：平台无关的 Host Core，包含状态机、RPC 超时、credit 管理、Hello 协商等。
- `core/protocol`：标准 Wire/RPC codec、protobuf/nanopb schema、Simulator IPC sideband schema。
- `core/common`：共享平台契约，OSAL 唯一来源位于 `osal/include/wlh/osal.h`；`wlh_posix_osal` 由 Common 提供，供本仓库启用。

本仓库的角色固定为 `HOST_SIM`。当通过 `--ipc` 直接连接对端（Manager 或 Coproc Sim）时，会自动启用 sideband 运行时/故障注入通道；当通过 `--usb` 连接真实 Coprocessor 时，只传输标准 WL-hosted wire 帧，sideband 关闭。

## 2. 构建要求

- macOS（开发测试目标平台）
- CMake >= 3.20
- C11 编译器（Clang 或 GCC）
- libusb-1.0（USB 真实设备模式必需）

安装 libusb：

```sh
brew install libusb
```

## 3. 构建步骤

推荐 out-of-tree 构建，并打开测试：

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-debug --parallel
ctest --test-dir build-debug --output-on-failure
```

如需 Release 构建：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-release --parallel
ctest --test-dir build-release --output-on-failure
```

公共接口、OSAL、并发或生命周期相关改动，还应额外运行 sanitizer 构建：

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
    -DCMAKE_C_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

编译选项包含 `-Wall -Wextra -Wpedantic -Werror`，错误会被视为构建失败。

## 4. 运行方式

构建产物为 `build-debug/wlh-host-macos-sim`（或对应构建目录）。程序支持两种传输后端：IPC 模拟后端 和 USB 真实设备后端。

### 4.1 IPC 模拟后端

通过 Unix domain socket 连接到 Manager 或另一端的 Coproc Sim，传输带 record 层的 Simulator IPC 帧，同时可上报运行时信息并接受故障注入。

```sh
./build-debug/wlh-host-macos-sim --ipc connect:/path/to/host.sock --scenario connect
```

`--ipc` 支持两种 endpoint 形式：

- `connect:PATH`：主动连接到指定 Unix socket 路径。
- `fd:N`：使用已继承的文件描述符（通常由 Manager 创建 `socketpair` 后传入）。

### 4.2 USB 真实设备后端

直接通过 libusb 连接符合 `espressif.esp32s3.coreboard.usb-wifi` 配置文件的 ESP32-S3 Coprocessor，传输原始 WL-hosted wire 帧，不经过 Simulator IPC record 层，也不启用 sideband。

```sh
./build-debug/wlh-host-macos-sim --usb 303A:8201 --scenario connect \
    --ssid MyAp --credential MyPassphrase
```

默认 VID:PID 为 `303A:8201`。USB 模式下：

- Bulk OUT 用于 Host -> Coprocessor，Bulk IN 用于 Coprocessor -> Host。
- 帧边界由 24 字节 wire header 决定，而不是 USB packet 边界。
- 检测到总线断开时会调用 `wlh_host_transport_lost`，由 Core 执行停止、有限重连等待、重新启动并重新协商 Hello 的恢复流程。
- Ethernet echo 步骤会被跳过，因为真实设备会把帧转发到 AP，而不是回环 echo。

## 5. 命令行参数

```text
--ipc connect:PATH|fd:N
    指定 IPC 后端 endpoint。与 --usb 二选一。

--usb VID:PID
    指定 USB 后端 VID:PID（十六进制）。与 --ipc 二选一。

--scenario smoke|scan|connect|recovery|services|managed
    指定运行场景，默认 connect。managed 仅适用于 IPC 模式。

--monitor-interval-ms N
    设置 sideband 运行时信息上报间隔，单位毫秒，默认 1000。

--rpc-timeout-ms N
    设置 Host Core 的 RPC 超时时间，单位毫秒，默认 3000。

--ssid SSID
    指定 Wi-Fi 连接使用的 SSID，默认 WPA2Net。

--credential CREDENTIAL
    指定 Wi-Fi 连接使用的密码/凭据，默认 password123。
```

## 6. 内置场景说明

所有场景都会先等待 Host Core 进入 `READY` 状态（Hello 协商完成）。

| 场景 | 行为 |
|------|------|
| `smoke` | 仅验证链路可达并进入 READY，不做任何 RPC。 |
| `recovery` | 在 READY 后人为触发 `wlh_host_transport_lost`，验证 Core 能在 2 秒内离开 READY，并在 5 秒内重新恢复 READY。 |
| `scan` | 初始化 Wi-Fi，执行一次 BSS 扫描，等待 `WLH_HOST_EVENT_WIFI_SCAN_COMPLETED` 事件。 |
| `connect` | 在 `scan` 的基础上，使用 `--ssid`/`--credential` 连接指定 AP，等待 `WLH_HOST_EVENT_WIFI_CONNECTED`，发送 Ethernet echo 帧并等待 `WLH_HOST_EVENT_ETHERNET_STA_RX`，最后断开并等待 `WLH_HOST_EVENT_WIFI_DISCONNECTED`。 |
| `services` | 调用 Device Information 服务获取厂商/板级/UID 信息，并发送一条 User Passthrough 消息等待 completion；还会短暂等待可选的 `USER_MESSAGE_RESULT` 事件。 |
| `managed` | Manager 驱动模式：等待 READY 后自动执行一次 Wi-Fi INITIALIZE（对端已初始化时容忍失败），随后长期驻留，通过 sideband 接收 Manager 下发的 `SIM_RECORD_WIFI_COMMAND`（scan / connect / disconnect / start_ap / stop_ap）并转换为标准 Wi-Fi RPC；链路断开后自动重新等待 READY 并重新 INITIALIZE，直到收到退出信号。仅 IPC + sideband 模式有效；USB `--usb` 独立运行模式行为不变。 |

## 7. 架构与线程模型

本仓库把 Host Core 所需的 OSAL、buffer、executor、transport 四类操作以 C 结构体回调形式注入：

- **OSAL**：启用 Common 提供的 `wlh_posix_osal`，包含互斥锁、条件变量、单调时钟定时器。
- **Buffer**：由主程序分配/释放，支持通过 `--fail-allocations`（内部测试钩子）模拟分配失败。
- **Executor**：`sim_executor_t` 是一个 64 槽有界单线程任务队列，用于执行 Core 提交的工作项。
- **Transport**：生命周期与发送操作都异步提交到独立的 TX executor，避免在 Core 回调上下文中阻塞或执行 I/O。

线程分布：

- 主线程：初始化、启动 Host Core、运行 scenario 逻辑、条件变量等待状态变化。
- TX executor 线程：执行 transport 的 start/stop 与 frame 发送。
- RX 线程：IPC 模式下读取 IPC record，解析为标准帧或 sideband 故障请求后提交给 Core；USB 模式下由 `transport_usb.c` 内部维护独立的 bulk IN 接收线程。

Core 不会周期性 poll；所有等待都基于条件变量或 Core 给出的最近 RPC/心跳 deadline。

## 8. Sideband 运行时信息

在 IPC 模式下，如果对端是 Manager，程序会周期性通过 `SIM_RECORD_RUNTIME_INFO` 上报：

- 当前角色（固定 `SIM_ROLE_HOST`）
- 链路状态（negotiating / recovering / up）
- session_id
- 运行时长
- TX/RX 帧数与丢帧数
- 实现名称与版本

这些信息仅用于测试/监控，不属于标准 WL-hosted wire 协议。

## 9. 故障注入

IPC + Manager 模式下，程序可接受 Manager 发来的 `SIM_RECORD_FAULT_REQUEST`，支持以下 fault kind：

| Fault | 作用 |
|-------|------|
| `HOST_RESET` | 调用 `wlh_host_transport_lost`，触发一次 transport 恢复。 |
| `CLEAR_CREDIT` / `LIMIT_CREDIT` | 将指定 channel 的 credit 清零，测试反压与重传。 |
| `RPC_TIMEOUT` | 调用 `wlh_host_test_expire_all`，强制所有 pending RPC 超时。 |
| `BUFFER_OOM` | 让接下来 N 次 buffer 分配返回 NULL，测试内存耗尽路径。 |
| `QUEUE_STARVATION` | 在 RX worker 中睡眠指定毫秒，模拟队列饥饿/延迟。 |

收到请求后会回复 `SIM_RECORD_FAULT_RESPONSE`，包含是否接受及简要说明。

## 10. 测试

仓库自带的 CTest 包括：

- `wlh_host_sim_ipc`：`tests/test_ipc.c` 使用 `socketpair` 验证 IPC hello 握手、record 读写、角色与 sideband 标志解析。

运行：

```sh
ctest --test-dir build-debug --output-on-failure
```

`wlh_posix_osal` 的一致性门在 Common 子模块的 CTest 中维护，修改 OSAL 相关代码时应同时验证 Common 的测试。

## 11. 格式化

本仓库遵循工作区统一的 `.clang-format`。不要手动格式化，应从工作区根目录运行：

```sh
./auto_format.sh
```

该脚本会格式化 Protocol、Common、两个 Core、两个 Sim 和 Manager 中的 C/C++ 文件，并排除 submodule、`third_party`、生成的 `*.pb.*`、构建目录和 Rust `target`。

## 12. 提交与子模块

本目录是独立 Git 仓库。修改后应单独提交，不要在工作区根目录执行全局 `git` 操作。

本仓库依赖的 `core` 子模块信息记录在：

- `.gitmodules`
- `core/` gitlink
- `SUBMODULE.lock`

更新子模块后应同步 `SUBMODULE.lock` 中的 commit，并确保：

- `core.commit` 与 gitlink 一致。

完成后执行：

```sh
git submodule update --init --recursive
git submodule status --recursive
```

未经授权不要 push、创建 PR 或改写远端历史。
