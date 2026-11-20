# rmcs_terminal_bridge

RMCS 侧的终端桥：作为 `rmcs_executor::Component` 插件加载，UDS server，和 Alliance Terminal 的 `teleop-node` 交换 `UpData`/`DownData`。

- 当前状态：**D0–D4 功能完成**（官方 envelope codec、`Link`、`TerminalBridgeRx`/`TerminalBridgeTx`、`bridge_link_test`、组件级联调）；D4 的 Link/组件/launch 级已过，10 s 客户端侧 `rx_up=750`、`tx_down=500`、`gaps=0`；**D3 的 CI 工作流、D5 `rmcs-runtime` 运行时（root/60 s/restart/慢消费者）待做**。
- `link.hpp` / `payload_codec.hpp` 是 header-only；`TerminalBridgeRx`/`TerminalBridgeTx` 在 `src/component.cpp`，测试源/汇在 `src/test_components.cpp`，都按 `rmcs_core` 风格直接定义组件类。
- 设计：[`docs/design.md`](docs/design.md)
- 线格式：[`docs/ipc-protocol.md`](docs/ipc-protocol.md)（与 `Alliance-Terminal/docs/ipc-protocol.md` 同内容；见头部指纹与同步规则）

## 构建

把本仓放在 `RMCS/rmcs_ws/src/rmcs_terminal_bridge`（手动拉取，不做子模块），然后在 RMCS 开发容器里：

```bash
source /opt/ros/jazzy/setup.bash
source rmcs_ws/install/setup.bash
colcon build --packages-select rmcs_terminal_bridge --symlink-install --merge-install
```

`third_party/teleop_ipc` 是 `Alliance-Terminal/libs/teleop_ipc` 的手工快照（Apache-2.0）。
桥仓构建**不访问 Alliance-Terminal、不联网**；来源/日期见 `third_party/teleop_ipc/ORIGIN.md`。

客户端库升级时手动同步 `third_party/teleop_ipc`，并更新 `ORIGIN.md`。

## 测试

```bash
colcon test --packages-select rmcs_terminal_bridge --merge-install
colcon test-result --verbose
```

- `test_payload_codec`：官方 `CustomControl`/`CustomByteBlock` golden bytes、空/最大/未知字段/截断/越界。
- `test_link`：握手、版本不匹配、握手超时、75 Hz/1 Hz、`EAGAIN`、单客户端替换、下行 decode、peercred。
- `TerminalTestSource`/`Sink` 与 Rx 写在同一个 `plugins.xml`；只有 `terminal_test.yaml` 列它们，生产 YAML 不加载。
- `bridge_link_test`：无 ROS 的 `Link` 服务端，给客户端 `ipc-probe` 当对端。

## 运行

在 operator 的 executor YAML 里加：

```yaml
rmcs_executor:
  ros__parameters:
    components:
      - rmcs_terminal_bridge::TerminalBridgeRx -> terminal_bridge
      # 自定义控制作者组件（示例，需另行实现/提供）
      - <your_author>::ControlSource -> control_source

terminal_bridge:
  ros__parameters:
    socket_path: /home/<user>/.local/state/teleop/run/rmcs/control.sock
    socket_mode: "0666"          # 字符串，按八进制解析
    allowed_uid: 1000
    send_rate_hz: 75.0
    health_rate_hz: 1.0
    protocol_rev: "2027-V1.0.0"
    handshake_timeout_ms: 2000
```

- 桥只导出 `TerminalBridgeRx`；`TerminalBridgeTx` 是 partner，由 Rx 用 `create_partner_component` 创建，**不要**单独写进 YAML。
- 作者组件通过 `/terminal/custom_control`（output）和 `/terminal/custom_byte_block`（input）与桥交互；`include <rmcs_terminal_bridge/types.hpp>`。
- 宿主 socket 目录必须由宿主先建：`install -d -m 0700 ~/.local/state/teleop/run/rmcs`；桥不会用 root 创建这个目录。

## 联调

Link 级（不需要 ROS）：桥仓起 `bridge_link_test`，客户端仓起 `ipc-probe`。

```bash
# 桥仓
./build/rmcs_terminal_bridge/bridge_link_test \
    --socket /tmp/teleop-test/control.sock --duration 60 --allowed-uid -1

# 客户端仓（同 uid 测试用 --peer-uid -1；rmcs-runtime root 用 --peer-uid 0）
./build/dev/tools/ipc_probe/ipc-probe \
    --socket /tmp/teleop-test/control.sock --peer-uid -1 \
    --duration 60 --down-hz 50 --stats
```

组件级：用 `config/terminal_test.yaml` 起 `rmcs_executor` + `TerminalTestSource/Sink`，再用 `ipc-probe` 连 `terminal_bridge`。

```bash
ros2 run rmcs_executor rmcs_executor --ros-args \
    --params-file src/rmcs_terminal_bridge/config/terminal_test.yaml
```

## 文档规则

- 本仓文档只放 `docs/`。
- `docs/ipc-protocol.md` 与 Alliance-Terminal 的副本**内容必须一致**；修改必须同步对端并更新头部版本/日期/正文 SHA256。

## 许可证

GPL-3.0-only（链接 `rmcs_executor`）。
