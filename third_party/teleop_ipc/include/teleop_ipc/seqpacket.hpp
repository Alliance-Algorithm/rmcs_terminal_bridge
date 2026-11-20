#pragma once

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "teleop_ipc/error.hpp"
#include "teleop_ipc/fd.hpp"

namespace teleop::link {

struct PeerCred {
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
};

struct SocketOptions {
    std::size_t rcvbuf_bytes = 0;
    std::size_t sndbuf_bytes = 0;
};

struct RecvResult {
    std::size_t length = 0;
    bool truncated = false;
};

namespace detail {

inline Error io_error(ErrorCode fallback, std::string_view context) noexcept {
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK) {
        return make_error(ErrorCode::WouldBlock, context, error);
    }
    if (error == EINPROGRESS || error == EALREADY) {
        return make_error(ErrorCode::WouldBlock, context, error);
    }
    return make_error(fallback, context, error);
}

inline Result<void> apply_socket_options(int fd, const SocketOptions& options) {
    if (options.rcvbuf_bytes > 0) {
        const auto value = static_cast<int>(std::min<std::size_t>(
            options.rcvbuf_bytes, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof value) < 0) {
            return tl::make_unexpected(io_error(ErrorCode::Io, "setsockopt SO_RCVBUF"));
        }
    }
    if (options.sndbuf_bytes > 0) {
        const auto value = static_cast<int>(std::min<std::size_t>(
            options.sndbuf_bytes, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof value) < 0) {
            return tl::make_unexpected(io_error(ErrorCode::Io, "setsockopt SO_SNDBUF"));
        }
    }
    return {};
}

} // namespace detail

class UnixAddress {
public:
    static Result<UnixAddress> path(std::string_view path) {
        if (path.empty() || path.find('\0') != std::string_view::npos
            || path.size() + 1 > sizeof(sockaddr_un::sun_path)) {
            return tl::make_unexpected(make_error(ErrorCode::InvalidArgument, "bad socket path"));
        }
        UnixAddress address;
        address.addr_.sun_family = AF_UNIX;
        std::memcpy(address.addr_.sun_path, path.data(), path.size());
        address.addr_.sun_path[path.size()] = '\0';
        address.length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        address.abstract_ = false;
        return address;
    }

    static Result<UnixAddress> abstract(std::string_view name) {
        if (name.empty() || name.find('\0') != std::string_view::npos
            || name.size() + 1 > sizeof(sockaddr_un::sun_path)) {
            return tl::make_unexpected(
                make_error(ErrorCode::InvalidArgument, "bad abstract socket name"));
        }
        UnixAddress address;
        address.addr_.sun_family = AF_UNIX;
        address.addr_.sun_path[0] = '\0';
        std::memcpy(address.addr_.sun_path + 1, name.data(), name.size());
        address.length_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        address.abstract_ = true;
        return address;
    }

    [[nodiscard]] const sockaddr* addr() const noexcept {
        return reinterpret_cast<const sockaddr*>(&addr_);
    }

    [[nodiscard]] socklen_t size() const noexcept { return length_; }

    [[nodiscard]] bool is_abstract() const noexcept { return abstract_; }

private:
    sockaddr_un addr_{};
    socklen_t length_ = 0;
    bool abstract_ = false;
};

class Listener;

class Socket {
public:
    Socket() = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    ~Socket() = default;

    static Result<Socket> connect(const UnixAddress& address, const SocketOptions& options = {}) {
        UniqueFd fd{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        if (!fd.valid()) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "socket"));
        }
        if (auto result = detail::apply_socket_options(fd.get(), options); !result) {
            return tl::make_unexpected(result.error());
        }
        if (::connect(fd.get(), address.addr(), address.size()) < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "connect"));
        }
        return Socket{std::move(fd)};
    }

    static Result<std::pair<Socket, Socket>> pair(const SocketOptions& options = {}) {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "socketpair"));
        }
        UniqueFd first{fds[0]};
        UniqueFd second{fds[1]};
        if (auto result = detail::apply_socket_options(first.get(), options); !result) {
            return tl::make_unexpected(result.error());
        }
        if (auto result = detail::apply_socket_options(second.get(), options); !result) {
            return tl::make_unexpected(result.error());
        }
        return std::pair<Socket, Socket>{Socket{std::move(first)}, Socket{std::move(second)}};
    }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }

    void close() noexcept { fd_.reset(); }

    Result<void> set_buffers(const SocketOptions& options) {
        return detail::apply_socket_options(fd_.get(), options);
    }

    Result<RecvResult> recv(std::span<std::byte> buffer) {
        iovec iov{};
        iov.iov_base = buffer.data();
        iov.iov_len = buffer.size();

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        const ssize_t length = ::recvmsg(fd_.get(), &msg, MSG_TRUNC);
        if (length < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "recvmsg"));
        }
        if (length == 0) {
            return tl::make_unexpected(make_error(ErrorCode::Closed, "peer closed"));
        }
        const auto real = static_cast<std::size_t>(length);
        return RecvResult{real, real > buffer.size()};
    }

    Result<void> send(std::span<const std::byte> data) {
        const ssize_t written = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
        if (written < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "send"));
        }
        if (static_cast<std::size_t>(written) != data.size()) {
            return tl::make_unexpected(make_error(ErrorCode::Io, "short send"));
        }
        return {};
    }

    Result<PeerCred> peer_cred() const {
        ucred cred{};
        socklen_t length = sizeof cred;
        if (::getsockopt(fd_.get(), SOL_SOCKET, SO_PEERCRED, &cred, &length) < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "getsockopt SO_PEERCRED"));
        }
        return PeerCred{cred.pid, cred.uid, cred.gid};
    }

private:
    friend class Listener;

    explicit Socket(UniqueFd fd) noexcept
        : fd_{std::move(fd)} {}

    UniqueFd fd_{};
};

class Listener {
public:
    Listener() = default;
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;
    Listener(Listener&&) noexcept = default;
    Listener& operator=(Listener&&) noexcept = default;
    ~Listener() = default;

    static Result<Listener>
        open(const UnixAddress& address, int backlog = 2, const SocketOptions& options = {}) {
        UniqueFd fd{::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
        if (!fd.valid()) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "socket"));
        }
        if (auto result = detail::apply_socket_options(fd.get(), options); !result) {
            return tl::make_unexpected(result.error());
        }
        if (::bind(fd.get(), address.addr(), address.size()) < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "bind"));
        }
        if (::listen(fd.get(), backlog) < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "listen"));
        }
        return Listener{std::move(fd)};
    }

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }

    void close() noexcept { fd_.reset(); }

    Result<Socket> accept(const SocketOptions& options = {}) const {
        const int fd = ::accept4(fd_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            return tl::make_unexpected(detail::io_error(ErrorCode::Io, "accept4"));
        }
        UniqueFd accepted{fd};
        if (auto result = detail::apply_socket_options(accepted.get(), options); !result) {
            return tl::make_unexpected(result.error());
        }
        return Socket{std::move(accepted)};
    }

private:
    explicit Listener(UniqueFd fd) noexcept
        : fd_{std::move(fd)} {}

    UniqueFd fd_{};
};

} // namespace teleop::link
