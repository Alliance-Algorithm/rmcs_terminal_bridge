#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "rmcs_terminal_bridge/payload_codec.hpp"
#include "rmcs_terminal_bridge/types.hpp"
#include "teleop_ipc/control_link.hpp"
#include "teleop_ipc/error.hpp"
#include "teleop_ipc/frame.hpp"
#include "teleop_ipc/seqpacket.hpp"
#include "teleop_ipc/session.hpp"

namespace rmcs_terminal_bridge {

namespace tlink = teleop::link;

inline constexpr std::uint32_t kHealthBridgeRunning = 1U << 0U;
inline constexpr std::uint32_t kHealthClientOnline = 1U << 1U;
inline constexpr std::uint32_t kHealthControlReady = 1U << 2U;
inline constexpr std::int64_t kReopenIntervalNs = 1'000'000'000;

struct LinkConfig {
    std::string socket_path;
    mode_t socket_mode{0666};
    int allowed_uid{1000};
    double send_rate_hz{75.0};
    double health_rate_hz{1.0};
    std::string protocol_rev{"2027-V1.0.0"};
    std::int64_t handshake_timeout_ns{2'000'000'000};
    std::uint32_t max_body_bytes{512};
    std::size_t rcvbuf_bytes{4096};
    std::size_t sndbuf_bytes{4096};
    teleop::link::NowFn now_ns{&teleop::link::monotonic_now_ns};
};

struct LinkStats {
    std::uint64_t accepts{0};
    std::uint64_t replaces{0};
    std::uint64_t peer_uid_rejected{0};
    std::uint64_t handshakes{0};
    std::uint64_t handshake_timeouts{0};
    std::uint64_t handshake_errors{0};
    std::uint64_t version_mismatches{0};
    std::uint64_t disconnects{0};
    std::uint64_t rx_down_frames{0};
    std::uint64_t rx_down_bad{0};
    std::uint64_t rx_unknown{0};
    std::uint64_t rx_before_active{0};
    std::uint64_t tx_up_frames{0};
    std::uint64_t tx_up_dropped{0};
    std::uint64_t tx_up_encode_errors{0};
    std::uint64_t tx_health_frames{0};
    std::uint64_t tx_health_dropped{0};
    std::uint64_t ack_retries{0};
    std::uint64_t down_overwritten{0};
    std::int32_t peer_uid{-1};
    std::int64_t last_accept_ns{0};
    std::int64_t last_rx_ns{0};
    std::int64_t last_tx_ns{0};
};

struct DownPacket {
    CustomByteBlockData data{};
    std::uint64_t seq{0};
    std::int64_t t_recv_ns{0};
};

// 桥侧 UDS acceptor：open/poll_recv/poll_send 必须在同一个 executor 更新线程调用。
// 不依赖 ROS；日志由 TerminalBridgeRx 读 stats()/last_error()。
class Link {
public:
    explicit Link(LinkConfig config);
    ~Link();

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;
    Link(Link&&) = delete;
    Link& operator=(Link&&) = delete;

    bool open();
    void close();

    void poll_recv(std::int64_t now_ns);
    void poll_send(std::int64_t now_ns, const CustomControlData* up);
    bool take_down(DownPacket& out);

    [[nodiscard]] bool connected() const noexcept { return active_; }

    [[nodiscard]] const LinkStats& stats() const noexcept { return stats_; }

    [[nodiscard]] const teleop::link::Error& last_error() const noexcept { return last_error_; }

    [[nodiscard]] std::string_view client_version() const noexcept { return client_version_; }

    [[nodiscard]] std::uint32_t peer_proto_version() const noexcept { return peer_proto_version_; }

    [[nodiscard]] std::uint64_t peer_capabilities() const noexcept { return peer_capabilities_; }

private:
    void handle_frame(const teleop::link::FrameView& frame, std::int64_t now_ns);
    void handle_hello(std::span<const std::byte> body);
    void handle_down(const teleop::link::FrameView& frame, std::int64_t now_ns);
    bool build_ack();
    void send_health(std::int64_t now_ns, bool control_source_ready);
    bool send_up(const CustomControlData& up, std::int64_t now_ns);
    void drop_session(teleop::link::Error error);
    void set_error(teleop::link::Error error);

    [[nodiscard]] std::int64_t send_period_ns() const noexcept;
    [[nodiscard]] std::int64_t health_period_ns() const noexcept;

    LinkConfig config_;
    std::optional<teleop::link::Listener> listener_;
    std::optional<teleop::link::Session> session_;

    std::array<std::byte, teleop::link::kHeaderBytes + 512> rx_buffer_{};
    std::array<std::byte, teleop::link::kHeaderBytes + 128> ack_frame_{};
    std::array<std::byte, teleop::link::kHeaderBytes + 128> tx_frame_{};
    std::array<std::byte, 128> body_buffer_{};
    std::array<std::byte, 64> payload_buffer_{};

    std::size_t ack_size_{0};
    bool ack_pending_{false};
    bool close_after_ack_{false};
    bool active_{false};
    std::optional<DownPacket> down_;

    std::int64_t handshake_deadline_ns_{0};
    std::int64_t next_open_ns_{0};
    std::int64_t next_send_ns_{0};
    std::int64_t next_health_ns_{0};

    LinkStats stats_{};
    teleop::link::Error last_error_{};
    std::string client_version_;
    std::uint32_t peer_proto_version_{0};
    std::uint64_t peer_capabilities_{0};
};

inline Link::Link(LinkConfig config)
    : config_{std::move(config)} {
    if (config_.now_ns == nullptr) {
        config_.now_ns = &tlink::monotonic_now_ns;
    }
    if (config_.max_body_bytes == 0 || config_.max_body_bytes > static_cast<std::uint32_t>(512)) {
        config_.max_body_bytes = 512;
    }
    if (!(config_.send_rate_hz > 0.0)) {
        config_.send_rate_hz = 75.0;
    }
    if (!(config_.health_rate_hz > 0.0)) {
        config_.health_rate_hz = 1.0;
    }
    if (config_.handshake_timeout_ns <= 0) {
        config_.handshake_timeout_ns = 2'000'000'000;
    }
}

inline Link::~Link() { close(); }

inline bool Link::open() {
    if (listener_) {
        return true;
    }
    if (config_.socket_path.empty()) {
        set_error(tlink::make_error(tlink::ErrorCode::InvalidArgument, "empty socket path"));
        return false;
    }

    const std::filesystem::path path{config_.socket_path};
    const auto parent = path.parent_path();
    std::error_code ec;
    if (!parent.empty() && !std::filesystem::is_directory(parent, ec)) {
        set_error(tlink::make_error(tlink::ErrorCode::Io, "socket directory missing", ENOENT));
        return false;
    }

    if (::unlink(config_.socket_path.c_str()) != 0 && errno != ENOENT) {
        set_error(tlink::make_error(tlink::ErrorCode::Io, "unlink socket", errno));
        return false;
    }

    auto address = tlink::UnixAddress::path(config_.socket_path);
    if (!address) {
        set_error(address.error());
        return false;
    }

    auto listener = tlink::Listener::open(
        *address, 2, tlink::SocketOptions{config_.rcvbuf_bytes, config_.sndbuf_bytes});
    if (!listener) {
        set_error(listener.error());
        return false;
    }
    if (::chmod(config_.socket_path.c_str(), config_.socket_mode) != 0) {
        set_error(tlink::make_error(tlink::ErrorCode::Io, "chmod socket", errno));
        return false;
    }

    listener_ = std::move(*listener);
    next_open_ns_ = 0;
    last_error_ = {};
    return true;
}

inline void Link::close() {
    if (session_) {
        session_->close();
        session_.reset();
    }
    listener_.reset();
    if (!config_.socket_path.empty()) {
        ::unlink(config_.socket_path.c_str());
    }
    active_ = false;
    ack_pending_ = false;
    close_after_ack_ = false;
    down_.reset();
}

inline void Link::poll_recv(std::int64_t now_ns) {
    if (!listener_) {
        if (now_ns < next_open_ns_) {
            return;
        }
        if (!open()) {
            next_open_ns_ = now_ns + kReopenIntervalNs;
            return;
        }
    }

    auto accepted =
        listener_->accept(tlink::SocketOptions{config_.rcvbuf_bytes, config_.sndbuf_bytes});
    if (accepted) {
        auto cred = accepted->peer_cred();
        if (!cred) {
            set_error(cred.error());
        } else {
            stats_.peer_uid = static_cast<std::int32_t>(cred->uid);
            if (config_.allowed_uid >= 0 && cred->uid != static_cast<uid_t>(config_.allowed_uid)) {
                ++stats_.peer_uid_rejected;
            } else {
                if (session_) {
                    session_->close();
                    session_.reset();
                    ++stats_.replaces;
                }
                tlink::SessionOptions options{};
                options.role = tlink::Role::Acceptor;
                options.session_id = 0;
                options.max_body_bytes = config_.max_body_bytes;
                options.now_ns = config_.now_ns;
                session_.emplace(std::move(*accepted), options);
                session_->begin_handshake();

                active_ = false;
                ack_pending_ = false;
                close_after_ack_ = false;
                down_.reset();
                client_version_.clear();
                peer_proto_version_ = 0;
                peer_capabilities_ = 0;
                handshake_deadline_ns_ = now_ns + config_.handshake_timeout_ns;
                ++stats_.accepts;
                stats_.last_accept_ns = now_ns;
                last_error_ = {};
            }
        }
    } else if (accepted.error().code != tlink::ErrorCode::WouldBlock) {
        set_error(accepted.error());
        listener_.reset();
        next_open_ns_ = now_ns + kReopenIntervalNs;
    }

    if (!session_) {
        return;
    }
    if (!active_ && now_ns >= handshake_deadline_ns_) {
        ++stats_.handshake_timeouts;
        drop_session(tlink::make_error(tlink::ErrorCode::Protocol, "handshake timeout"));
        return;
    }

    for (int i = 0; i < 8 && session_; ++i) {
        auto frame = session_->recv(std::span<std::byte>{rx_buffer_});
        if (!frame) {
            if (frame.error().code == tlink::ErrorCode::WouldBlock) {
                return;
            }
            drop_session(frame.error());
            return;
        }
        handle_frame(*frame, now_ns);
    }
}

inline void Link::poll_send(std::int64_t now_ns, const CustomControlData* up) {
    if (!session_) {
        return;
    }

    if (ack_pending_) {
        auto sent =
            session_->send_prepared(std::span<const std::byte>{ack_frame_.data(), ack_size_});
        if (!sent) {
            if (sent.error().code == tlink::ErrorCode::WouldBlock) {
                ++stats_.ack_retries;
                return;
            }
            drop_session(sent.error());
            return;
        }
        ack_pending_ = false;
        if (close_after_ack_) {
            drop_session(last_error_);
            return;
        }
        session_->activate();
        active_ = true;
        ++stats_.handshakes;
        next_send_ns_ = now_ns;
        next_health_ns_ = now_ns;
    }

    if (!active_) {
        return;
    }

    if (now_ns >= next_health_ns_) {
        const auto period = health_period_ns();
        send_health(now_ns, up != nullptr);
        if (!session_) {
            return;
        }
        next_health_ns_ += period;
        if (next_health_ns_ <= now_ns) {
            next_health_ns_ = now_ns + period;
        }
    }

    if (up != nullptr && now_ns >= next_send_ns_) {
        const auto period = send_period_ns();
        if (!send_up(*up, now_ns)) {
            return;
        }
        next_send_ns_ += period;
        if (next_send_ns_ <= now_ns) {
            next_send_ns_ = now_ns + period;
        }
    }
}

inline bool Link::take_down(DownPacket& out) {
    if (!down_) {
        return false;
    }
    out = std::move(*down_);
    down_.reset();
    return true;
}

inline void Link::handle_frame(const tlink::FrameView& frame, std::int64_t now_ns) {
    if (!active_) {
        if (frame.header.msg_type == tlink::kMsgHello && !ack_pending_) {
            handle_hello(frame.body);
        } else {
            ++stats_.rx_before_active;
        }
        return;
    }

    if (frame.header.msg_type == tlink::kMsgDownData) {
        handle_down(frame, now_ns);
    } else {
        ++stats_.rx_unknown;
    }
}

inline void Link::handle_hello(std::span<const std::byte> body) {
    auto hello = tlink::decode_hello(body);
    if (!hello) {
        ++stats_.handshake_errors;
        drop_session(hello.error());
        return;
    }

    peer_proto_version_ = hello->proto_version;
    peer_capabilities_ = hello->capabilities;
    client_version_.assign(hello->client_version.begin(), hello->client_version.end());

    if (hello->proto_version != tlink::kControlProtoVersion) {
        ++stats_.version_mismatches;
        close_after_ack_ = true;
        set_error(tlink::make_error(tlink::ErrorCode::Protocol, "hello version mismatch"));
    } else {
        close_after_ack_ = false;
    }

    if (!build_ack()) {
        ++stats_.handshake_errors;
        drop_session(last_error_);
    }
}

inline bool Link::build_ack() {
    if (!session_) {
        return false;
    }

    tlink::HelloAck ack{};
    ack.proto_version = tlink::kControlProtoVersion;
    ack.capabilities = tlink::kCapUpData | tlink::kCapDownData | tlink::kCapHealth;
    ack.protocol_rev = config_.protocol_rev;

    auto body_size = tlink::encode_hello_ack(ack, body_buffer_);
    if (!body_size) {
        set_error(body_size.error());
        return false;
    }
    auto frame_size = session_->prepare(
        tlink::kMsgHelloAck, std::span<const std::byte>{body_buffer_.data(), *body_size},
        ack_frame_);
    if (!frame_size) {
        set_error(frame_size.error());
        return false;
    }
    ack_size_ = *frame_size;
    ack_pending_ = true;
    return true;
}

inline void Link::handle_down(const tlink::FrameView& frame, std::int64_t now_ns) {
    auto down = tlink::decode_down_data(frame.body);
    if (!down) {
        drop_session(down.error());
        return;
    }
    if (down->channel != tlink::kChannelCustomByteBlock) {
        ++stats_.rx_down_bad;
        return;
    }
    auto block = decode_custom_byte_block(down->payload);
    if (!block) {
        ++stats_.rx_down_bad;
        return;
    }

    if (down_) {
        ++stats_.down_overwritten;
    }
    down_ = DownPacket{*block, frame.header.seq, down->t_recv_ns};
    ++stats_.rx_down_frames;
    stats_.last_rx_ns = now_ns;
}

inline void Link::send_health(std::int64_t now_ns, bool control_source_ready) {
    if (!session_ || !active_) {
        return;
    }

    std::uint32_t state = kHealthBridgeRunning;
    if (active_) {
        state |= kHealthClientOnline;
    }
    if (control_source_ready) {
        state |= kHealthControlReady;
    }

    tlink::Health health{};
    health.state = state;
    health.load_pct = 0;

    auto body_size = tlink::encode_health(health, body_buffer_);
    if (!body_size) {
        set_error(body_size.error());
        drop_session(body_size.error());
        return;
    }
    auto frame_size = session_->prepare(
        tlink::kMsgHealth, std::span<const std::byte>{body_buffer_.data(), *body_size}, tx_frame_);
    if (!frame_size) {
        set_error(frame_size.error());
        drop_session(frame_size.error());
        return;
    }
    auto sent = session_->send_prepared(std::span<const std::byte>{tx_frame_.data(), *frame_size});
    if (!sent) {
        if (sent.error().code == tlink::ErrorCode::WouldBlock) {
            ++stats_.tx_health_dropped;
            return;
        }
        drop_session(sent.error());
        return;
    }

    ++stats_.tx_health_frames;
    stats_.last_tx_ns = now_ns;
}

inline bool Link::send_up(const CustomControlData& up, std::int64_t now_ns) {
    if (!session_ || !active_) {
        return false;
    }

    auto payload_size = encode_custom_control(up, payload_buffer_);
    if (!payload_size) {
        ++stats_.tx_up_encode_errors;
        return true;
    }

    tlink::UpData data{};
    data.channel = tlink::kChannelCustomControl;
    data.t_src_ns = now_ns;
    data.payload = std::span<const std::byte>{payload_buffer_.data(), *payload_size};

    auto body_size = tlink::encode_up_data(data, body_buffer_);
    if (!body_size) {
        ++stats_.tx_up_encode_errors;
        return true;
    }
    auto frame_size = session_->prepare(
        tlink::kMsgUpData, std::span<const std::byte>{body_buffer_.data(), *body_size}, tx_frame_);
    if (!frame_size) {
        if (frame_size.error().code == tlink::ErrorCode::Closed
            || frame_size.error().code == tlink::ErrorCode::Protocol) {
            drop_session(frame_size.error());
            return false;
        }
        ++stats_.tx_up_encode_errors;
        return true;
    }
    auto sent = session_->send_prepared(std::span<const std::byte>{tx_frame_.data(), *frame_size});
    if (!sent) {
        if (sent.error().code == tlink::ErrorCode::WouldBlock) {
            ++stats_.tx_up_dropped;
            return true;
        }
        drop_session(sent.error());
        return false;
    }

    ++stats_.tx_up_frames;
    stats_.last_tx_ns = now_ns;
    return true;
}

inline void Link::drop_session(tlink::Error error) {
    if (session_) {
        session_->close();
        session_.reset();
    }
    active_ = false;
    ack_pending_ = false;
    close_after_ack_ = false;
    down_.reset();
    last_error_ = error;
    ++stats_.disconnects;
}

inline void Link::set_error(tlink::Error error) { last_error_ = error; }

inline std::int64_t Link::send_period_ns() const noexcept {
    return std::max<std::int64_t>(
        1, static_cast<std::int64_t>(1'000'000'000.0 / config_.send_rate_hz));
}

inline std::int64_t Link::health_period_ns() const noexcept {
    return std::max<std::int64_t>(
        1, static_cast<std::int64_t>(1'000'000'000.0 / config_.health_rate_hz));
}

} // namespace rmcs_terminal_bridge
