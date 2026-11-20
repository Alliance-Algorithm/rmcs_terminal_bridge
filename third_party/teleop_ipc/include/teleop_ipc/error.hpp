#pragma once

#include <cstdint>
#include <string_view>

#include <tl/expected.hpp>

namespace teleop::link {

// 失败一律走 Result，不抛异常；context 必须是静态字符串（只用于日志/断言）。
enum class ErrorCode : std::uint8_t {
    None = 0,
    InvalidArgument,
    WouldBlock,
    Closed,
    Protocol,
    Truncated,
    TooLarge,
    Unsupported,
    Io,
};

struct Error {
    ErrorCode code = ErrorCode::None;
    int sys_errno = 0;
    std::string_view context{};
};

template <typename T>
using Result = tl::expected<T, Error>;

inline Error make_error(ErrorCode code, std::string_view context = {}, int sys_errno = 0) noexcept {
    return Error{code, sys_errno, context};
}

inline std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::None: return "none";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::WouldBlock: return "would_block";
    case ErrorCode::Closed: return "closed";
    case ErrorCode::Protocol: return "protocol";
    case ErrorCode::Truncated: return "truncated";
    case ErrorCode::TooLarge: return "too_large";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Io: return "io";
    }
    return "unknown";
}

inline bool is_would_block(const Error& error) noexcept {
    return error.code == ErrorCode::WouldBlock;
}

} // namespace teleop::link
