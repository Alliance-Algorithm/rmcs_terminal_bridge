#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rmcs_terminal_bridge {

inline constexpr std::size_t kCustomControlMaxBytes = 60;
inline constexpr std::size_t kCustomByteBlockMaxBytes = 300;

// 数据段：CustomControl.data 的内容，内层格式由自定义控制作者定义。
struct CustomControlData {
    std::array<std::byte, kCustomControlMaxBytes> data{};
    std::uint16_t size{0};
};

// 数据段：CustomByteBlock.data 的内容，内层格式由自定义控制作者定义。
struct CustomByteBlockData {
    std::array<std::byte, kCustomByteBlockMaxBytes> data{};
    std::uint16_t size{0};
};

} // namespace rmcs_terminal_bridge
