#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "teleop_ipc/error.hpp"
#include "teleop_ipc/wire.hpp"

namespace teleop::link {

inline constexpr std::uint8_t kMsgHello = 1;
inline constexpr std::uint8_t kMsgHelloAck = 2;
inline constexpr std::uint8_t kMsgUpData = 3;
inline constexpr std::uint8_t kMsgDownData = 4;
inline constexpr std::uint8_t kMsgHealth = 6;

inline constexpr std::uint32_t kChannelCustomControl = 1;
inline constexpr std::uint32_t kChannelCustomByteBlock = 2;

inline constexpr std::uint32_t kControlProtoVersion = 1;
inline constexpr std::uint64_t kCapUpData = 1U << 0U;
inline constexpr std::uint64_t kCapDownData = 1U << 1U;
inline constexpr std::uint64_t kCapHealth = 1U << 2U;

// 控制链路 session 的 body 上限；含官方 envelope 的余量。
inline constexpr std::size_t kControlMaxBodyBytes = 512;

struct Hello {
    std::uint32_t proto_version = 0;
    std::uint64_t capabilities = 0;
    std::string_view client_version{};
};

struct HelloAck {
    std::uint32_t proto_version = 0;
    std::uint64_t capabilities = 0;
    std::string_view protocol_rev{};
};

struct UpData {
    std::uint32_t channel = 0;
    std::int64_t t_src_ns = 0;
    std::span<const std::byte> payload{};
};

struct DownData {
    std::uint32_t channel = 0;
    std::int64_t t_recv_ns = 0;
    std::span<const std::byte> payload{};
};

struct Health {
    std::uint32_t state = 0;
    std::uint32_t load_pct = 0;
};

namespace detail {

inline Result<void> write_uint(wire::Writer& writer, std::uint32_t field, std::uint64_t value) {
    if (value == 0) {
        return {};
    }
    if (auto result = writer.tag(field, wire::Type::Varint); !result) {
        return result;
    }
    return writer.varint(value);
}

inline Result<void> write_int64(wire::Writer& writer, std::uint32_t field, std::int64_t value) {
    return write_uint(writer, field, std::bit_cast<std::uint64_t>(value));
}

inline Result<void>
    write_bytes(wire::Writer& writer, std::uint32_t field, std::span<const std::byte> value) {
    if (value.empty()) {
        return {};
    }
    return writer.bytes(field, value);
}

inline Result<void>
    write_string(wire::Writer& writer, std::uint32_t field, std::string_view value) {
    if (value.empty()) {
        return {};
    }
    return writer.string(field, value);
}

inline Result<std::uint64_t> read_uint(wire::Reader& reader) { return reader.varint(); }

inline Result<std::int64_t> read_int64(wire::Reader& reader) {
    auto value = reader.varint();
    if (!value) {
        return tl::make_unexpected(value.error());
    }
    return std::bit_cast<std::int64_t>(*value);
}

inline Result<std::span<const std::byte>> read_bytes(wire::Reader& reader) {
    return reader.bytes();
}

inline Result<void> skip_unknown(wire::Reader& reader, wire::Type type) {
    return reader.skip(type);
}

} // namespace detail

inline Result<std::size_t> encode_hello(const Hello& msg, std::span<std::byte> out) {
    wire::Writer writer{out};
    if (auto result = detail::write_uint(writer, 1, msg.proto_version); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_uint(writer, 2, msg.capabilities); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_string(writer, 3, msg.client_version); !result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

inline Result<Hello> decode_hello(std::span<const std::byte> in) {
    wire::Reader reader{in};
    Hello msg;
    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;
        if (field == 1 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.proto_version = static_cast<std::uint32_t>(*value);
        } else if (field == 2 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.capabilities = *value;
        } else if (field == 3 && type == wire::Type::LengthDelimited) {
            auto value = detail::read_bytes(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.client_version = wire::as_string_view(*value);
        } else {
            if (auto result = reader.skip(type); !result) {
                return tl::make_unexpected(result.error());
            }
        }
    }
    return msg;
}

inline Result<std::size_t> encode_hello_ack(const HelloAck& msg, std::span<std::byte> out) {
    wire::Writer writer{out};
    if (auto result = detail::write_uint(writer, 1, msg.proto_version); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_uint(writer, 2, msg.capabilities); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_string(writer, 3, msg.protocol_rev); !result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

inline Result<HelloAck> decode_hello_ack(std::span<const std::byte> in) {
    wire::Reader reader{in};
    HelloAck msg;
    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;
        if (field == 1 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.proto_version = static_cast<std::uint32_t>(*value);
        } else if (field == 2 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.capabilities = *value;
        } else if (field == 3 && type == wire::Type::LengthDelimited) {
            auto value = detail::read_bytes(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.protocol_rev = wire::as_string_view(*value);
        } else {
            if (auto result = reader.skip(type); !result) {
                return tl::make_unexpected(result.error());
            }
        }
    }
    return msg;
}

inline Result<std::size_t> encode_up_data(const UpData& msg, std::span<std::byte> out) {
    wire::Writer writer{out};
    if (auto result = detail::write_uint(writer, 1, msg.channel); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_int64(writer, 2, msg.t_src_ns); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_bytes(writer, 3, msg.payload); !result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

inline Result<UpData> decode_up_data(std::span<const std::byte> in) {
    wire::Reader reader{in};
    UpData msg;
    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;
        if (field == 1 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.channel = static_cast<std::uint32_t>(*value);
        } else if (field == 2 && type == wire::Type::Varint) {
            auto value = detail::read_int64(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.t_src_ns = *value;
        } else if (field == 3 && type == wire::Type::LengthDelimited) {
            auto value = detail::read_bytes(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.payload = *value;
        } else {
            if (auto result = reader.skip(type); !result) {
                return tl::make_unexpected(result.error());
            }
        }
    }
    return msg;
}

inline Result<std::size_t> encode_down_data(const DownData& msg, std::span<std::byte> out) {
    wire::Writer writer{out};
    if (auto result = detail::write_uint(writer, 1, msg.channel); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_int64(writer, 2, msg.t_recv_ns); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_bytes(writer, 3, msg.payload); !result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

inline Result<DownData> decode_down_data(std::span<const std::byte> in) {
    wire::Reader reader{in};
    DownData msg;
    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;
        if (field == 1 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.channel = static_cast<std::uint32_t>(*value);
        } else if (field == 2 && type == wire::Type::Varint) {
            auto value = detail::read_int64(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.t_recv_ns = *value;
        } else if (field == 3 && type == wire::Type::LengthDelimited) {
            auto value = detail::read_bytes(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.payload = *value;
        } else {
            if (auto result = reader.skip(type); !result) {
                return tl::make_unexpected(result.error());
            }
        }
    }
    return msg;
}

inline Result<std::size_t> encode_health(const Health& msg, std::span<std::byte> out) {
    wire::Writer writer{out};
    if (auto result = detail::write_uint(writer, 1, msg.state); !result) {
        return tl::make_unexpected(result.error());
    }
    if (auto result = detail::write_uint(writer, 2, msg.load_pct); !result) {
        return tl::make_unexpected(result.error());
    }
    return writer.size();
}

inline Result<Health> decode_health(std::span<const std::byte> in) {
    wire::Reader reader{in};
    Health msg;
    while (!reader.done()) {
        auto tag = reader.tag();
        if (!tag) {
            return tl::make_unexpected(tag.error());
        }
        const auto [field, type] = *tag;
        if (field == 1 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.state = static_cast<std::uint32_t>(*value);
        } else if (field == 2 && type == wire::Type::Varint) {
            auto value = detail::read_uint(reader);
            if (!value) {
                return tl::make_unexpected(value.error());
            }
            msg.load_pct = static_cast<std::uint32_t>(*value);
        } else {
            if (auto result = reader.skip(type); !result) {
                return tl::make_unexpected(result.error());
            }
        }
    }
    return msg;
}

} // namespace teleop::link
