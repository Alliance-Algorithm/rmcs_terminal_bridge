#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unistd.h>
#include <utility>

#include <gtest/gtest.h>

#include "rmcs_terminal_bridge/link.hpp"
#include "teleop_ipc/control_link.hpp"
#include "teleop_ipc/seqpacket.hpp"
#include "teleop_ipc/session.hpp"

namespace {

namespace bridge = rmcs_terminal_bridge;
namespace link = teleop::link;

std::int64_t g_now = 1'000'000'000;

std::int64_t fake_now() noexcept { return g_now; }

class TestClient {
public:
    bool connect(const std::string& path) {
        auto address = link::UnixAddress::path(path);
        if (!address) {
            return false;
        }
        auto socket = link::Socket::connect(*address);
        if (!socket) {
            return false;
        }
        socket_.emplace(std::move(*socket));

        link::SessionOptions options{};
        options.role = link::Role::Connector;
        options.session_id = 0x1234;
        options.max_body_bytes = static_cast<std::uint32_t>(link::kControlMaxBodyBytes);
        options.now_ns = &fake_now;
        session_.emplace(std::move(*socket_), options);
        session_->begin_handshake();
        return true;
    }

    bool send_hello(std::uint32_t proto_version = link::kControlProtoVersion) {
        link::Hello hello{};
        hello.proto_version = proto_version;
        hello.capabilities = link::kCapUpData | link::kCapDownData | link::kCapHealth;
        hello.client_version = "test-client";

        std::array<std::byte, 128> body{};
        auto body_size = link::encode_hello(hello, body);
        if (!body_size) {
            return false;
        }
        std::array<std::byte, link::kHeaderBytes + 128> frame{};
        auto frame_size = session_->prepare(
            link::kMsgHello, std::span<const std::byte>{body.data(), *body_size}, frame);
        if (!frame_size) {
            return false;
        }
        return session_->send_prepared(std::span<const std::byte>{frame.data(), *frame_size})
            .has_value();
    }

    bool send_down(std::span<const std::byte> payload) {
        link::DownData data{};
        data.channel = link::kChannelCustomByteBlock;
        data.t_recv_ns = g_now;
        data.payload = payload;

        std::array<std::byte, 512> body{};
        auto body_size = link::encode_down_data(data, body);
        if (!body_size) {
            return false;
        }
        std::array<std::byte, link::kHeaderBytes + 512> frame{};
        auto frame_size = session_->prepare(
            link::kMsgDownData, std::span<const std::byte>{body.data(), *body_size}, frame);
        if (!frame_size) {
            return false;
        }
        return session_->send_prepared(std::span<const std::byte>{frame.data(), *frame_size})
            .has_value();
    }

    link::Result<link::FrameView> recv() { return session_->recv(rx_); }

    link::Session& session() { return *session_; }

private:
    std::optional<link::Socket> socket_;
    std::optional<link::Session> session_;
    std::array<std::byte, link::kHeaderBytes + 512> rx_{};
};

class LinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_now = 1'000'000'000;
        path_ = (std::filesystem::temp_directory_path()
                 / ("rmcs_terminal_bridge_test_" + std::to_string(::getpid()) + ".sock"))
                    .string();
        ::unlink(path_.c_str());
    }

    void TearDown() override { ::unlink(path_.c_str()); }

    bridge::LinkConfig config(std::size_t sndbuf = 4096, std::size_t rcvbuf = 4096) {
        bridge::LinkConfig config;
        config.socket_path = path_;
        config.socket_mode = 0600;
        config.allowed_uid = -1;
        config.send_rate_hz = 75.0;
        config.health_rate_hz = 1.0;
        config.handshake_timeout_ns = 2'000'000'000;
        config.sndbuf_bytes = sndbuf;
        config.rcvbuf_bytes = rcvbuf;
        config.now_ns = &fake_now;
        return config;
    }

    std::string path_;
};

void handshake(bridge::Link& link, TestClient& client) {
    ASSERT_TRUE(client.send_hello());
    link.poll_recv(g_now);
    link.poll_send(g_now, nullptr);

    auto ack_frame = client.recv();
    ASSERT_TRUE(ack_frame);
    ASSERT_EQ(ack_frame->header.msg_type, link::kMsgHelloAck);
    auto ack = link::decode_hello_ack(ack_frame->body);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->proto_version, link::kControlProtoVersion);
    EXPECT_EQ(std::string(ack->protocol_rev), "2027-V1.0.0");

    client.session().activate();
    EXPECT_TRUE(link.connected());
    EXPECT_EQ(link.stats().handshakes, 1ULL);
}

TEST_F(LinkTest, Handshake) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    handshake(link, client);

    EXPECT_EQ(link.stats().accepts, 1ULL);
    EXPECT_EQ(link.peer_proto_version(), link::kControlProtoVersion);
    EXPECT_EQ(std::string(link.client_version()), "test-client");
    EXPECT_EQ(link.stats().peer_uid, static_cast<std::int32_t>(::geteuid()));
}

TEST_F(LinkTest, UplinkRateAndHealth) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    handshake(link, client);

    bridge::CustomControlData up{};
    up.size = 4;
    up.data[0] = std::byte{0xAA};
    up.data[1] = std::byte{0xBB};
    up.data[2] = std::byte{0xCC};
    up.data[3] = std::byte{0xDD};

    std::uint64_t up_count = 0;
    std::uint64_t health_count = 0;

    for (int i = 0; i < 1000; ++i) {
        g_now += 1'000'000;
        link.poll_send(g_now, &up);

        while (true) {
            auto frame = client.recv();
            if (!frame) {
                break;
            }
            if (frame->header.msg_type == link::kMsgUpData) {
                auto data = link::decode_up_data(frame->body);
                ASSERT_TRUE(data);
                EXPECT_EQ(data->channel, link::kChannelCustomControl);
                ASSERT_EQ(data->payload.size(), 6U);
                EXPECT_EQ(data->payload[0], std::byte{0x0A});
                EXPECT_EQ(data->payload[1], std::byte{0x04});
                EXPECT_EQ(data->payload[2], std::byte{0xAA});
                ++up_count;
            } else if (frame->header.msg_type == link::kMsgHealth) {
                auto health = link::decode_health(frame->body);
                ASSERT_TRUE(health);
                EXPECT_NE(health->state & 1U, 0U);
                ++health_count;
            } else {
                FAIL() << "unexpected msg_type " << static_cast<int>(frame->header.msg_type);
            }
        }
    }

    EXPECT_GE(up_count, 70U);
    EXPECT_LE(up_count, 80U);
    EXPECT_GE(health_count, 1U);
    EXPECT_EQ(link.stats().tx_up_frames, up_count);
    EXPECT_EQ(link.stats().tx_up_dropped, 0ULL);
}

TEST_F(LinkTest, DownlinkIsKeepLatest) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    handshake(link, client);

    const std::array<std::byte, 4> payload{
        std::byte{0x0A}, std::byte{0x02}, std::byte{0xAA}, std::byte{0xBB}};
    ASSERT_TRUE(client.send_down(payload));
    link.poll_recv(g_now);

    bridge::DownPacket packet;
    ASSERT_TRUE(link.take_down(packet));
    EXPECT_EQ(packet.data.size, 2U);
    EXPECT_EQ(packet.data.data[0], std::byte{0xAA});
    EXPECT_EQ(packet.data.data[1], std::byte{0xBB});
    EXPECT_GT(packet.seq, 0U);
    EXPECT_EQ(packet.t_recv_ns, g_now);
    EXPECT_EQ(link.stats().rx_down_frames, 1ULL);
    EXPECT_FALSE(link.take_down(packet));
}

TEST_F(LinkTest, VersionMismatchAcksThenCloses) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    ASSERT_TRUE(client.send_hello(2));

    link.poll_recv(g_now);
    link.poll_send(g_now, nullptr);

    auto ack_frame = client.recv();
    ASSERT_TRUE(ack_frame);
    auto ack = link::decode_hello_ack(ack_frame->body);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->proto_version, link::kControlProtoVersion);

    auto closed = client.recv();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, link::ErrorCode::Closed);
    EXPECT_FALSE(link.connected());
    EXPECT_EQ(link.stats().version_mismatches, 1ULL);
}

TEST_F(LinkTest, HandshakeTimeout) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    link.poll_recv(g_now);

    g_now += 2'000'000'001;
    link.poll_recv(g_now);

    EXPECT_FALSE(link.connected());
    EXPECT_EQ(link.stats().handshake_timeouts, 1ULL);

    auto closed = client.recv();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, link::ErrorCode::Closed);
}

TEST_F(LinkTest, UpEagainDropsAndCounts) {
    bridge::Link link{config(512, 512)};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    handshake(link, client);

    bridge::CustomControlData up{};
    up.size = 60;
    up.data.fill(std::byte{0x5A});

    for (int i = 0; i < 200; ++i) {
        g_now += 1'000'000;
        link.poll_send(g_now, &up);
    }

    EXPECT_GT(link.stats().tx_up_dropped + link.stats().tx_health_dropped, 0ULL);
}

TEST_F(LinkTest, NewConnectionReplacesOld) {
    bridge::Link link{config()};
    ASSERT_TRUE(link.open());

    TestClient first;
    ASSERT_TRUE(first.connect(path_));
    handshake(link, first);

    TestClient second;
    ASSERT_TRUE(second.connect(path_));
    ASSERT_TRUE(second.send_hello());
    link.poll_recv(g_now);

    EXPECT_EQ(link.stats().accepts, 2ULL);
    EXPECT_EQ(link.stats().replaces, 1ULL);

    link.poll_send(g_now, nullptr);
    auto ack = second.recv();
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->header.msg_type, link::kMsgHelloAck);

    // 旧连接上可能还有握手时排队的 HEALTH，先 drain 到关闭。
    while (true) {
        auto frame = first.recv();
        if (!frame) {
            EXPECT_EQ(frame.error().code, link::ErrorCode::Closed);
            break;
        }
    }
}

TEST_F(LinkTest, PeerUidMismatchIsRejected) {
    auto cfg = config();
    cfg.allowed_uid = static_cast<int>(::geteuid()) + 1;
    bridge::Link link{cfg};
    ASSERT_TRUE(link.open());

    TestClient client;
    ASSERT_TRUE(client.connect(path_));
    link.poll_recv(g_now);

    EXPECT_FALSE(link.connected());
    EXPECT_EQ(link.stats().peer_uid_rejected, 1ULL);

    auto closed = client.recv();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().code, link::ErrorCode::Closed);
}

} // namespace
