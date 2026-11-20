# 遥控客户端 · IPC 协议（控制链路 v1）

> **共用文档（两端各一份，内容必须一致）**
> 对端：`Alliance-Terminal/docs/ipc-protocol.md`（https://github.com/Alliance-Algorithm/Alliance-Terminal/blob/main/docs/ipc-protocol.md）
> 版本：v1.1 · 最后同步：2026-11-20
> 正文指纹：`sha256:9e6223a33eacbb7a8bc308ea9bc87de3a197b8f5e3ac964604bec2c2984dc878`（从 `## 0.` 起算）
> **修改本文件时必须同步修改对端；只改一边视为文档缺陷。**

| 项 | 内容 |
|---|---|
| 文档版本 | v1.0 |
| 状态 | **定稿**（实现属于 B2；本文是线格式与外部桥的契约） |
| 日期 | 2026-11-20 |
| 适用范围 | 通信进程 ↔ RMCS 终端桥（`rmcs_terminal_bridge`）的控制链路；展示链路复用同一帧头，消息号另列 |
| 关联 | `teleop-client-architecture.md` §5/§7、`teleop-client-implementation-plan.md` B2、`rmcs_terminal_bridge` 仓的 `docs/design.md` |

> 本文只说"线上是什么字节、谁解析什么"。桥的内部实现（组件、线程、部署）在桥仓的 `docs/design.md`；客户端侧的实现计划在施工计划 B2。

---

## 0. 一页结论

| 项 | 结论 |
|---|---|
| 传输 | `AF_UNIX` + `SOCK_SEQPACKET`，**单条双向连接**；终端桥 listen，通信进程 connect |
| 帧头 | 32 字节，**全部大端**；magic 线上是 `54 4C`（`'T','L'`） |
| 版本 | 帧头 `u8` = 帧格式版本（1）；`Hello`/`HelloAck` 的 `u32` = 语义协议版本（1）；v1 精确相等 |
| 会话 | 每端每次连接生成随机 `session_id`（u64，0 无效）；每端独立 `seq`，首帧 1 |
| 长度 | 单帧 ≤ **65536**，body ≤ **65504**；控制链路 `max_body_bytes=512`，展示链路 8192 |
| 消息 | 控制链路：1 HELLO、2 HELLO_ACK、3 UP_DATA、4 DOWN_DATA、6 HEALTH |
| 编解码 | 客户端库 `teleop_ipc` 手写 IPC 消息 codec（零 protobuf）；官方 `CustomControl`/`CustomByteBlock` 由**桥**组/解 |
| 核心边界 | 客户端只搬 `UpData.payload` / `DownData.payload` 字节，**不解 payload** |
| 部署 | 桥跑在 `rmcs-runtime`（root）：socket `0666`、`allowed_uid=1000`；节点侧 `allowed_peer_uid=0` |

---

## 1. 角色与连接

```text
UDS SOCK_SEQPACKET：/home/<user>/.local/state/teleop/run/rmcs/control.sock

┌─────────────────────────────┐                              ┌──────────────────────────┐
│ RMCS 进程                    │◀────────────────────────────▶│ 通信进程（客户端）        │
│  rmcs_terminal_bridge        │        单条双向连接            │  teleop-node             │
│  · UDS server（listen）      │                              │  · UDS client（connect） │
│  · 组/解官方 envelope        │                              │  · 只解 IPC envelope     │
└─────────────────────────────┘                              └──────────────────────────┘
```

| 端 | 角色 | 谁发起 | `session_id` |
|---|---|---|---|
| `rmcs_terminal_bridge` | server（listen/accept） | 先启动并 listen | accept 成功后生成 |
| `teleop-node` | client（connect） | 后启动，失败退避重试 | connect 成功后生成 |

- 控制链路只有这一条连接；重连就是新连接、新 `session_id`、`seq` 从 1 开始。
- 展示链路（节点 ↔ 前端）复用本文的帧头和握手规则，但消息号与 body schema 不同，见 §3.4。
- 运行期没有第二条通道；MQTT 只在节点和裁判系统服务器之间。

---

## 2. 帧格式

### 2.1 32 字节头

所有多字节字段**大端**（网络字节序）。`magic` 按两个字节比较，不按 `u16` 的宿主表示。

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| 0 | `magic` | 2 × `u8` | `0x54 0x4C`（`'T','L'`） |
| 2 | `proto_version` | `u8` | **帧格式版本**，当前 `1` |
| 3 | `msg_type` | `u8` | 见 §3.1 |
| 4 | `flags` | `u16` | 保留；发送方写 0；接收方**忽略未定义位** |
| 6 | `body_len` | `u16` | body 字节数，`0..65504` |
| 8 | `session_id` | `u64` | 发送方本次连接的会话 id；0 无效 |
| 16 | `seq` | `u64` | 发送方逐帧递增，首帧 1；每端独立 |
| 24 | `t_mono_ns` | `i64` | 发送时刻，`CLOCK_MONOTONIC` 纳秒 |
| 32 | `body` | `body_len × u8` | 对应 `msg_type` 的 protobuf 消息 |

示例（字段取值仅示意）：

```text
54 4C                     magic
01                        proto_version = 1
03                        msg_type = UP_DATA
00 00                     flags
00 2A                     body_len = 42
00 00 00 00 00 00 00 01   session_id
00 00 00 00 00 00 00 01   seq
00 00 01 8F 2A 3B 4C 5D   t_mono_ns
<42 字节 protobuf body>
```

### 2.2 版本

| 位置 | 含义 | 当前 | 规则 |
|---|---|---|---|
| 帧头 `proto_version u8` | 帧格式/装帧方式 | `1` | 接收方解析头之前检查；`≠1` 直接关闭 |
| `Hello`/`HelloAck` 的 `proto_version u32` | 语义协议版本 | `1` | 握手时检查；`≠1` 拒绝进入 ACTIVE |
| `HelloAck.protocol_rev` | 官方 RoboMaster 协议版本串（如 `"2027-V1.0.0"`） | 配置 | 只显示/记录，不参与我们的兼容判断 |

- v1 采用**精确相等**；向后兼容的新增 protobuf 字段不升版本（未知字段忽略），破坏性变更才升到 2。
- 版本不匹配时，桥发送带自己版本的 `HELLO_ACK` 后关闭，让节点日志能打印双方版本；节点收到不匹配的 ACK 记录后关闭。

### 2.3 长度上限

| 常量 | 值 | 说明 |
|---|---:|---|
| `kHeaderBytes` | 32 | |
| `kMaxFrameBytes` | 65536 | 头 + body |
| `kMaxBodyBytes` | 65504 | `u16` 可表示 65535，但按 64 KiB 总长截 |
| 控制链路 `max_body_bytes` | 512 | `Session` 参数；实际消息 ~70/~310 B |
| 展示链路 `max_body_bytes` | 8192 | `UiHelloAck` 带 32 条 channel 定义，约 1.5 KB |

- 编码：body 超过 `kMaxBodyBytes` 或当前链路的 `max_body_bytes` → `TooLarge`，不发送。
- 解码：`recvmsg(MSG_TRUNC)` 拿真实长度 `real`；
  - `real < 32` → `Truncated`；
  - `real > 32 + max_body_bytes` → `TooLarge`；
  - `real != 32 + body_len` → `BadLength`。
  - 以上都计数并关闭连接；SEQPACKET 已丢弃整个 record，下一条仍然对齐。
- 发送：一帧大于 `SO_SNDBUF` 会 `EMSGSIZE`，库映射为 `TooLarge/Io`，调用方丢弃并计数。

---

## 3. 消息

### 3.1 类型号

| 值 | 控制链路 | 方向 | 频率 | 说明 |
|---:|---|---|---|---|
| 1 | `HELLO` | 节点 → 桥 | 连接时 | 携带 `Hello` |
| 2 | `HELLO_ACK` | 桥 → 节点 | 连接时 | 携带 `HelloAck` |
| 3 | `UP_DATA` | 桥 → 节点 | 75 Hz | 携带 `UpData`；可丢，取最新 |
| 4 | `DOWN_DATA` | 节点 → 桥 | ≤50 Hz | 携带 `DownData` |
| 6 | `HEALTH` | 桥 → 节点 | 1 Hz | 携带 `Health`；尽力 |
| 5 | — | — | — | 保留 |
| 100–199 | 对端扩展 | — | — | 未知类型忽略并计数 |
| 200+ | 实验性 | — | — | 默认忽略并计数 |

### 3.2 控制链路消息体（`proto/ipc.proto`）

| 消息 | 字段 | 说明 |
|---|---|---|
| `Hello` | `u32 proto_version=1`、`u64 capabilities=2`、`string client_version=3` | 节点发起 |
| `HelloAck` | `u32 proto_version=1`、`u64 capabilities=2`、`string protocol_rev=3` | 桥回应 |
| `UpData` | `u32 channel=1`（固定 1）、`i64 t_src_ns=2`、`bytes payload=3` | 桥 → 节点；`payload` 是**官方 `CustomControl` 的序列化字节** |
| `DownData` | `u32 channel=1`（固定 2）、`i64 t_recv_ns=2`、`bytes payload=3` | 节点 → 桥；`payload` 是**官方 `CustomByteBlock` 的序列化字节** |
| `Health` | `u32 state=1`、`u32 load_pct=2` | 桥 → 节点 |

- `session_id`/`seq`/`t_mono_ns` 只在帧头里，body 不重复。
- `UpData.t_src_ns`：桥侧采样时刻（同机单调钟），用于 A2 延迟。
- `DownData.t_recv_ns`：节点从 MQTT 收到的时刻，用于桥/作者侧延迟分析。
- `Health.state` 建议位：bit0 = 桥运行、bit1 = 客户端在线、bit2 = 控制源就绪；`load_pct` 先填 0。节点只用"有没有 HEALTH"判活。
- `capabilities` 位：bit0 = 支持 UP_DATA、bit1 = 支持 DOWN_DATA、bit2 = 支持 HEALTH；v1 桥填 `0b111`，节点可忽略。

### 3.3 编解码规则

- 客户端库 `libs/teleop_ipc` **不依赖 protobuf**；IPC 消息用手写 codec：
  - `control_link.hpp`：`Hello`/`HelloAck`/`UpData`/`DownData`/`Health` 的编解码；
  - `wire.hpp`：varint、tag、length-delimited、未知字段跳过等原语；
  - `encode` 写入调用方缓冲区并返回字节数；`decode` 返回指向输入缓冲区的 `span`/`string_view`，**零分配**；
  - 未知字段跳过；截断、非法 wire type、长度越界返回错误；
  - 测试用 `ipc.pb` 生成的字节做**逐字节对照**（编码）和反解（解码）。
- 官方 `CustomControl` / `CustomByteBlock` 的 codec **不在客户端**：
  - 它们在桥仓，用 `wire.hpp` 手写（两个消息各约 20 行）；
  - 客户端不 include、不链接、不使用这两个官方消息类型；
  - `rm_proto` 里的官方类型仍供前端/模拟器使用，但不进入节点的 IPC 数据路径。

### 3.4 展示链路消息号（本规范只列号，不展开 schema）

| 值 | 展示链路 |
|---:|---|
| 1 | `UI_HELLO` |
| 2 | `UI_HELLO_ACK` |
| 3 | `UI_DATA` |
| 4 | `UI_HEALTH` |
| 5 | `UI_RECORD_STATE` |
| 6 | `UI_CMD` |
| 7 | `UI_CMD_ACK` |

- 两条链路的 1/2 语义一致，`Session` 的握手辅助共用；3 以上各自解析。
- 展示链路的 body schema 见 `proto/ui_link.proto`；`UiData.seq` 与帧头 `seq` 的关系在展示链路实现前另行收口，不影响控制链路。

---

## 4. 握手、会话与序号

### 4.1 时序

```text
节点（connector）                         桥（acceptor）
    │ connect                               │ listen / accept
    │ session_id = nid                       │ session_id = bid
    │ ── HELLO (seq=1, sid=nid) ───────────▶ │
    │                                        │ 校验帧头/版本/body
    │ ◀────────── HELLO_ACK (seq=1, sid=bid) │
    │ 校验版本/记录 bid                        │ 记录 nid
    │ state = ACTIVE                         │ state = ACTIVE
    │ ── DOWN_DATA (seq=2.., sid=nid) ─────▶ │
    │ ◀── UP_DATA / HEALTH (seq=2.., sid=bid) │
```

- `Hello`/`HelloAck` 的 body 必须是有效 protobuf；空 body 视为协议错误。
- 桥在收到合法 `HELLO` 后才发 `HELLO_ACK`；在 ACTIVE 之前收到数据帧，跳过并计数。
- 握手超时：2 s（配置 `handshake_timeout_ms`）；超时关闭并计数。
- 版本不匹配：见 §2.2；桥回 ACK 后关闭，节点记录并退避。

### 4.2 `session_id`

- 每端在**每次连接**生成一个随机 `u64`；`0` 保留为无效；用 `getrandom`，失败退到时间/pid/计数器混合。
- 发送方把自己所有帧的 `session_id` 设为同一个值；对端从第一帧学到。
- 接收方在握手第一帧记录 `peer_session_id`；**同一连接内**后续帧必须等于它；变化视为协议错误，关闭并计数（正常重启会走新连接）。
- 新连接自然重置：新的 `session_id`、`seq` 基线从 0 开始等首帧 1。
- 录制/日志用 `session_id` 标记会话；不要把它和 `seq` 混用。

### 4.3 `seq`

- 每端独立计数，首帧（`HELLO`/`HELLO_ACK`）为 `1`，之后每发一帧 +1。
- 数据帧 `send` 返回 `EAGAIN`：丢弃、计数，**seq 已消耗**（接收端会看到缺口，和发送端 drop 对上）。
- `HELLO_ACK` 这类必须送达的帧：保留同一帧/同一 seq，下个 update 重试，不消耗新 seq。
- 接收端：`expected = last_seq + 1`
  - `seq == expected`：正常；
  - `seq > expected`：`rx_gaps += seq - expected`；
  - `seq < expected`：`rx_anomalies++`，不崩溃、不阻塞。
- `u64` 不考虑回绕（1 kHz 也要数亿年）。
- 断线重连由调用方负责：100 ms → 2 s 指数退避（±20% 抖动），重连后状态清空、等下一帧，不做 resync 请求。

### 4.4 单客户端与异常

- 控制链路只有一个客户端。新连接到来时，桥**关闭旧连接并替换**（节点重启场景），计数并写日志。
- 未知 `msg_type`：跳过并计数，不断连。
- 帧/长度/版本异常：关闭并计数。
- 对端关闭（`recv` 返回 0 或 `ECONNRESET`）：关闭、计数、等下一个 accept。

---

## 5. 传输层

| 项 | 规定 |
|---|---|
| 类型 | `AF_UNIX` + `SOCK_SEQPACKET` |
| 创建 | `socket(AF_UNIX, SOCK_SEQPACKET \| SOCK_NONBLOCK \| SOCK_CLOEXEC, 0)`；`accept4` 同样 |
| 地址 | 文件系统路径（生产）或抽象地址（测试）；库同时支持 |
| socket 路径 | 宿主 `~/.local/state/teleop/run/rmcs/control.sock`；桥配置写宿主绝对路径 |
| 监听 | 桥在启动时 `unlink` 旧 socket、`bind`、`listen`、`chmod`；失败只 log，1 Hz 重试 |
| 读取 | `recvmsg(..., MSG_TRUNC)`；用真实长度判断截断/越界 |
| 写入 | `send(..., MSG_NOSIGNAL)`；`EAGAIN` 映射为 `WouldBlock` |
| peercred | `accept`/`connect` 后 `getsockopt(SO_PEERCRED)`；按配置校验 uid |
| 缓冲 | 控制链路 `SO_SNDBUF`/`SO_RCVBUF` 建议 4 KiB；展示链路 16 KiB；库不强制，应用设置 |
| 线程 | 库不建线程、不持有 epoll；所有调用在调用方线程；`Session` 非线程安全 |

- 控制链路的实际帧很小（~70/~310 B），4 KiB 缓冲约十几帧，把最坏排队延迟限制在几百毫秒内；B4 按实测调。
- `Session` 的 `send` 和 `recv` 可以由不同组件调用（桥的 Tx/Rx），但必须在**同一个 executor 更新线程**里。
- 库提供 `Listener`/`Socket`/`Session`；不提供重连、退避、事件循环——这些在 B4/桥里。

---

## 6. Payload 边界（核心契约）

### 6.1 三层

| 层 | 内容 | 谁组/解 | 位置 |
|---|---|---|---|
| **数据段** | `CustomControl.data` / `CustomByteBlock.data`，0–60 B / 0–300 B | 自定义控制作者 | 作者组件 ↔ 桥 |
| **官方 envelope** | 序列化后的 `CustomControl` / `CustomByteBlock` | **RMCS 终端桥** | 桥内部 |
| **IPC envelope** | `UpData`/`DownData` + 32 B 帧头 | 节点/桥共用 `teleop_ipc` | UDS |
| **MQTT** | 官方 envelope 的裸字节 | 节点原样收发 | 节点 ↔ 服务器 |

### 6.2 客户端义务

- `UpData.payload`：**原样** publish 到 MQTT `CustomControl`；不解析、不改写、不补字节。
- `DownData.payload`：从 MQTT `CustomByteBlock` 收到的字节**原样**填入；不解析、不改写。
- 不 include `rm/terminal.pb.h` 里的 `CustomControl`/`CustomByteBlock`；不调用它们的 `ParseFrom*`/`Serialize*`。
- 只按 `Session.max_body_bytes` 做长度边界；payload 内部长度/格式不归客户端管。
- 录制写原始字节；前端如需展示，只显示大小/hex，不解析内层。

### 6.3 桥义务

- 上行：把作者给的数据段封成 `CustomControl{ optional bytes data = 1 }`（线上 `0A <varint len> <data>`），再封 `UpData`；
- 下行：从 `DownData.payload` 解 `CustomByteBlock`，把 `data` 数据段交给作者；
- 只检查 `size ≤ 60/300`；不解释、不补位、不校验内层格式；
- 使用 `teleop::link::wire` 或自带等价实现；不依赖 protobuf。

### 6.4 数据段类型

```cpp
// rmcs_terminal_bridge/types.hpp（桥仓导出；作者包依赖桥包）
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

- 两个类型只表示**数据段**；`size` 之外不读。
- `size=0` 按"存在但为空的数据段"编码（`0A 00`）；如果作者需要"字段不存在"，要额外加 presence，当前不做。
- 作者侧 producer/consumer 用同名同类型接口；executor 按 `名字 + 类型` 绑定。

### 6.5 接口名

| 名字 | 方向 | 类型 | 注册方 |
|---|---|---|---|
| `/terminal/custom_control` | 作者 output → 桥 Tx input | `InputInterface<CustomControlData>` | 桥 Tx |
| `/terminal/custom_byte_block` | 桥 Rx output → 作者 input | `OutputInterface<CustomByteBlockData>` | 桥 Rx |
| `/terminal/custom_byte_block/meta` | 桥 Rx output（可选） | `seq`/`t_recv_ns` | 桥 Rx |
| `/terminal/status` | 桥 Rx output（可选） | 连接/计数 | 桥 Rx |

---

## 7. 与 `rmcs_terminal_bridge` 的契约

### 7.1 两个组件（桥内部）

单组件同时注册 input 和 output 会形成循环依赖：

```text
A（作者组件）── output 控制 ──▶ B.input
B（单桥组件）── output 字节块 ──▶ A.input
=> B 依赖 A，A 依赖 B；executor 报 "Circular dependency found"
```

桥拆成 Rx/Tx，共享一个 `Link` 对象（不是 executor 接口，不产生依赖边）：

```text
A.out ──▶ Tx.in        （Tx 依赖 A）
Rx.out ──▶ A.in        （A 依赖 Rx）
Rx ──shared_ptr<Link>──▶ Tx

拓扑：Rx → A → Tx
```

| 组件 | 接口 | 职责 | YAML |
|---|---|---|---|
| `TerminalBridgeRx`（owner） | `OutputInterface<CustomByteBlockData>` | 持有 `Link`；accept/recv/握手/解 `CustomByteBlock`；写下行 | `rmcs_terminal_bridge::TerminalBridgeRx -> terminal_bridge` |
| `TerminalBridgeTx`（partner） | `InputInterface<CustomControlData>` | 读数据段；组 `CustomControl`+`UpData`；75 Hz/健康/ACK | 不单列，Rx 用 `create_partner_component` 创建 |

- `Link` 是普通 C++ 对象（socket/session/codec/seq/计数器），Rx 的 `poll_recv` 和 Tx 的 `poll_send` 在同一更新线程里调用。
- 只要作者同时消费下行、生产上行，拓扑保证 **Rx 先跑、A 次之、Tx 最后**：下行同周期到达作者，上行同周期发出，没有额外延迟。
- `plugins.xml` 只导出 Rx；Tx 是 partner，不需要 pluginlib 导出。
- YAML 只列 Rx；作者侧组件单独列；executor 按依赖排序，列表顺序无所谓。

### 7.2 客户端侧

- `teleop_ipc` 的 `Session` 支持 `send`/`recv` 由不同组件调用（同一线程）；B2 的测试覆盖拆分调用。
- 客户端不需要知道桥的 Rx/Tx 划分；它只看到一条 UDS 连接和 IPC 消息。

### 7.3 桥仓

- 仓库：`https://github.com/Alliance-Algorithm/rmcs_terminal_bridge`（私有仓，内部使用）。
- 桥的设计/部署/测试在桥仓 `docs/design.md`；客户端仓只保留本文作为线格式契约。
- 桥仓 vendor 一份 `libs/teleop_ipc` 快照（Apache-2.0，保留来源/许可证）；客户端库仍是源头，升级是一次显式同步。

---

## 8. 部署（`rmcs-runtime`）

`rmcs-runtime` 默认 **root**；桥在 RMCS 进程里也是 root。节点在宿主以 **uid 1000**（示例）运行。

| 项 | 值 |
|---|---|
| 宿主目录 | `~/.local/state/teleop/run/rmcs`，`0700`，属主 1000 |
| 容器挂载 | `-v /home/<user>/.local/state/teleop/run/rmcs:/home/<user>/.local/state/teleop/run/rmcs` |
| 桥 `socket_path` | 宿主绝对路径（容器内 `HOME=/root`，不能用 `~`） |
| 桥 `socket_mode` | `"0666"`（root 创建，节点 1000 才能 connect） |
| 桥 `allowed_uid` | `1000`（桥 euid 0，默认同 euid 检查会拒绝节点） |
| 节点 `allowed_peer_uid` | `0`（节点 euid 1000，对端是 root） |
| 目录权限 | `0700` 是真正的闸门：其他本地用户进不来 |

```yaml
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

- 宿主目录必须**在容器启动前**由用户创建；否则 Docker 会把挂载源建成 root，节点无法遍历。
- 桥启动时 unlink 旧 socket、bind、listen、chmod；失败只 log，1 Hz 重试，不阻止 RMCS 启动。
- 运行时 `/rmcs_install` 由 `sync-remote` 从开发容器同步；`pluginlib` 从 `ament_index` 找到桥的插件。
- 如果以后改成"同 uid"模型（dev 容器/原生）：`socket_mode="0600"`、`allowed_uid=-1`、节点 `allowed_peer_uid=-1`，目录仍是 `0700`。
- 详细验证清单见 `teleop-client-dev-environment.md` §6.3。

---

## 9. 测试与验收（B2）

| 测试 | 覆盖 |
|---|---|
| `test_frame` | 32 B 头 golden bytes（BE）、字段极限、magic/版本错误、`body_len` 越界/长度不符、flags 未知位忽略 |
| `test_seqpacket` | 抽象/路径 socket、listen/connect/accept、send/recv、`EAGAIN`、对端关闭、截断/超长 record、peercred、名过长 |
| `test_session` | 握手（两种角色）、版本不匹配、握手超时（假时钟）、seq 缺口、session_id 变化、未知类型、无 sleep/无线程 |
| `test_control_link` | 手写 codec ↔ `ipc.pb` 逐字节对照；`payload` 0/60/300/1400 B 原样往返；未知字段/截断 |

验收：

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset ci  && cmake --build --preset ci  && ctest --preset ci   # 容器内，-Werror
```

- 桥仓的 codec/link 测试在桥仓 CI 里跑（RMCS 镜像 + gtest + colcon），不在本仓库。
- 端到端（真桥 + 节点）在 B4 之后用 `teleop_sim`/真桥各跑一次。

---

## 10. 非本规范范围

- 展示链路的 `UiData`/`UiCmd` body schema、`UiData.seq` 与帧头的关系：展示链路实现前另定。
- MQTT topic 表与官方 32 条消息：见 `proto/topics.inc` 与 `docs/custom-terminal-protocol.md`。
- 桥的组件实现、RMCS 集成、CI：见桥仓 `docs/design.md`。

---

## 11. 变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-11-20 | 定稿：大端帧头、双版本号、每端 session/seq、65536/65504 上限、控制/展示消息号、手写 IPC codec、payload 三层边界、Rx/Tx 桥契约、rmcs-runtime 部署 |
