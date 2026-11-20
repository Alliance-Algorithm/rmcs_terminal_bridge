# rmcs_terminal_bridge

RMCS 侧的终端桥：作为 `rmcs_executor::Component` 插件加载，UDS server，和 Alliance Terminal 的 `teleop-node` 交换 `UpData`/`DownData`。

- 当前状态：**D0 骨架**（插件可加载；Rx/Tx 接口、官方 envelope codec、Link 尚未实现）。
- 设计：[`docs/design.md`](docs/design.md)
- 线格式：[`docs/ipc-protocol.md`](docs/ipc-protocol.md)（与 `Alliance-Terminal/docs/ipc-protocol.md` 同内容；见头部指纹与同步规则）

## 构建

作为 RMCS 子模块放在 `RMCS/rmcs_ws/src/rmcs_terminal_bridge`，然后在 RMCS 开发容器里：

```bash
source /opt/ros/jazzy/setup.bash
source rmcs_ws/install/setup.bash
colcon build --packages-select rmcs_terminal_bridge --merge-install
```

`third_party/teleop_ipc` 是 `Alliance-Terminal/libs/teleop_ipc` 的手工快照（Apache-2.0）。
桥仓构建**不访问 Alliance-Terminal、不联网**；来源/日期见 `third_party/teleop_ipc/ORIGIN.md`。

客户端库升级时手动同步 `third_party/teleop_ipc`，并更新 `ORIGIN.md`。

## 运行

在 operator 的 executor YAML 里加：

```yaml
rmcs_executor:
  ros__parameters:
    components:
      - rmcs_terminal_bridge::TerminalBridgeRx -> terminal_bridge
```

参数见 `config/terminal_bridge.yaml`。

## 文档规则

- 本仓文档只放 `docs/`。
- `docs/ipc-protocol.md` 与 Alliance-Terminal 的副本**内容必须一致**；修改必须同步对端并更新头部版本/日期/正文 SHA256。

## 许可证

GPL-3.0-only（链接 `rmcs_executor`）。
