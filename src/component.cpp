#include <rmcs_executor/component.hpp>
#include <rclcpp/node.hpp>

namespace rmcs_terminal_bridge {

// D0 骨架：只证明插件可加载；Rx/Tx 的接口与 Link 在 D1/D2 落地。
class TerminalBridgeRx
    : public rmcs_executor::Component
    , public rclcpp::Node {
public:
    TerminalBridgeRx()
        : Node{
              get_component_name(),
              rclcpp::NodeOptions().automatically_declare_parameters_from_overrides(true)} {}

    void update() override {}
};

} // namespace rmcs_terminal_bridge

#include <pluginlib/class_list_macros.hpp>

PLUGINLIB_EXPORT_CLASS(rmcs_terminal_bridge::TerminalBridgeRx, rmcs_executor::Component)
