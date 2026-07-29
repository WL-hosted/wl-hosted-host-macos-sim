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
- `third_party/lwip`：Host 侧 IPv4、DHCP、DNS 和 ICMP 栈。lwIP
  运行于 `NO_SYS=0` 模式，其 `sys_arch` 复用 `wlh_posix_osal` 的线程、
  队列、semaphore、mutex 和 monotonic time。
- `third_party/mynewt-nimble`：Apache NimBLE 1.10.0 BLE Host（GAP/GATT/SM）。
  其 NimBLE Porting Layer（NPL）由本仓库的 `src/ble/npl` 移植到
  `wlh_posix_osal` 之上，不额外引入直接的 pthread 依赖。
- `third_party/mbedtls`：Mbed-TLS 3.6.6，仅用于 NimBLE Security Manager 的
  AES-CMAC / ECDH（LE Secure Connections）加密原语。
- `third_party/linenoise`：antirez/linenoise（BSD-2），REPL 场景的行编辑/
  历史；非 TTY（管道）输入时自动退化为普通行读取。
- `third_party/cJSON`：DaveGamble/cJSON（MIT），REPL 场景的 JSON Lines
  输出构建与转义。
本仓库的角色固定为 `HOST_SIM`。当通过 `--ipc` 直接连接对端（Manager 或 Coproc Sim）时，会自动启用 sideband 运行时/故障注入通道；当通过 `--usb` 连接真实 Coprocessor 时，只传输标准 WL-hosted wire 帧，sideband 关闭。

## 2. 构建要求

- macOS（开发测试目标平台）
- CMake >= 3.20
- C11 编译器（Clang 或 GCC）
- libusb-1.0（USB 真实设备模式必需）
- lwIP 2.2.1（固定于 `third_party/lwip` 子模块）
- Apache NimBLE 1.10.0、Mbed-TLS 3.6.6
  （分别固定于 `third_party/mynewt-nimble`、`third_party/mbedtls` 子模块；
  BLE 场景必需）
- linenoise、cJSON（分别固定于 `third_party/linenoise`、`third_party/cJSON`
  子模块；REPL 场景必需）

许可证：NimBLE 为 Apache-2.0，Mbed-TLS 为 Apache-2.0 / GPL-2.0
双许可（本项目按 Apache-2.0 使用），linenoise 为 BSD-2-Clause，cJSON 为
MIT。各自完整声明见对应子模块目录下的 `LICENSE` 文件。

安装 libusb：

```sh
brew install libusb
```

首次检出后初始化全部依赖：

```sh
git submodule update --init --recursive
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

编译选项包含 `-Wall -Werror`，错误会被视为构建失败。

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

--scenario smoke|scan|connect|recovery|services|managed|ota|repl|ble-central|ble-peripheral|ble-coexistence
    指定运行场景，默认 connect。managed 仅适用于 IPC 模式；ble-* 仅适用于
    USB 模式（HCI 通道不经 IPC 承载）；repl 两种模式均可用。

--monitor-interval-ms N
    设置 sideband 运行时信息上报间隔，单位毫秒，默认 1000。

--rpc-timeout-ms N
    设置 Host Core 的 RPC 超时时间，单位毫秒，默认 3000。

--ssid SSID
    指定 Wi-Fi 连接使用的 SSID，默认 WPA2Net。

--credential CREDENTIAL
    指定 Wi-Fi 连接使用的密码/凭据，默认 password123。

--ble-bond-store PATH
    指定 BLE bond（配对密钥）持久化文件路径。默认为
    ~/Library/Application Support/WL-hosted/bonds.bin（通过 getpwuid 解析
    home 目录，不依赖 $HOME）。文件权限 0600，写入采用临时文件 + fsync + rename。

--ble-clear-bonds
    启动时清除已保存的有效 bond（以及临时文件），随后从空 bond 存储开始。
    不会删除无法识别版本的备份文件。

--ble-io-cap no-io|display|keyboard|display-yes-no
    指定 BLE 配对时本机的 IO 能力，默认 no-io（Just Works，适合自动化）。

--ble-passkey N
    指定 6 位数字 passkey（000000-999999），用于需要输入 passkey 的配对。

--ble-peer-address ADDR
    指定要连接的对端地址，形如 public:AA:BB:CC:DD:EE:FF 或
    random:AA:BB:CC:DD:EE:FF。未指定时按测试服务 UUID / 设备名扫描匹配。

--ble-timeout-ms N
    覆盖 BLE 场景中各步骤的等待超时，单位毫秒。
```

## 6. 内置场景说明

除 `managed` 与 `repl` 自行管理 READY 等待外，其余场景都会先等待 Host Core 进入 `READY` 状态（Hello 协商完成）。

| 场景 | 行为 |
|------|------|
| `smoke` | 仅验证链路可达并进入 READY，不做任何 RPC。 |
| `recovery` | 在 READY 后人为触发 `wlh_host_transport_lost`，验证 Core 能在 2 秒内离开 READY，并在 5 秒内重新恢复 READY。 |
| `scan` | 初始化 Wi-Fi，执行一次 BSS 扫描，等待 `WLH_HOST_EVENT_WIFI_SCAN_COMPLETED` 事件。 |
| `connect` | 在 `scan` 的基础上，使用 `--ssid`/`--credential` 连接指定 AP，等待 `WLH_HOST_EVENT_WIFI_CONNECTED`，发送 Ethernet echo 帧并等待 `WLH_HOST_EVENT_ETHERNET_STA_RX`，最后断开并等待 `WLH_HOST_EVENT_WIFI_DISCONNECTED`。 |
| `services` | 调用 Device Information 服务获取厂商/板级/UID 信息，并发送一条 User Passthrough 消息等待 completion；还会短暂等待可选的 `USER_MESSAGE_RESULT` 事件。 |
| `managed` | Manager 驱动模式：等待 READY 后自动执行一次 Wi-Fi INITIALIZE（对端已初始化时容忍失败），随后长期驻留，通过 sideband 接收 Manager 下发的 `SIM_RECORD_WIFI_COMMAND`（scan / connect / disconnect / start_ap / stop_ap）并转换为标准 Wi-Fi RPC；也可接收 `SIM_RECORD_PING_COMMAND`，由 `NO_SYS=0` lwIP 通过 STA Ethernet 数据面完成 DHCP、DNS 和 ICMP ping 并返回 `SIM_RECORD_PING_RESULT`。链路断开后自动重新等待 READY 并重新 INITIALIZE，直到收到退出信号。仅 IPC + sideband 模式有效；USB `--usb` 独立运行模式行为不变。 |
| `repl` | 交互式 REPL：从 stdin 逐行读取命令，stdout 输出 JSON Lines（详见 §6.1）。IPC 与 USB 模式均可用；每条命令自行等待 READY，链路断开时 REPL 存活，异步 state 事件反映恢复过程。 |
| `ble-central` | 仅 USB。启动 NimBLE Host（GET_INFO → INITIALIZE(HCI) → ENABLE(LE) → NPL/pools → host task → sync），扫描（或直连 `--ble-peer-address`）测试对端，建立连接并发起 LE Secure Connections 配对，发现测试服务/特征/CCCD，订阅通知，写入 `ping`，验证收到 `pong` 通知并读取校验为 `pong`，最后断开。第二次运行会复用已持久化的 bond 直接加密，无需重新配对。 |
| `ble-peripheral` | 仅 USB。以测试服务广播为 peripheral，接受中心设备连接与配对，处理 `ping`/`pong` GATT 事务并发送通知；对端断开后继续广播，等待下一次连接。 |
| `ble-coexistence` | 仅 USB。先完成 WPA2 连接与 DHCP，并启动对 `one.one.one.one` 的持续 DNS/ICMP 健康检查；在 Wi-Fi 保持连接的同时执行完整的 ble-central 事务；BLE 结束后 Wi-Fi 仍须在线且再完成 ≥10 次 ping，验证 BLE 与 Wi-Fi 数据面并发共存。 |

### 6.1 REPL JSON Lines 契约

`--scenario repl` 从 stdin 逐行读取命令，stdout 只输出 JSON Lines：每行一个
JSON 对象，固定携带 `"source":"wlh-host-sim"` 与 `"event"` 字段；日志全部走
stderr，stdout 可直接接管道逐行 `json.loads`。TTY 下由 linenoise 提供
`wlh> ` 提示符、行编辑与历史（上箭头），Ctrl-C/Ctrl-D 干净退出；管道（非
TTY）输入时无提示符，读到 EOF 后自然退出。

命令（参数含空格时用双引号包裹；`user-message` 取命令名后的原始剩余文本）：

| 命令 | 模式 | 说明 |
|------|------|------|
| `scan [ssid]` | 两者 | 惰性执行 Wi-Fi INITIALIZE 后扫描；每个网络输出一行 `scan_result` 事件。 |
| `connect <ssid> [credential]` | 两者 | 连接 AP；无凭据按 Open，有凭据按 WPA2-PSK。 |
| `disconnect` | 两者 | 断开当前 AP。 |
| `status` | 两者 | 输出 Host 状态、session、帧计数、连接状态与运行时长。 |
| `device-info` | 两者 | 查询 Device Information 服务（UID 以十六进制输出）。 |
| `user-message <text>` | 两者 | 发送 User Passthrough 消息；可选 RESULT 事件另行输出。 |
| `eth-echo` | 仅 IPC | 发送测试 Ethernet 帧并等待 mock coprocessor 回环。 |
| `ping <host> [count] [timeout_ms]` | 两者 | 经 lwIP STA 数据面执行 DNS + ICMP ping（count 1-10，默认 1；超时默认 2000ms）。 |
| `iperf tcp client <IPv4> [duration_sec]` | 两者 | Host Sim 主动 TCP 发送至 Mac iPerf2 server（默认 30 秒）。 |
| `iperf tcp server [duration_sec]` | 两者 | Host Sim TCP 接收 Mac iPerf2 client。 |
| `iperf udp client <IPv4> [duration_sec] [mbps]` | 两者 | Host Sim 按 iPerf2 UDP 格式发送（默认 30 秒、20 Mbps）。 |
| `iperf udp server [duration_sec]` | 两者 | Host Sim 接收 iPerf2 UDP 并报告丢包、乱序和 jitter。 |
| `ble central\|peripheral` | 仅 USB | 同步运行完整 BLE 场景（阻塞至结束），选项沿用 `--ble-*` 参数。 |
| `help` / `quit` / `exit` | 两者 | 列出命令 / 退出。 |

事件流：命令结束输出 `{"event":"result","command":"…","result":N}`（N 为
`wlh_host_result_t` 数值，0 为成功）；失败前先输出 `error` 事件，携带
`result`、`status_domain`/`status_code` 或 `detail`。异步事件（`state`、
`scan_result`、`scan_complete`、`wifi_connected`、`wifi_disconnected`、
`user_message_result`、`bluetooth_state`、`ping_result`、`protocol_fault`）
随时可能插入。会话以 `repl_ready` 开始、`repl_exit`（reason 为
`quit|eof|signal|transport`）结束。不适用当前传输模式的命令返回
`NOT_SUPPORTED (-11)` 的 error 行。

SSID/payload 为任意字节串：合法 UTF-8 原样输出；否则字段内容替换为可打印
形式，并附加 `<key>_hex` 字段携带原始字节的十六进制。

`status` 在 DHCP 完成后带有 `dhcp_ipv4`，供 Mac 侧 client 使用。iPerf 使用
iPerf2 端口 5001（不兼容 iPerf3）：先在 Mac 执行 `brew install iperf`，并先启动
server。四项手工验证矩阵为：`iperf -s -i 3` + `iperf tcp client <mac-ip> 30`；
`iperf -c <dhcp-ip> -t 30 -i 3` + `iperf tcp server 30`；`iperf -u -s -i 3` +
`iperf udp client <mac-ip> 30 20`；以及 `iperf -u -c <dhcp-ip> -t 30 -i 3 -b 20M`
`iperf udp server 30`。REPL 会输出 `iperf_started`、每 3 秒的
`iperf_interval` 和最终 `iperf_result` JSON 行。

管道驱动示例：

```sh
printf 'scan\nconnect WPA2Net password123\neth-echo\nping one.one.one.one\ndisconnect\nquit\n' \
  | ./build-debug/wlh-host-macos-sim --ipc connect:/tmp/host.sock --scenario repl 2>/dev/null
```

已知限制：TTY 交互中异步事件行可能与正在编辑的行交错；管道驱动的自动化场景
无此问题。

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

## 9.1 BLE 栈

BLE 仅在 USB 模式下可用，HCI 通过标准 WL-hosted Bluetooth 通道（service `0x0003`
/ channel `0x04`）与真实 Coprocessor 交互，不使用任何 sideband。相关源码位于
`src/ble/`：

- `npl/`：NimBLE Porting Layer，完全建立在 `wlh_posix_osal` 之上（event queue、
  callout、mutex、semaphore、critical section、tick 换算）。该目录不含直接的
  pthread 调用。`syscfg/` 覆盖 NimBLE 的固定配置（4 连接、16 bond、64 CCCD、
  MTU 247、SC only、16/16 HCI/ACL 槽）。
- `hci_transport.c`：把 NimBLE transport 框架桥接到 Core 的 HCI 通道。TX 在
  `WLH_HOST_NO_CREDIT` 时保留 NimBLE buffer 所有权并入有界 pending 队列，待
  `hci_tx_ready` 回调在 NimBLE host task 上重试；RX 使用单生产者/单消费者环形
  缓冲，环满时回撤 credit 让 Coprocessor 反压，transport pool 耗尽时经 callout
  重试并在 buffer 回收后恢复。
- `bond_store.c`：文件持久化的 `ble_store`。记录含 magic/version/count/length/
  checksum；写入采用临时文件 + fsync + rename，权限 0600；bond 超限时按 LRU
  淘汰最旧且未连接的对端；无法识别的版本不会被覆盖；文件损坏时重命名为
  `.corrupt.<time>` 并从空存储启动。
- `ble_app.c`：启动/停止流程与 central/peripheral 场景驱动。测试 GATT 服务
  UUID 为 `7f510000-1b15-4f0d-8a61-4c574c480001`，特征
  `7f510001-…`（Read/Write/Notify，加密访问；启用 MITM 时要求鉴权）。中心写入
  `ping`，peripheral 将值置为 `pong` 并发送通知。

启动流程：READY → 校验 service `0x0003` + channel `0x04` → GET_INFO →
INITIALIZE(HCI) → ENABLE(LE) → 初始化 NPL 与 transport pools → 启动 host task →
等待 sync → 运行场景。停止流程：停止扫描/广播 → 断开 → 停止 host task →
detach transport → DISABLE → DEINITIALIZE。USB 断开时执行完整 teardown（保留
bond 存储）并整体重启。

## 10. 测试

仓库自带的 CTest 包括：

- `wlh_host_sim_ipc`：`tests/test_ipc.c` 使用 `socketpair` 验证 IPC hello 握手、record 读写、角色与 sideband 标志解析。
- `wlh_host_sim_npl`：`tests/test_npl.c` 验证 NimBLE Porting Layer over `wlh_posix_osal` 的行为：event 队列 FIFO 顺序、callout 取消/重排、单次定时器到期、semaphore pend 超时、阻塞消费者唤醒（停止路径）、反复 start/stop 循环、毫秒/tick 换算。
- `wlh_host_sim_repl_json`：`tests/test_repl_json.c` 验证 REPL 的 JSON Lines 输出层：敌意字节串（引号/反斜杠/控制符/非法 UTF-8/内嵌 NUL）产出的每行都必须是合法 JSON，非法 UTF-8 必须附带 `_hex` 回退字段。

运行：

```sh
ctest --test-dir build-debug --output-on-failure
```

`wlh_posix_osal` 的一致性门在 Common 子模块的 CTest 中维护，修改 OSAL 相关代码时应同时验证 Common 的测试。

## 11. 格式化

本仓库遵循工作区统一的 `.clang-format`。不要手动格式化，应从工作区根目录运行：

```sh
./wl-hosted-tools/auto_format.sh
```

该脚本会格式化 Protocol、Common、两个 Core、两个 Sim 和 Manager 中的 C/C++ 文件，并排除 submodule、`third_party`、生成的 `*.pb.*`、构建目录和 Rust `target`。

## 12. 提交与子模块

本目录是独立 Git 仓库。修改后应单独提交，不要在工作区根目录执行全局 `git` 操作。

本仓库依赖的子模块信息记录在：

- `.gitmodules`
- 各子模块 gitlink（`core/`、`third_party/lwip`、`third_party/easylogger`、
  `third_party/mynewt-nimble`、`third_party/mbedtls`、`third_party/linenoise`、
  `third_party/cJSON`）
- `SUBMODULE.lock`

更新子模块后应同步 `SUBMODULE.lock` 中的完整 40 字符 commit SHA，并确保每个
子模块的 `*.commit` 与对应 gitlink 一致。

完成后执行：

```sh
git submodule update --init --recursive
git submodule status --recursive
```

未经授权不要 push、创建 PR 或改写远端历史。
