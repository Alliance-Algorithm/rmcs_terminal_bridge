# teleop_ipc

控制链路 IPC 的 header-only 库（Apache-2.0）。节点、模拟器和 RMCS 终端桥共用；**不依赖 protobuf**，也不包含官方 `CustomControl`/`CustomByteBlock` codec。

## 头文件

| 头 | 内容 |
|---|---|
| `teleop_ipc/error.hpp` | `ErrorCode`/`Error`/`Result<T>`（`tl::expected`） |
| `teleop_ipc/fd.hpp` | `UniqueFd` |
| `teleop_ipc/frame.hpp` | 32 字节大端帧头、`FrameView`、长度校验 |
| `teleop_ipc/seqpacket.hpp` | `UnixAddress`、`Socket`、`Listener`、peercred |
| `teleop_ipc/wire.hpp` | protobuf wire 原语（varint/tag/length-delimited） |
| `teleop_ipc/control_link.hpp` | `Hello`/`HelloAck`/`UpData`/`DownData`/`Health` 手写 codec |
| `teleop_ipc/session.hpp` | `Session`：role/state/session_id/seq/长度/计数 |

## 自包含

```cmake
# 本仓库
add_subdirectory(libs/teleop_ipc)
target_link_libraries(app PRIVATE teleop::ipc)

# 桥仓：FetchContent 或 add_subdirectory 均可
```

- 不调 `project()`、不依赖 `${PROJECT_SOURCE_DIR}`、不依赖顶层 target；
- `third_party/` 里 vendored `tl::expected` v1.3.1（CC0-1.0），通过 `SYSTEM` include 引入；
- `_GNU_SOURCE` 通过 INTERFACE 传播（`ucred`/`accept4`/`SOCK_CLOEXEC`）。

## 命名空间

库使用 **`teleop::link`**，不是 `teleop::ipc`：`proto/ipc.proto` 生成的 C++ 代码占用 `teleop::ipc`，避免同名类型（`Hello`/`UpData`/…）跨 TU 的 ODR 冲突。

## 线程与边界

- `Session` 非线程安全；`send`/`recv` 可以由不同组件调用，但必须在同一个线程（RMCS 的 1 kHz executor 更新线程满足）。
- `UpData.payload`/`DownData.payload` 是**不透明字节**；本库不解析官方 envelope。官方 envelope 由桥仓的 `payload_codec` 组/解。
- 帧头全大端；协议定稿见 `docs/ipc-protocol.md`。

## 测试

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```

`test_control_link` 用 `ipc.pb` 生成的字节与手写 codec 逐字节对照；`test_session` 用 `socketpair` + 假时钟，不 sleep。
