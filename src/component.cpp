#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <pluginlib/class_list_macros.hpp>
#include <rclcpp/logging.hpp>
#include <rclcpp/node.hpp>
#include <rmcs_executor/component.hpp>

#include "rmcs_terminal_bridge/link.hpp"
#include "rmcs_terminal_bridge/types.hpp"

namespace rmcs_terminal_bridge {

// partner 组件：由 TerminalBridgeRx 用 create_partner_component 创建；不单独导出插件。
class TerminalBridgeTx : public rmcs_executor::Component {
public:
    explicit TerminalBridgeTx(std::shared_ptr<Link> link)
        : link_{std::move(link)} {
        register_input("/predefined/timestamp", timestamp_);
        register_input("/terminal/custom_control", control_, false);
    }

    void before_updating() override {
        if (!control_.ready()) {
            RCLCPP_WARN(
                rclcpp::get_logger("rmcs_terminal_bridge"),
                "terminal bridge: /terminal/custom_control is not bound; uplink disabled");
        }
    }

    void update() override {
        if (!link_ || !timestamp_.ready()) {
            return;
        }
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp_->time_since_epoch())
                .count();
        link_->poll_send(now_ns, control_.ready() ? &*control_ : nullptr);
    }

private:
    std::shared_ptr<Link> link_;
    InputInterface<std::chrono::steady_clock::time_point> timestamp_;
    InputInterface<CustomControlData> control_;
};

// owner 组件：UDS server + 下行输出；上行由 partner TerminalBridgeTx 发送。
class TerminalBridgeRx
    : public rmcs_executor::Component
    , public rclcpp::Node {
public:
    TerminalBridgeRx()
        : Node{
              get_component_name(),
              rclcpp::NodeOptions().automatically_declare_parameters_from_overrides(true)} {
        LinkConfig config;
        get_parameter_or<std::string>("socket_path", config.socket_path, std::string{});

        std::string mode_text;
        get_parameter_or<std::string>("socket_mode", mode_text, std::string{"0666"});
        config.socket_mode = parse_socket_mode(mode_text);

        get_parameter_or<int>("allowed_uid", config.allowed_uid, -1);
        get_parameter_or<double>("send_rate_hz", config.send_rate_hz, 75.0);
        get_parameter_or<double>("health_rate_hz", config.health_rate_hz, 1.0);
        get_parameter_or<std::string>(
            "protocol_rev", config.protocol_rev, std::string{"2027-V1.0.0"});

        int handshake_timeout_ms = 2000;
        get_parameter_or<int>("handshake_timeout_ms", handshake_timeout_ms, 2000);
        if (handshake_timeout_ms > 0) {
            config.handshake_timeout_ns =
                static_cast<std::int64_t>(handshake_timeout_ms) * 1'000'000;
        }

        socket_path_ = config.socket_path;
        link_ = std::make_shared<Link>(std::move(config));

        register_input("/predefined/timestamp", timestamp_);
        register_output("/terminal/custom_byte_block", down_);
        tx_ = create_partner_component<TerminalBridgeTx>(get_component_name() + "_tx", link_);

        if (socket_path_.empty()) {
            RCLCPP_ERROR(
                get_logger(), "terminal bridge: socket_path is empty; UDS server disabled");
        }
    }

    void before_updating() override {
        if (!link_->open()) {
            const auto& error = link_->last_error();
            const std::string error_name{teleop::link::error_code_name(error.code)};
            const std::string error_context{error.context};
            RCLCPP_WARN(
                get_logger(), "terminal bridge: open %s failed: %s (%s, errno=%d)",
                socket_path_.c_str(), error_name.c_str(), error_context.c_str(), error.sys_errno);
        }
    }

    void update() override {
        if (!timestamp_.ready()) {
            return;
        }
        const auto now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(timestamp_->time_since_epoch())
                .count();
        link_->poll_recv(now_ns);

        DownPacket packet;
        if (link_->take_down(packet)) {
            *down_ = packet.data;
        }

        const auto& stats = link_->stats();
        RCLCPP_INFO_THROTTLE(
            get_logger(), *get_clock(), 10000,
            "terminal bridge: connected=%d handshakes=%llu rx_down=%llu tx_up=%llu "
            "up_drop=%llu health=%llu peer_reject=%llu",
            link_->connected() ? 1 : 0, static_cast<unsigned long long>(stats.handshakes),
            static_cast<unsigned long long>(stats.rx_down_frames),
            static_cast<unsigned long long>(stats.tx_up_frames),
            static_cast<unsigned long long>(stats.tx_up_dropped),
            static_cast<unsigned long long>(stats.tx_health_frames),
            static_cast<unsigned long long>(stats.peer_uid_rejected));

        if (!link_->connected() && link_->last_error().code != teleop::link::ErrorCode::None) {
            const auto& error = link_->last_error();
            const std::string error_name{teleop::link::error_code_name(error.code)};
            const std::string error_context{error.context};
            RCLCPP_WARN_THROTTLE(
                get_logger(), *get_clock(), 30000,
                "terminal bridge: waiting for client: %s (%s, errno=%d)", error_name.c_str(),
                error_context.c_str(), error.sys_errno);
        }
    }

private:
    static mode_t parse_socket_mode(const std::string& text) {
        if (text.empty()) {
            return 0666;
        }
        char* end = nullptr;
        errno = 0;
        const unsigned long value = std::strtoul(text.c_str(), &end, 8);
        if (errno != 0 || end == text.c_str() || *end != '\0' || value > 07777) {
            return 0666;
        }
        return static_cast<mode_t>(value);
    }

    std::shared_ptr<Link> link_;
    std::shared_ptr<TerminalBridgeTx> tx_;
    std::string socket_path_;
    InputInterface<std::chrono::steady_clock::time_point> timestamp_;
    OutputInterface<CustomByteBlockData> down_;
};

} // namespace rmcs_terminal_bridge

PLUGINLIB_EXPORT_CLASS(rmcs_terminal_bridge::TerminalBridgeRx, rmcs_executor::Component)
