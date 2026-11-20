#pragma once

#include <unistd.h>

#include <utility>

namespace teleop::link {

// 只负责关闭；线程安全性与 fd 本身一致（同 fd 不要并发 close）。
class UniqueFd {
public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept
        : fd_{fd} {}

    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept
        : fd_{std::exchange(other.fd_, -1)} {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

} // namespace teleop::link
