#include <cstddef>
#include <cstdint>

#include <pluginlib/class_list_macros.hpp>
#include <rmcs_executor/component.hpp>

#include "rmcs_terminal_bridge/types.hpp"

namespace rmcs_terminal_bridge {

// 仅用于组件级联调；生产 YAML 不列，不会实例化。
class TerminalTestSource : public rmcs_executor::Component {
public:
    TerminalTestSource() { register_output("/terminal/custom_control", control_); }

    void update() override {
        control_->size = static_cast<std::uint16_t>(kCustomControlMaxBytes);
        for (std::size_t i = 0; i < kCustomControlMaxBytes; ++i) {
            control_->data[i] = static_cast<std::byte>((counter_ + i) & 0xFFU);
        }
        ++counter_;
    }

private:
    OutputInterface<CustomControlData> control_;
    std::uint64_t counter_{0};
};

class TerminalTestSink : public rmcs_executor::Component {
public:
    TerminalTestSink() { register_input("/terminal/custom_byte_block", down_); }

    void update() override {
        if (down_.ready()) {
            last_size_ = down_->size;
            ++counter_;
        }
    }

private:
    InputInterface<CustomByteBlockData> down_;
    std::uint64_t counter_{0};
    std::uint16_t last_size_{0};
};

} // namespace rmcs_terminal_bridge

PLUGINLIB_EXPORT_CLASS(rmcs_terminal_bridge::TerminalTestSource, rmcs_executor::Component)
PLUGINLIB_EXPORT_CLASS(rmcs_terminal_bridge::TerminalTestSink, rmcs_executor::Component)
