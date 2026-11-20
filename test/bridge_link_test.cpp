#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "rmcs_terminal_bridge/link.hpp"
#include "rmcs_terminal_bridge/types.hpp"

namespace {

namespace bridge = rmcs_terminal_bridge;
namespace link = teleop::link;

struct Options {
    std::string socket_path{"/tmp/rmcs_terminal_bridge_link_test.sock"};
    double duration_s{10.0};
    int allowed_uid{-1};
    double send_rate_hz{75.0};
    double health_rate_hz{1.0};
};

void print_usage(const char* argv0) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "  --socket PATH        UDS path (default: /tmp/rmcs_terminal_bridge_link_test.sock)\n"
        "  --duration SEC       run time in seconds (default: 10)\n"
        "  --allowed-uid UID    expected client uid, -1 disables (default: -1)\n"
        "  --send-rate HZ       UP_DATA rate (default: 75)\n"
        "  --health-rate HZ     HEALTH rate (default: 1)\n"
        "  --help               show this help\n",
        argv0);
}

bool parse_double(const char* text, double& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parse_int(const char* text, int& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

std::string format_error(const link::Error& error) {
    if (error.code == link::ErrorCode::None && error.context.empty() && error.sys_errno == 0) {
        return "none";
    }
    std::string out{link::error_code_name(error.code)};
    if (!error.context.empty()) {
        out += ':';
        out.append(error.context.data(), error.context.size());
    }
    if (error.sys_errno != 0) {
        out += " errno=";
        out += std::to_string(error.sys_errno);
        const char* text = std::strerror(error.sys_errno);
        out += " (";
        out += text != nullptr ? text : "?";
        out += ')';
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--socket" && i + 1 < argc) {
            options.socket_path = argv[++i];
            continue;
        }
        if (arg == "--duration" && i + 1 < argc && parse_double(argv[i + 1], options.duration_s)
            && options.duration_s > 0.0) {
            ++i;
            continue;
        }
        if (arg == "--allowed-uid" && i + 1 < argc && parse_int(argv[i + 1], options.allowed_uid)) {
            ++i;
            continue;
        }
        if (arg == "--send-rate" && i + 1 < argc && parse_double(argv[i + 1], options.send_rate_hz)
            && options.send_rate_hz > 0.0) {
            ++i;
            continue;
        }
        if (arg == "--health-rate" && i + 1 < argc
            && parse_double(argv[i + 1], options.health_rate_hz) && options.health_rate_hz > 0.0) {
            ++i;
            continue;
        }
        std::fprintf(
            stderr, "bridge_link_test: invalid argument '%.*s'\n", static_cast<int>(arg.size()),
            arg.data());
        return 2;
    }

    bridge::LinkConfig config;
    config.socket_path = options.socket_path;
    config.socket_mode = 0600;
    config.allowed_uid = options.allowed_uid;
    config.send_rate_hz = options.send_rate_hz;
    config.health_rate_hz = options.health_rate_hz;

    bridge::Link link{std::move(config)};
    if (!link.open()) {
        std::fprintf(
            stderr, "bridge_link_test: open %s failed: %s\n", options.socket_path.c_str(),
            format_error(link.last_error()).c_str());
        return 1;
    }
    std::printf("bridge_link_test: listening on %s\n", options.socket_path.c_str());

    const auto start = link::monotonic_now_ns();
    const auto duration_ns = static_cast<std::int64_t>(options.duration_s * 1e9);
    auto now = start;
    std::uint64_t down_frames = 0;
    std::uint64_t control_updates = 0;

    while (now - start < duration_ns) {
        link.poll_recv(now);

        bridge::CustomControlData up{};
        up.size = static_cast<std::uint16_t>(bridge::kCustomControlMaxBytes);
        for (std::size_t i = 0; i < bridge::kCustomControlMaxBytes; ++i) {
            up.data[i] = static_cast<std::byte>((control_updates + i) & 0xFFU);
        }
        ++control_updates;
        link.poll_send(now, &up);

        bridge::DownPacket down;
        if (link.take_down(down)) {
            ++down_frames;
            std::printf(
                "bridge_link_test: down seq=%llu size=%u t_recv=%lld\n",
                static_cast<unsigned long long>(down.seq), static_cast<unsigned>(down.data.size),
                static_cast<long long>(down.t_recv_ns));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        now = link::monotonic_now_ns();
    }

    link.close();
    const auto& stats = link.stats();
    std::printf(
        "bridge_link_test: summary accepts=%llu replaces=%llu peer_reject=%llu "
        "handshakes=%llu handshake_timeouts=%llu version_mismatch=%llu "
        "rx_down=%llu down_bad=%llu tx_up=%llu up_drop=%llu health=%llu health_drop=%llu "
        "peer_uid=%d last_error=%s\n",
        static_cast<unsigned long long>(stats.accepts),
        static_cast<unsigned long long>(stats.replaces),
        static_cast<unsigned long long>(stats.peer_uid_rejected),
        static_cast<unsigned long long>(stats.handshakes),
        static_cast<unsigned long long>(stats.handshake_timeouts),
        static_cast<unsigned long long>(stats.version_mismatches),
        static_cast<unsigned long long>(stats.rx_down_frames),
        static_cast<unsigned long long>(stats.rx_down_bad),
        static_cast<unsigned long long>(stats.tx_up_frames),
        static_cast<unsigned long long>(stats.tx_up_dropped),
        static_cast<unsigned long long>(stats.tx_health_frames),
        static_cast<unsigned long long>(stats.tx_health_dropped), static_cast<int>(stats.peer_uid),
        format_error(link.last_error()).c_str());
    (void)down_frames;
    return 0;
}
