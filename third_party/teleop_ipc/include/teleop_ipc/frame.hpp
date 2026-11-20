#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "teleop_ipc/error.hpp"

namespace teleop::link {

// 线上格式：全部多字节字段大端；magic 是字节对 'T','L'。
inline constexpr std::uint8_t kFrameVersion = 1;
inline constexpr std::size_t kHeaderBytes = 32;
inline constexpr std::size_t kMaxFrameBytes = 64 * 1024;
inline constexpr std::size_t kMaxBodyBytes = kMaxFrameBytes - kHeaderBytes;
inline constexpr std::byte kMagic0{0x54};
inline constexpr std::byte kMagic1{0x4C};

struct FrameHeader {
    std::uint8_t proto_version = kFrameVersion;
    std::uint8_t msg_type = 0;
    std::uint16_t flags = 0;
    std::uint64_t session_id = 0;
    std::uint64_t seq = 0;
    std::int64_t t_mono_ns = 0;
    std::uint16_t body_len = 0;
};

struct FrameView {
    FrameHeader header{};
    std::span<const std::byte> body{};
};

namespace detail {

inline void put_u16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
    out[offset] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    out[offset + 1] = static_cast<std::byte>(value & 0xFFU);
}

inline void put_u64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::byte>((value >> ((7U - i) * 8U)) & 0xFFU);
    }
}

inline std::uint16_t get_u16(std::span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(in[offset]) << 8U)
        | static_cast<std::uint16_t>(in[offset + 1]));
}

inline std::uint64_t get_u64(std::span<const std::byte> in, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value = (value << 8U) | static_cast<std::uint64_t>(in[offset + i]);
    }
    return value;
}

} // namespace detail

inline std::array<std::byte, kHeaderBytes> encode_header(const FrameHeader& header) {
    std::array<std::byte, kHeaderBytes> out{};
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = static_cast<std::byte>(header.proto_version);
    out[3] = static_cast<std::byte>(header.msg_type);
    detail::put_u16(out, 4, header.flags);
    detail::put_u16(out, 6, header.body_len);
    detail::put_u64(out, 8, header.session_id);
    detail::put_u64(out, 16, header.seq);
    detail::put_u64(out, 24, static_cast<std::uint64_t>(header.t_mono_ns));
    return out;
}

inline Result<FrameHeader> decode_header(std::span<const std::byte, kHeaderBytes> bytes) {
    if (bytes[0] != kMagic0 || bytes[1] != kMagic1) {
        return tl::make_unexpected(make_error(ErrorCode::Protocol, "bad magic"));
    }
    const auto proto_version = static_cast<std::uint8_t>(bytes[2]);
    if (proto_version != kFrameVersion) {
        return tl::make_unexpected(make_error(ErrorCode::Protocol, "unsupported frame version"));
    }

    FrameHeader header{};
    header.proto_version = proto_version;
    header.msg_type = static_cast<std::uint8_t>(bytes[3]);
    header.flags = detail::get_u16(bytes, 4);
    header.body_len = detail::get_u16(bytes, 6);
    header.session_id = detail::get_u64(bytes, 8);
    header.seq = detail::get_u64(bytes, 16);
    header.t_mono_ns = static_cast<std::int64_t>(detail::get_u64(bytes, 24));

    if (header.body_len > kMaxBodyBytes) {
        return tl::make_unexpected(make_error(ErrorCode::TooLarge, "body too large"));
    }
    return header;
}

inline std::size_t layout_size(const FrameHeader& header) noexcept {
    return kHeaderBytes + static_cast<std::size_t>(header.body_len);
}

inline Result<void> validate_frame_length(const FrameHeader& header, std::size_t length) {
    if (layout_size(header) > kMaxFrameBytes) {
        return tl::make_unexpected(make_error(ErrorCode::TooLarge, "frame too large"));
    }
    if (length < kHeaderBytes) {
        return tl::make_unexpected(make_error(ErrorCode::Truncated, "record shorter than header"));
    }
    if (length != layout_size(header)) {
        return tl::make_unexpected(make_error(ErrorCode::Protocol, "frame length mismatch"));
    }
    return {};
}

inline Result<std::size_t> encode_frame(
    const FrameHeader& header, std::span<const std::byte> body, std::span<std::byte> out) {
    if (body.size() > kMaxBodyBytes) {
        return tl::make_unexpected(make_error(ErrorCode::TooLarge, "body too large"));
    }

    const std::size_t size = kHeaderBytes + body.size();
    if (out.size() < size) {
        return tl::make_unexpected(make_error(ErrorCode::TooLarge, "output buffer too small"));
    }

    FrameHeader adjusted = header;
    adjusted.body_len = static_cast<std::uint16_t>(body.size());
    const auto bytes = encode_header(adjusted);
    std::memcpy(out.data(), bytes.data(), kHeaderBytes);
    if (!body.empty()) {
        std::memcpy(out.data() + kHeaderBytes, body.data(), body.size());
    }
    return size;
}

} // namespace teleop::link
