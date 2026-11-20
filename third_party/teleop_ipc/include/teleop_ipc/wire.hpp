#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "teleop_ipc/error.hpp"

namespace teleop::link::wire {

enum class Type : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

inline constexpr std::uint32_t kMaxFieldNumber = (1U << 29U) - 1U;

constexpr bool is_valid_type(std::uint8_t type) noexcept {
    return type == 0 || type == 1 || type == 2 || type == 5;
}

// 只写不分配；out 不够时返回 TooLarge，已写内容不保证清零。
class Writer {
public:
    explicit Writer(std::span<std::byte> out) noexcept
        : out_{out} {}

    [[nodiscard]] std::size_t size() const noexcept { return pos_; }

    Result<void> varint(std::uint64_t value) {
        do {
            if (pos_ >= out_.size()) {
                return tl::make_unexpected(make_error(ErrorCode::TooLarge, "proto buffer full"));
            }
            std::byte byte = static_cast<std::byte>(value & 0x7FU);
            value >>= 7U;
            if (value != 0) {
                byte |= std::byte{0x80};
            }
            out_[pos_++] = byte;
        } while (value != 0);
        return {};
    }

    Result<void> tag(std::uint32_t field, Type type) {
        if (field == 0 || field > kMaxFieldNumber) {
            return tl::make_unexpected(make_error(ErrorCode::InvalidArgument, "bad proto field"));
        }
        return varint((static_cast<std::uint64_t>(field) << 3U) | static_cast<std::uint8_t>(type));
    }

    Result<void> bytes(std::uint32_t field, std::span<const std::byte> value) {
        if (auto result = tag(field, Type::LengthDelimited); !result) {
            return result;
        }
        if (auto result = varint(value.size()); !result) {
            return result;
        }
        if (out_.size() - pos_ < value.size()) {
            return tl::make_unexpected(make_error(ErrorCode::TooLarge, "proto buffer full"));
        }
        if (!value.empty()) {
            std::memcpy(out_.data() + pos_, value.data(), value.size());
            pos_ += value.size();
        }
        return {};
    }

    Result<void> string(std::uint32_t field, std::string_view value) {
        // 协议边界：只把字符字节当作不透明字节，不做 UTF-8 校验。
        const auto* data = reinterpret_cast<const std::byte*>(value.data());
        return bytes(field, std::span<const std::byte>{data, value.size()});
    }

private:
    std::span<std::byte> out_;
    std::size_t pos_ = 0;
};

// 只读不分配；返回的 span/string_view 指向输入缓冲区。
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept
        : in_{in} {}

    [[nodiscard]] bool done() const noexcept { return pos_ >= in_.size(); }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

    Result<std::pair<std::uint32_t, Type>> tag() {
        auto raw = varint();
        if (!raw) {
            return tl::make_unexpected(raw.error());
        }
        const std::uint64_t value = *raw;
        const auto field = static_cast<std::uint32_t>(value >> 3U);
        const auto type = static_cast<std::uint8_t>(value & 0x07U);
        if (field == 0 || field > kMaxFieldNumber || !is_valid_type(type)) {
            return tl::make_unexpected(make_error(ErrorCode::Protocol, "bad proto tag"));
        }
        return std::pair{field, static_cast<Type>(type)};
    }

    Result<std::uint64_t> varint() {
        std::uint64_t value = 0;
        for (std::uint32_t index = 0; index < 10; ++index) {
            if (pos_ >= in_.size()) {
                return tl::make_unexpected(make_error(ErrorCode::Truncated, "truncated varint"));
            }
            const auto byte = static_cast<std::uint8_t>(in_[pos_++]);
            if (index == 9 && (byte & 0x7EU) != 0) {
                return tl::make_unexpected(make_error(ErrorCode::Protocol, "varint overflow"));
            }
            value |= static_cast<std::uint64_t>(byte & 0x7FU) << (index * 7U);
            if ((byte & 0x80U) == 0) {
                return value;
            }
        }
        return tl::make_unexpected(make_error(ErrorCode::Protocol, "varint too long"));
    }

    Result<std::span<const std::byte>> bytes() {
        auto length_result = varint();
        if (!length_result) {
            return tl::make_unexpected(length_result.error());
        }
        const std::uint64_t length = *length_result;
        if (length > in_.size() - pos_) {
            return tl::make_unexpected(make_error(ErrorCode::Truncated, "truncated bytes field"));
        }
        const auto data = in_.subspan(pos_, static_cast<std::size_t>(length));
        pos_ += static_cast<std::size_t>(length);
        return data;
    }

    Result<void> skip(Type type) {
        switch (type) {
        case Type::Varint:
            if (auto result = varint(); !result) {
                return tl::make_unexpected(result.error());
            }
            return {};
        case Type::Fixed64:
            if (in_.size() - pos_ < 8) {
                return tl::make_unexpected(make_error(ErrorCode::Truncated, "truncated fixed64"));
            }
            pos_ += 8;
            return {};
        case Type::LengthDelimited:
            if (auto result = bytes(); !result) {
                return tl::make_unexpected(result.error());
            }
            return {};
        case Type::Fixed32:
            if (in_.size() - pos_ < 4) {
                return tl::make_unexpected(make_error(ErrorCode::Truncated, "truncated fixed32"));
            }
            pos_ += 4;
            return {};
        }
        return tl::make_unexpected(make_error(ErrorCode::Protocol, "unsupported wire type"));
    }

private:
    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

inline std::string_view as_string_view(std::span<const std::byte> bytes) noexcept {
    // 协议边界：string_view 不复制，调用方必须保证输入缓冲区存活。
    return std::string_view{
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(bytes.size())};
}

} // namespace teleop::link::wire
