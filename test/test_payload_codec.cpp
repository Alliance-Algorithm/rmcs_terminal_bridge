#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "rmcs_terminal_bridge/payload_codec.hpp"

namespace {

namespace bridge = rmcs_terminal_bridge;
namespace link = teleop::link;

std::vector<std::byte> make_bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

TEST(PayloadCodec, CustomControlGoldenBytes) {
    bridge::CustomControlData control{};
    control.size = 2;
    control.data[0] = std::byte{0xAA};
    control.data[1] = std::byte{0xBB};

    std::array<std::byte, 64> out{};
    auto size = bridge::encode_custom_control(control, out);
    ASSERT_TRUE(size);
    EXPECT_EQ(*size, 4U);
    const auto expected = make_bytes({0x0A, 0x02, 0xAA, 0xBB});
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out.begin()));
}

TEST(PayloadCodec, CustomControlEmptyIsPresentButEmpty) {
    bridge::CustomControlData control{};
    std::array<std::byte, 64> out{};
    auto size = bridge::encode_custom_control(control, out);
    ASSERT_TRUE(size);
    EXPECT_EQ(*size, 2U);
    const auto expected = make_bytes({0x0A, 0x00});
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), out.begin()));
}

TEST(PayloadCodec, CustomControlMaxAndOverflow) {
    bridge::CustomControlData control{};
    control.size = static_cast<std::uint16_t>(bridge::kCustomControlMaxBytes);
    control.data.fill(std::byte{0x5A});

    std::array<std::byte, 64> out{};
    auto size = bridge::encode_custom_control(control, out);
    ASSERT_TRUE(size);
    EXPECT_EQ(*size, bridge::kCustomControlMaxBytes + 2);

    control.size = static_cast<std::uint16_t>(bridge::kCustomControlMaxBytes + 1);
    auto overflow = bridge::encode_custom_control(control, out);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, link::ErrorCode::TooLarge);
}

TEST(PayloadCodec, DecodeCustomByteBlockGoldenBytes) {
    const auto input = make_bytes({0x0A, 0x02, 0xAA, 0xBB});
    auto block = bridge::decode_custom_byte_block(input);
    ASSERT_TRUE(block);
    EXPECT_EQ(block->size, 2U);
    EXPECT_EQ(block->data[0], std::byte{0xAA});
    EXPECT_EQ(block->data[1], std::byte{0xBB});
}

TEST(PayloadCodec, DecodeCustomByteBlockMissingFieldIsEmpty) {
    auto block = bridge::decode_custom_byte_block(std::span<const std::byte>{});
    ASSERT_TRUE(block);
    EXPECT_EQ(block->size, 0U);
}

TEST(PayloadCodec, DecodeCustomByteBlockUnknownFieldIsSkipped) {
    const auto input = make_bytes({0x10, 0x01, 0x0A, 0x02, 0xAA, 0xBB});
    auto block = bridge::decode_custom_byte_block(input);
    ASSERT_TRUE(block);
    EXPECT_EQ(block->size, 2U);
    EXPECT_EQ(block->data[1], std::byte{0xBB});
}

TEST(PayloadCodec, DecodeCustomByteBlockMaxAndOverflow) {
    std::vector<std::byte> input{std::byte{0x0A}, std::byte{0xAC}, std::byte{0x02}};
    input.resize(input.size() + bridge::kCustomByteBlockMaxBytes, std::byte{0x5A});

    auto block = bridge::decode_custom_byte_block(input);
    ASSERT_TRUE(block);
    EXPECT_EQ(block->size, bridge::kCustomByteBlockMaxBytes);

    std::vector<std::byte> overflow{std::byte{0x0A}, std::byte{0xAD}, std::byte{0x02}};
    overflow.resize(overflow.size() + bridge::kCustomByteBlockMaxBytes + 1, std::byte{0x5A});
    auto too_large = bridge::decode_custom_byte_block(overflow);
    ASSERT_FALSE(too_large);
    EXPECT_EQ(too_large.error().code, link::ErrorCode::TooLarge);
}

TEST(PayloadCodec, DecodeCustomByteBlockRejectsTruncated) {
    const auto input = make_bytes({0x0A, 0x04, 0xAA});
    auto block = bridge::decode_custom_byte_block(input);
    ASSERT_FALSE(block);
    EXPECT_EQ(block.error().code, link::ErrorCode::Truncated);
}

} // namespace
