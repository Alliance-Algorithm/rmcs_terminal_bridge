#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <span>
#include <sys/random.h>
#include <unistd.h>
#include <utility>

#include "teleop_ipc/error.hpp"
#include "teleop_ipc/frame.hpp"
#include "teleop_ipc/seqpacket.hpp"

namespace teleop::link {

enum class Role : std::uint8_t {
    Connector,
    Acceptor,
};

enum class SessionState : std::uint8_t {
    Idle,
    Handshaking,
    Active,
    Closed,
    Failed,
};

using NowFn = std::int64_t (*)() noexcept;

inline std::int64_t monotonic_now_ns() noexcept {
    timespec timestamp{};
    if (::clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0) {
        return 0;
    }
    return static_cast<std::int64_t>(timestamp.tv_sec) * 1'000'000'000
         + static_cast<std::int64_t>(timestamp.tv_nsec);
}

inline std::uint64_t generate_session_id() noexcept {
    std::uint64_t value = 0;
#if defined(__linux__)
    if (::getrandom(&value, sizeof value, 0) != static_cast<ssize_t>(sizeof value)) {
        value = 0;
    }
#endif
    if (value == 0) {
        static std::atomic<std::uint64_t> counter{0};
        const auto now = static_cast<std::uint64_t>(monotonic_now_ns());
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto index = counter.fetch_add(1, std::memory_order_relaxed);
        value = now ^ (pid << 32U) ^ (index * 0x9E3779B97F4A7C15ULL);
    }
    return value == 0 ? 1 : value;
}

struct SessionOptions {
    Role role = Role::Connector;
    std::uint64_t session_id = 0; // 0 = 自动生成
    std::uint32_t max_body_bytes = static_cast<std::uint32_t>(kMaxBodyBytes);
    NowFn now_ns = &monotonic_now_ns;
};

struct SessionStats {
    std::uint64_t tx_frames = 0;
    std::uint64_t rx_frames = 0;
    std::uint64_t rx_gaps = 0;
    std::uint64_t rx_anomalies = 0;
    std::uint64_t peer_session_changes = 0;
};

// 非线程安全；所有调用必须在同一个线程。send/recv 可以由不同组件调用。
class Session {
public:
    Session(Socket socket, const SessionOptions& options)
        : socket_{std::move(socket)}
        , role_{options.role}
        , session_id_{options.session_id == 0 ? generate_session_id() : options.session_id}
        , max_body_bytes_{std::clamp<std::size_t>(
              options.max_body_bytes, 1, static_cast<std::size_t>(kMaxBodyBytes))}
        , now_ns_{options.now_ns == nullptr ? &monotonic_now_ns : options.now_ns} {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    ~Session() = default;

    [[nodiscard]] int fd() const noexcept { return socket_.fd(); }

    [[nodiscard]] Role role() const noexcept { return role_; }

    [[nodiscard]] SessionState state() const noexcept { return state_; }

    [[nodiscard]] std::uint64_t session_id() const noexcept { return session_id_; }

    [[nodiscard]] std::uint64_t tx_seq() const noexcept { return tx_seq_; }

    [[nodiscard]] std::uint64_t peer_session_id() const noexcept { return peer_session_id_; }

    [[nodiscard]] const SessionStats& stats() const noexcept { return stats_; }

    [[nodiscard]] std::int64_t now_ns() const noexcept { return now_ns_(); }

    [[nodiscard]] Socket& socket() noexcept { return socket_; }

    void begin_handshake() noexcept {
        if (state_ == SessionState::Idle) {
            state_ = SessionState::Handshaking;
        }
    }

    void activate() noexcept {
        if (state_ == SessionState::Handshaking) {
            state_ = SessionState::Active;
        }
    }

    void fail() noexcept { state_ = SessionState::Failed; }

    void close() noexcept {
        socket_.close();
        state_ = SessionState::Closed;
    }

    // 分配下一 seq 并编码；返回帧长。seq 在这里消耗，即使后续 send 失败。
    Result<std::size_t>
        prepare(std::uint8_t msg_type, std::span<const std::byte> body, std::span<std::byte> out) {
        if (state_ == SessionState::Closed || state_ == SessionState::Failed) {
            return tl::make_unexpected(make_error(ErrorCode::Closed, "session closed"));
        }
        if (body.size() > max_body_bytes_) {
            return tl::make_unexpected(make_error(ErrorCode::TooLarge, "body exceeds session max"));
        }

        FrameHeader header{};
        header.proto_version = kFrameVersion;
        header.msg_type = msg_type;
        header.session_id = session_id_;
        header.seq = ++tx_seq_;
        header.t_mono_ns = now_ns_();
        return encode_frame(header, body, out);
    }

    // 发送已经编码好的帧（可用于 ACK 的同一 seq 重试）。
    Result<void> send_prepared(std::span<const std::byte> frame) {
        if (state_ == SessionState::Closed || state_ == SessionState::Failed) {
            return tl::make_unexpected(make_error(ErrorCode::Closed, "session closed"));
        }
        if (frame.size() < kHeaderBytes) {
            return tl::make_unexpected(make_error(ErrorCode::InvalidArgument, "short frame"));
        }
        auto result = socket_.send(frame);
        if (!result) {
            return result;
        }
        ++stats_.tx_frames;
        return {};
    }

    Result<FrameView> recv(std::span<std::byte> buffer) {
        if (state_ == SessionState::Closed || state_ == SessionState::Failed) {
            return tl::make_unexpected(make_error(ErrorCode::Closed, "session closed"));
        }
        if (buffer.size() < kHeaderBytes) {
            return tl::make_unexpected(
                make_error(ErrorCode::InvalidArgument, "receive buffer too small"));
        }

        auto received = socket_.recv(buffer);
        if (!received) {
            if (received.error().code == ErrorCode::Closed) {
                state_ = SessionState::Closed;
            }
            return tl::make_unexpected(received.error());
        }
        if (received->truncated) {
            state_ = SessionState::Failed;
            return tl::make_unexpected(make_error(ErrorCode::TooLarge, "record truncated"));
        }

        const std::size_t length = received->length;
        if (length < kHeaderBytes) {
            state_ = SessionState::Failed;
            return tl::make_unexpected(make_error(ErrorCode::Truncated, "record too short"));
        }

        const std::span<const std::byte, kHeaderBytes> header_bytes{buffer.data(), kHeaderBytes};
        auto header = decode_header(header_bytes);
        if (!header) {
            state_ = SessionState::Failed;
            return tl::make_unexpected(header.error());
        }
        if (auto valid = validate_frame_length(*header, length); !valid) {
            state_ = SessionState::Failed;
            return tl::make_unexpected(valid.error());
        }
        if (header->session_id == 0) {
            state_ = SessionState::Failed;
            return tl::make_unexpected(make_error(ErrorCode::Protocol, "zero session id"));
        }

        if (peer_session_id_ == 0) {
            peer_session_id_ = header->session_id;
        } else if (peer_session_id_ != header->session_id) {
            ++stats_.peer_session_changes;
            state_ = SessionState::Failed;
            return tl::make_unexpected(make_error(ErrorCode::Protocol, "peer session changed"));
        }

        if (stats_.rx_frames == 0) {
            last_seq_ = header->seq;
        } else if (header->seq > last_seq_ + 1) {
            stats_.rx_gaps += header->seq - last_seq_ - 1;
            last_seq_ = header->seq;
        } else if (header->seq <= last_seq_) {
            ++stats_.rx_anomalies;
            last_seq_ = header->seq;
        } else {
            last_seq_ = header->seq;
        }
        ++stats_.rx_frames;

        return FrameView{*header, buffer.subspan(kHeaderBytes, header->body_len)};
    }

private:
    Socket socket_;
    Role role_ = Role::Connector;
    SessionState state_ = SessionState::Idle;
    std::uint64_t session_id_ = 0;
    std::uint64_t peer_session_id_ = 0;
    std::uint64_t tx_seq_ = 0;
    std::uint64_t last_seq_ = 0;
    std::size_t max_body_bytes_ = kMaxBodyBytes;
    NowFn now_ns_ = &monotonic_now_ns;
    SessionStats stats_{};
};

} // namespace teleop::link
