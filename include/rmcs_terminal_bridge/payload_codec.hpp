#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rmcs_terminal_bridge/types.hpp"
#include "teleop_ipc/error.hpp"
#include "teleop_ipc/wire.hpp"

namespace rmcs_terminal_bridge {

using teleop::link::Result;

// 官方 CustomControl { optional bytes data = 1 }：0A <varint len> <data>。
inline Result<std::size_t>
    encode_custom_control(const CustomControlData& control, std::span<std::byte> out) {
    using teleop::link::ErrorCode;
    using teleop::link::make_error;

    if (control.size > kCustomControlMaxBytes) {
        return tl::make_unexpected(make_error(ErrorCode::TooLarge, "custom control too large"));
    }

    teleop::link::wire::Writer writer{out};
    auto result = writer.bytes(
        1, std::span<const std::byte>{control.data.data(), static_cast<std::size_t>(control.size)});
    if (!result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

// 官方 CustomByteBlock { optional bytes data = 1 }：0A <varint len> <data>。
// 未知字段跳过；len > 300 拒绝；字段缺失按 size=0。
inline Result<CustomByteBlockData> decode_custom_byte_block(std::span<const std::byte> in) {
    using teleop::link::ErrorCode;
    using teleop::link::make_error;

    teleop::link::wire::Reader reader{in};
    CustomByteBlockData block{};

    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;

        if (field == 1 && type == teleop::link::wire::Type::LengthDelimited) {
            auto bytes = reader.bytes();
            if (!bytes) {
                return tl::make_unexpected(bytes.error());
            }
            if (bytes->size() > kCustomByteBlockMaxBytes) {
                return tl::make_unexpected(
                    make_error(ErrorCode::TooLarge, "custom byte block too large"));
            }
            block.size = static_cast<std::uint16_t>(bytes->size());
            std::copy(bytes->begin(), bytes->end(), block.data.begin());
            continue;
        }

        auto skipped = reader.skip(type);
        if (!skipped) {
            return tl::make_unexpected(skipped.error());
        }
    }

    return block;
}

} // namespace rmcs_terminal_bridge
