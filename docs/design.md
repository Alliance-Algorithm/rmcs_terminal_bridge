# rmcs_terminal_bridge · 设计

> 本文件属于 `rmcs_terminal_bridge` 仓，不属于 Alliance-Terminal；路径 `rmcs_terminal_bridge/docs/design.md`。
>
> 状态：v0.3（D0 骨架 + vendored `teleop_ipc`；D1+ 未开始）
> 日期：2026-11-20
> 线格式契约：本仓 [`docs/ipc-protocol.md`](ipc-protocol.md)（与 `Alliance-Terminal/docs/ipc-protocol.md` 同内容；修改必须同步对端）
> 关联：RMCS 的 `rmcs_executor::Component`、`rmcs_bringup` 的 executor YAML、`rmcs-runtime` 镜像
>
> **文档约定**
> - 本仓文档只放 `docs/`；不得把文档直接放在 `~/Workspace/Alliance/`。
> - `docs/ipc-protocol.md` 是与客户端的共用文档：两边各一份、内容一致；头部有版本/日期/正文 SHA256；修改必须同步对端。
> - `docs/design.md`（本文）只属于桥仓。

---

## 0. 一页结论

| 项 | 结论 |
|---|---|
| 定位 | RMCS 侧的 ROS2 包，原 AT-Bridge；作为 `rmcs_executor::Component` 插件加载，UDS **server** |
| 组件 | **两个**：`TerminalBridgeRx`（owner，接收/下行）+ `TerminalBridgeTx`（partner，发送/上行），共享一个 `Link` 对象 |
| 为什么拆 | 单组件同时注册 input/output 会与作者的"既消费又生产"组件形成 executor 循环依赖 |
| 上行 | 作者 `CustomControlData` → Tx → 组 `CustomControl` 官方 envelope → `UpData` → UDS → 节点 → MQTT |
| 下行 | MQTT → 节点 → `DownData` → Rx → 解 `CustomByteBlock` 官方 envelope → 作者 `CustomByteBlockData` |
| 内层协议 | **不归桥管**；桥只检查 0–60 / 0–300 的长度，内容原样搬运 |
| 依赖 | 仓内 vendor `third_party/teleop_ipc`（Apache-2.0 快照；手动同步，构建不访问 Alliance-Terminal） |
| 许可证 | GPL-3.0-only（与 RMCS 一致；链接 `rmcs_executor`） |
| 部署 | `rmcs-runtime`（root）：宿主目录 0700、socket 0666、桥 `allowed_uid=1000`；节点 `allowed_peer_uid=0` |
| 集成 | 子模块 `rmcs_ws/src/rmcs_terminal_bridge`；插件描述导出到 `rmcs_executor`；operator 的 YAML 加一行 |
| 联调 | **先于客户端 B3/B4**：客户端 `ipc_probe` + 本仓 `bridge_link_test` / `TerminalTestSource`/`Sink`，分 Link → RMCS executor → rmcs-runtime 三级 |

---

## 1. 角色与三层边界

### 1.1 三层

| 层 | 内容 | 谁组/解 |
|---|---|---|
| 数据段 | `CustomControl.data` / `CustomByteBlock.data`，0–60 / 0–300 B | 自定义控制作者（RMCS 侧的 producer/consumer 组件） |
| 官方 envelope | 序列化后的 `CustomControl` / `CustomByteBlock` | **本桥** |
| IPC envelope | `UpData` / `DownData` + 32 B 帧头 | 本桥与节点共用 `teleop_ipc` |
| MQTT | 官方 envelope 裸字节 | `teleop-node` 原样收发 |

### 1.2 谁解析什么

| 环节 | 数据段 | 官方 envelope | IPC envelope |
|---|---|---|---|
| 作者组件 | **组/解** | 不碰 | 不碰 |
| 本桥 | 只搬运、只查长度 | **组/解** | 组/解（用 `teleop_ipc`） |
| `teleop-node` | 不碰 | **不碰**（只当字节） | 解/组 |
| 机器人侧 | **组/解** | 不碰 | 不碰 |

### 1.3 与 Alliance-Terminal 的契约

- 线格式以本仓 [`docs/ipc-protocol.md`](ipc-protocol.md) 为准（与客户端副本同内容）；本桥不重新定义协议。
- 客户端库 `teleop_ipc` 自包含、Apache-2.0、header-only；本桥只 link 它，不修改它。
- 客户端不解析官方 envelope；本桥是唯一组/解 `CustomControl` / `CustomByteBlock` 的地方。

---

## 2. 拓扑

```text
┌────────────────────────────── RMCS 进程（root，rmcs-runtime） ──────────────────────────────┐
│                                                                                            │
│  A: 自定义控制组件（作者）                                                                  │
│    OutputInterface<CustomControlData>  /terminal/custom_control                            │
│    InputInterface<CustomByteBlockData> /terminal/custom_byte_block                         │
│         │                                    ▲                                             │
│         ▼                                    │                                             │
│  TerminalBridgeTx（partner）          TerminalBridgeRx（owner）                            │
│    InputInterface<CustomControlData>    OutputInterface<CustomByteBlockData>               │
│         │                                    ▲                                             │
│         └────────────── shared_ptr<Link> ────┘                                             │
│                        │                                                                   │
│                        ▼                                                                   │
│              UDS SEQPACKET（32 B 帧头，BE）                                                │
└────────────────────────┬───────────────────────────────────────────────────────────────────┘
                         │ /home/<user>/.local/state/teleop/run/rmcs/control.sock
                         ▼
                  teleop-node（宿主，uid 1000）
                         │ MQTT
                         ▼
                  服务器 / 机器人
```

依赖图（executor 拓扑）：

```text
A.out ──▶ Tx.in        （Tx 依赖 A）
Rx.out ──▶ A.in        （A 依赖 Rx）
Rx ──shared_ptr<Link>──▶ Tx   （不是 executor 接口，不算依赖边）

=> Rx → A → Tx
```

- 只要 A 同时消费下行、生产上行，拓扑保证 **Rx 先跑、A 次之、Tx 最后**：下行同周期到达 A，上行同周期发出。
- 握手 `HELLO_ACK` 由 Rx 排队、Tx 发送；最坏等 A 的一个 1 ms 周期，2 s 超时内无影响。

---

## 3. 包结构

```text
rmcs_terminal_bridge/
├─ package.xml                  GPL-3.0-only；depend rmcs_executor/rclcpp/pluginlib
├─ CMakeLists.txt               colcon/ament；add_subdirectory(third_party/teleop_ipc)；pluginlib 导出
├─ third_party/teleop_ipc/      vendor 快照（Apache-2.0；见 ORIGIN.md）
├─ plugins.xml                  生产：只导出 TerminalBridgeRx
├─ include/rmcs_terminal_bridge/
│  ├─ types.hpp                 数据段类型（作者包也 include 这个）
│  ├─ link.hpp                  无 ROS 的核心：Listener/Session/codec/节奏/计数
│  ├─ terminal_bridge_rx.hpp    owner 组件
│  └─ terminal_bridge_tx.hpp    partner 组件
├─ src/
│  ├─ link.cpp
│  ├─ payload_codec.cpp         官方 CustomControl/CustomByteBlock 编解码
│  └─ component.cpp             PLUGINLIB_EXPORT_CLASS(TerminalBridgeRx)
├─ config/
│  ├─ terminal_bridge.yaml      生产配置片段（operator YAML 里复制）
│  └─ terminal_test.yaml        测试 executor 配置（桥 + 测试源/汇）
├─ test/
│  ├─ test_payload_codec.cpp    golden bytes
│  ├─ test_link.cpp             socketpair + 假时钟
│  ├─ bridge_link_test.cpp      无 ROS 的 Link 服务端，供客户端 ipc_probe 连
│  ├─ source_sink.cpp           TerminalTestSource / TerminalTestSink（测试组件）
│  └─ plugins-test.xml          测试插件的 pluginlib 描述（不进生产 YAML）
├─ docs/
│  ├─ design.md                 本文
│  └─ ipc-protocol.md           与客户端共用的线格式副本
├─ README.md
└─ .github/workflows/ci.yml     RMCS 镜像 + colcon + gtest
```

- `link` 不依赖 ROS，测试不需要起 `rmcs_executor`。
- `TerminalBridgeRx` 继承 `Component` + `rclcpp::Node`（读参数）；`TerminalBridgeTx` 只继承 `Component`（无参数）。
- `TerminalBridgeTx` 不导出插件；由 Rx 用 `create_partner_component` 创建。
- `TerminalTestSource`/`Sink` 只在 `config/terminal_test.yaml` 里出现，生产 YAML 不加载；`plugins-test.xml` 单独导出。
- `docs/ipc-protocol.md` 与 `Alliance-Terminal/docs/ipc-protocol.md` 同内容；头部有版本/日期/正文 SHA256；修改必须同步对端。

---

## 4. 接口契约

### 4.1 数据段类型（`types.hpp`）

```cpp
namespace rmcs_terminal_bridge {

inline constexpr std::size_t kCustomControlMaxBytes = 60;
inline constexpr std::size_t kCustomByteBlockMaxBytes = 300;

struct CustomControlData {
    std::array<std::byte, kCustomControlMaxBytes> data{};
    std::uint16_t size{0};
};

struct CustomByteBlockData {
    std::array<std::byte, kCustomByteBlockMaxBytes> data{};
    std::uint16_t size{0};
};

} // namespace rmcs_terminal_bridge
```

- `size` 之外的内容不读；`size=0` 按"存在但为空"编码（官方 `0A 00`）。
- 桥包导出 `include/`；作者包 `find_package(rmcs_terminal_bridge REQUIRED)` 后 include。
- 作者包不需要链接桥的逻辑，只需要类型头；但 `package.xml` 依赖桥包以满足构建顺序。

### 4.2 executor 接口

| 名字 | 方向 | 类型 | 注册方 | 作者侧 |
|---|---|---|---|---|
| `/terminal/custom_control` | 作者 output → 桥 input | `InputInterface<CustomControlData>` | `TerminalBridgeTx` | `OutputInterface<CustomControlData>` |
| `/terminal/custom_byte_block` | 桥 output → 作者 input | `OutputInterface<CustomByteBlockData>` | `TerminalBridgeRx` | `InputInterface<CustomByteBlockData>` |
| `/terminal/custom_byte_block/meta` | 桥 output（可选） | `{u64 seq; i64 t_recv_ns;}` | `TerminalBridgeRx` | 可选绑定 |
| `/terminal/status` | 桥 output（可选） | 连接/计数 | `TerminalBridgeRx` | 可选绑定 |

- 桥的 input 是 **optional**：作者组件没加载时跳过上行，不阻止 RMCS 启动；`status.control_source_ready=false`。
- 桥的 output 总是注册；作者不绑也不影响。
- 作者组件可以是一个（同时有 output/input），这正是拆 Rx/Tx 的原因。

### 4.3 作者侧组件

```cpp
class ControlSource : public rmcs_executor::Component {
public:
    ControlSource() {
        register_output("/terminal/custom_control", out_);
        // 下游
        register_input("/terminal/custom_byte_block", in_, false);
    }
    void update() override {
        // 1. 组内层数据段（作者自己的协议）
        CustomControlData packet;
        packet.size = ...;
        // 2. 写到 output
        *out_ = packet;
        // 3. 读下行
        if (in_.ready()) { /* 解析 in_->data / in_->size */ }
    }
private:
    OutputInterface<CustomControlData> out_;
    InputInterface<CustomByteBlockData> in_;
};
```

- 内层格式、字段、组包/解包逻辑全在作者这里；桥不 include 作者的协议头。
- 作者的 `package.xml` 依赖 `rmcs_terminal_bridge`；`include <rmcs_terminal_bridge/types.hpp>`。

---

## 5. 组件行为

### 5.1 生命周期

| 阶段 | `TerminalBridgeRx` | `TerminalBridgeTx` |
|---|---|---|
| 构造 | 读参数；建 `Link`；注册 output；`create_partner_component<TerminalBridgeTx>(name + "_tx", link_)` | 拿 `shared_ptr<Link>`；注册 input |
| `before_updating` | `link_->open()`（unlink/bind/listen/chmod）；失败只 log | input 没 ready 时 log 一次；不绑定默认值 |
| `update` | `poll_recv(now)`；`take_down` 写 output/meta/status | `poll_send(now, up)` |
| 析构 | 关闭 socket；删除 socket 文件（尽力） | 无 |

### 5.2 `update()` 非阻塞

- 所有 socket 调用都是非阻塞；`EAGAIN` 不是错误。
- Rx：每个 update 最多 accept 一次，recv drain 到 `EAGAIN`，最多 8 帧；`take_down` 只取最新一块。
- Tx：每个 update 最多发 1 帧 `UP_DATA`、1 帧 `HEALTH`，以及待发的 `HELLO_ACK`。
- 不另开线程；`Link`/`Session` 只在 executor 更新线程访问；无锁。
- `update()` 不抛异常；错误转为计数 + 限流日志（`RCLCPP_WARN_THROTTLE`）。

### 5.3 握手、版本、超时

- 桥是 acceptor：等 `HELLO`；校验帧头版本和 `Hello.proto_version`；合法则排 `HELLO_ACK`，由 Tx 发送。
- 版本不匹配：回带自己版本的 `HELLO_ACK`，然后关闭；日志打印双方版本。
- 握手超时 2 s（`handshake_timeout_ms`）；超时关闭并计数。
- 未知 `msg_type`：跳过并计数；帧/长度异常：关闭并计数。
- 新连接到来时替换旧连接（节点重启场景），计数。

### 5.4 75 Hz / 健康 / 丢帧

- 用 `/predefined/timestamp`（`std::chrono::steady_clock::time_point`）驱动；`next_send_ns += 1e9/75`；大停顿后 resync，不补发。
- 1 kHz executor 下 13/14 ms 交替，平均 75 Hz，相位抖动 ≤1 ms。
- `send` 返回 `EAGAIN`：`UP_DATA`/`HEALTH` 丢弃、计数、消耗 seq；`HELLO_ACK` 保留同 seq 下个周期重试。
- `Health` 1 Hz；`state` 位：bit0 桥运行、bit1 客户端在线、bit2 控制源就绪；`load_pct` 先 0。
- 下行 `DOWN_DATA` drain：每帧解 `CustomByteBlock`，只保留最新一块给 output；`seq`/`t_recv_ns` 写 meta。

### 5.5 单客户端与重连

- 控制链路只允许一个客户端；新连接替换旧连接。
- 对端关闭后，Rx 关闭 session，等下一个 accept；Tx 发现无 session 就跳过发送。
- 桥不主动重连（它是 server）；节点负责退避重连。
- 桥启动时 socket 目录/权限不对：只 log，1 Hz 重试 bind；不阻止 RMCS 启动。

### 5.6 共享 `Link` 的职责

| 方法 | 调用方 | 职责 |
|---|---|---|
| `open()` | Rx `before_updating`/`update` | 建目录、unlink、bind、listen、chmod |
| `poll_recv(now)` | Rx `update` | accept、recv、握手、解 `DownData` + `CustomByteBlock`、维护 peer seq/session |
| `take_down(packet)` | Rx `update` | 取最新下行数据段 |
| `poll_send(now, up)` | Tx `update` | 发 `HELLO_ACK`、75 Hz `UpData`、1 Hz `Health`；组官方 envelope |
| `status()` | Rx `update` | 连接/计数/最后 rx 时间 |

- `Link` 持有 `Listener`/`Socket`/`Session`，以及两个方向的 seq 基线、计数器。
- `teleop::link::Session` 的 `recv` 在 Rx、`send` 在 Tx；同一对象、同一线程；库文档明确非线程安全。

---

## 6. 协议

### 6.1 IPC envelope

完全按本仓 [`docs/ipc-protocol.md`](ipc-protocol.md)（共用副本）：

- 32 B 头，**全大端**；magic `54 4C`；版本 1；控制类型 1/2/3/4/6；最大帧 65536/body 65504；控制链路 `max_body_bytes=512`。
- 每端 `session_id` 随机、`seq` 首帧 1；`EAGAIN` 丢数据帧也消耗 seq；`HELLO_ACK` 重试同 seq。
- `Hello`/`HelloAck`/`UpData`/`DownData`/`Health` 的 codec 由 `teleop_ipc/control_link.hpp` 提供；本桥不再手写 IPC 消息。

### 6.2 官方 envelope

只处理两个字段为 1 的 `bytes` 消息：

```text
CustomControl   { optional bytes data = 1 }   → 0A <varint len> <data>
CustomByteBlock { optional bytes data = 1 }   → 0A <varint len> <data>
```

- 上行 `encode_custom_control(CustomControlData) -> bytes`：写 `0A` + varint(size) + data；`size>60` 拒绝。
- 下行 `decode_custom_byte_block(bytes) -> CustomByteBlockData`：读字段 1（wire type 2）；未知字段跳过；`len>300` 拒绝；字段缺失则 `size=0`。
- 用 `teleop::link::wire` 的 varint/tag/length-delimited 原语；**不引入 protobuf**。
- 测试用官方 golden 字节：
  - `CustomControl{data=AA BB}` → `0A 02 AA BB`；
  - `CustomByteBlock{data=300×5A}` → `0A AC 02 5A…`。

### 6.3 与客户端的分工

| 方向 | 本桥 | 节点 |
|---|---|---|
| 上行 | 组 `CustomControl` + `UpData` | 原样 publish `UpData.payload` 到 MQTT |
| 下行 | 解 `DownData` + `CustomByteBlock` | 原样填 `DownData.payload` |

---

## 7. 依赖：vendored `teleop_ipc`

### 7.1 vendor 快照（定稿）

桥仓内自带 `third_party/teleop_ipc/`，构建时直接 `add_subdirectory`；**不访问 Alliance-Terminal、不联网、不固定 commit**：

```cmake
add_subdirectory(third_party/teleop_ipc)
target_link_libraries(rmcs_terminal_bridge_component PRIVATE teleop::ipc)
```

- 快照来源/日期/许可证见 `third_party/teleop_ipc/ORIGIN.md`；代码是 Apache-2.0，桥仓整体仍是 GPL-3.0-only。
- 客户端 `libs/teleop_ipc` 是源头；升级是显式同步，不是 submodule / FetchContent。
- `tl::expected` 已随快照带入；桥仓不需要 protobuf。
- 同步后跑桥仓 `test_link`/`test_payload_codec`，并确认两份 `docs/ipc-protocol.md` 一致。

### 7.2 构建依赖

- `rmcs_executor`、`rclcpp`、`pluginlib`；**不需要 protobuf**。
- C++23（RMCS 现有标准）；`-O2 -Wall -Wextra -Wpedantic`。
- gtest 走 `ament_cmake_gtest`；RMCS 镜像里已有。

---

## 8. RMCS 集成

### 8.1 子模块

```text
RMCS/
└─ rmcs_ws/src/rmcs_terminal_bridge   # git submodule → Alliance-Algorithm/rmcs_terminal_bridge
```

- `.gitmodules` 加一项；子模块指向桥仓的固定 commit。
- 桥仓是 private 时，RMCS 的 public clone 需要权限；README 写明。

### 8.2 插件

```xml
<!-- plugins.xml -->
<library path="rmcs_terminal_bridge_component">
  <class type="rmcs_terminal_bridge::TerminalBridgeRx"
         base_class_type="rmcs_executor::Component">
    <description>RMCS ↔ Alliance Terminal 控制链路桥（接收/下行；发送是 partner）</description>
  </class>
</library>
```

```cmake
pluginlib_export_plugin_description_file(rmcs_executor plugins.xml)
```

- 只导出 Rx；Tx 由 `create_partner_component` 创建。
- 桥包导出 `include/`，作者包依赖它拿 `types.hpp`。

### 8.3 operator 的 executor YAML

```yaml
rmcs_executor:
  ros__parameters:
    components:
      - rmcs_terminal_bridge::TerminalBridgeRx -> terminal_bridge
      # 作者组件（示例）
      - rmcs_custom_control::ControlSource -> control_source

terminal_bridge:
  ros__parameters:
    socket_path: /home/pigeon/.local/state/teleop/run/rmcs/control.sock
    socket_mode: "0666"
    allowed_uid: 1000
    send_rate_hz: 75.0
    health_rate_hz: 1.0
    protocol_rev: "2027-V1.0.0"
    handshake_timeout_ms: 2000
```

- YAML 只列 Rx；Tx 是 partner。
- 作者组件单独列；executor 按依赖排序，列表顺序无所谓。
- 配置在 `rmcs_bringup/config/<operator>.yaml`（`set-robot` 按名字找），或新建一个 operator 专用 YAML。

### 8.4 运行时

- 开发容器里 `colcon build --merge-install` 构建桥包；
- `sync-remote` 把 `rmcs_ws/install` 同步到 runtime 容器的 `/rmcs_install`；
- runtime 的 `env_setup.bash` source `/rmcs_install/local_setup.bash`，pluginlib 从 `ament_index` 找到桥。

---

## 9. 部署（rmcs-runtime）

| 项 | 值 |
|---|---|
| 镜像 | `qzhhhi/rmcs-runtime`，默认 root |
| 宿主目录 | `~/.local/state/teleop/run/rmcs`，`0700`，属主 1000 |
| 容器挂载 | `-v /home/<user>/.local/state/teleop/run/rmcs:/home/<user>/.local/state/teleop/run/rmcs` |
| 桥 `socket_path` | 宿主绝对路径 |
| 桥 `socket_mode` | `"0666"` |
| 桥 `allowed_uid` | `1000` |
| 节点 `allowed_peer_uid` | `0` |

```bash
install -d -m 0700 ~/.local/state/teleop/run ~/.local/state/teleop/run/rmcs

docker run -d --restart=always --privileged --network=host \
    -v /dev:/dev \
    -v /home/pigeon/.local/state/teleop/run/rmcs:/home/pigeon/.local/state/teleop/run/rmcs \
    qzhhhi/rmcs-runtime:latest
```

- 安全口径：目录 0700 挡住其他本地用户；root 受信任；socket 0666 只对能进目录的 1000/root 有效；peercred 是白名单。
- 桥启动失败（目录没挂/权限不对）只 log + 1 Hz 重试，不阻止 RMCS 启动。
- 如果改回 dev 容器/原生（uid 1000）：`0600`、桥 `allowed_uid=-1`、节点 `allowed_peer_uid=-1`。

验证清单见 [Alliance-Terminal 开发容器文档 §6.3](https://github.com/Alliance-Algorithm/Alliance-Terminal/blob/main/docs/teleop-client-dev-environment.md)。

---

## 10. 测试与 CI

### 10.1 三级联调（I0）

| 级别 | 谁跑 | 内容 | 出口 |
|---|---|---|---|
| Link 级 | 桥仓 CI/本机 | `bridge_link_test`（无 ROS）起 `Link`；本地 fake peer（`third_party/teleop_ipc`） | 握手、75/50 Hz、seq/缺口、`EAGAIN`、重连、版本/超时/截断 |
| 组件级 | 桥仓 CI/本机 | `rmcs_executor` 加载 `TerminalBridgeRx` + `TerminalTestSource`/`Sink`（`config/terminal_test.yaml`） | pluginlib/YAML/75/50 |
| 运行时 | 手工 | `rmcs-runtime` root + 宿主 `ipc_probe` | socket 0666、uid、peercred、目录 0700 |

- **Link 级优先用桥仓本地 fake peer**（`third_party/teleop_ipc` + `bridge_link_test`），不依赖客户端仓。
- 跨端联调时**可选**客户端 `ipc_probe`（测试期输入，不进入桥仓构建/CI 的必备路径）。
- 组件级测试用测试源/汇，不依赖自定义控制作者；生产 YAML 不列测试组件。

### 10.2 单元测试

| 测试 | 覆盖 |
|---|---|
| `test_payload_codec` | `CustomControl`/`CustomByteBlock` golden bytes、空/最大、未知字段、截断、`size` 越界 |
| `test_link` | socketpair + 假时钟：握手、版本不匹配、握手超时、75 Hz 节奏、`EAGAIN` 丢弃、单客户端替换、下行 decode、seq/计数 |
| colcon 构建 | 插件能链接、`ClassLoader` 能找到 Rx、Tx partner 能注册接口 |
| 跨端（I0，可选） | 客户端 `ipc_probe` ↔ 桥 Link / RMCS executor；见 §10.1；不进入桥仓构建 |

- CI 用 `qzhhhi/rmcs-develop` 镜像（public），照 `rmcs_auto_aim_v2` 的做法：
  1. clone RMCS `main` + submodules；
  2. overlay 桥仓；
  3. `colcon build --packages-up-to rmcs_terminal_bridge`；
  4. `colcon test`；
  5. Link 级用桥仓本地 fake peer 跑 `bridge_link_test`，不 clone Alliance-Terminal；
  6. （可选，跨端 job）如需要客户端 `ipc_probe`，再单独 clone Alliance-Terminal 构建 probe，不进入核心 CI。
  7. （可选）`rmcs_executor` + `config/terminal_test.yaml` 的组件级 smoke。
- 客户端库的测试在 Alliance-Terminal 的 B2 里；桥仓不重复。

---

## 11. 路线（桥仓自己的批次）

| 批次 | 内容 | 出口 |
|---|---|---|
| D0 文档/骨架 | 本文、README、package.xml/CMakeLists/plugins.xml、`.clang-format`、`.gitignore` | 能 configure（空组件） |
| D1 types + codec | `types.hpp`、`payload_codec`、golden 测试 | `test_payload_codec` 绿 |
| D2 Link + Rx/Tx | `link`、`TerminalBridgeRx`、`TerminalBridgeTx`、socketpair 测试 | `test_link` 绿 |
| D3 插件/CI | pluginlib 导出、YAML 片段、RMCS 镜像 CI | `colcon build` + gtest 绿 |
| D4 两端联调 | 客户端 `ipc_probe` + `bridge_link_test`；RMCS executor + TestSource/Sink | Link/组件两级绿 |
| D5 运行时 | RMCS 子模块、operator YAML、rmcs-runtime 部署 + 宿主 probe | 运行时 75/50 Hz、socket/uid/peercred 清单 |

> D0–D3 与客户端 B2b 并行；D4/D5 对应客户端 I0。

---

## 12. 风险与待定

| 风险 | 影响 | 对策 |
|---|---|---|
| 桥仓 private，RMCS public | 外部 clone 需要权限 | README 写明；或后续转 public |
| vendor 快照来源 commit 未定 | 无法追溯 | `ORIGIN.md` 记录 B2 工作区/日期；B2 提交后补 commit |
| 测试组件混进生产 | 生产 YAML 多加载组件 | `plugins-test.xml` 单独；生产 YAML 只列 Rx；CI grep |
| 单组件循环依赖 | RMCS 起不来 | Rx/Tx 拆分 + 共享 `Link`；不把 Link 做成 executor 接口 |
| `create_partner_component` 生命周期 | 悬空引用 | Rx/Tx 各持 `shared_ptr<Link>`；executor 先 join 更新线程 |
| rmcs-runtime root 的 socket 权限 | 节点连不上 | 0666 + `allowed_uid` + 节点 `allowed_peer_uid` + 目录 0700 |
| 作者组件未加载 | 上行缺失 | input optional；`status.control_source_ready` |
| 手写官方 codec 漂移 | 字节不一致 | golden 测试；协议只在官方赛季变更时动 |
| YAML 漏配 | 桥不加载 | 文档 + 配置片段 + CI 检查？ |
| vendor 与客户端副本漂移 | 字节不一致 | 显式同步；两份 `docs/ipc-protocol.md` + 双方测试 |
| vendored 许可证 | 分发合规 | 保留 Apache-2.0 `LICENSE` / `ORIGIN.md`；桥仓整体 GPL-3.0-only |

---

## 13. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v0.1 | 2026-11-20 | 设计草稿：Rx/Tx 双组件 + 共享 Link、数据段/官方 envelope/IPC 三层、rmcs-runtime 部署、FetchContent pin |
| v0.2 | 2026-11-20 | IPC 两端优先：客户端 `ipc_probe` / `bridge_link_test` / TestSource-Sink 三级联调；roadmap D4/D5；开发期 FetchContent 本地覆盖；共用文档副本 |
| v0.3 | 2026-11-20 | 桥仓改为 **vendored `teleop_ipc`**：`third_party/teleop_ipc` + `add_subdirectory`；移除 FetchContent/FETCHCONTENT_SOURCE_DIR；D0 构建自包含 |
