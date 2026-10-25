#pragma once
#include <cstdint>
#include <charconv>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace clashflux::native_control {
// Bounded unary gRPC-Web codec for the pinned StartedService schema. No
// compression, reflection or arbitrary protobuf object construction is needed.
inline std::optional<std::string> UnaryMessage(std::string_view body, int* grpcStatus = nullptr) {
    if (grpcStatus) *grpcStatus = -1;
    if (body.size() > 65536) return {};
    std::optional<std::string> message;
    bool trailers = false;
    int status = -1;
    while (!body.empty()) {
        if (body.size() < 5 || trailers) return {};
        const auto flag = static_cast<unsigned char>(body[0]);
        std::uint32_t size = 0;
        for (int i = 1; i <= 4; ++i) size = (size << 8) | static_cast<unsigned char>(body[i]);
        body.remove_prefix(5);
        if (size > body.size()) return {};
        const auto frame = body.substr(0, size);
        body.remove_prefix(size);
        if (flag == 0) {
            if (message) return {};
            message = std::string(frame);
        } else if (flag == 128) {
            trailers = true;
            auto lines = frame;
            bool sawStatus = false;
            while (!lines.empty()) {
                const auto end = lines.find("\r\n");
                auto line = lines.substr(0, end);
                if (line.starts_with("grpc-status:")) {
                    if (sawStatus) return {};
                    sawStatus = true;
                    line.remove_prefix(12);
                    while (!line.empty() && line.front() == ' ') line.remove_prefix(1);
                    const auto parsed = std::from_chars(line.data(), line.data() + line.size(), status);
                    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() ||
                        status < 0 || status > 16) return {};
                }
                if (end == std::string_view::npos) break;
                lines.remove_prefix(end + 2);
            }
        } else return {};
    }
    if (!trailers || status < 0) return {};
    if (grpcStatus) *grpcStatus = status;
    return status == 0 ? message : std::nullopt;
}

inline bool Varint(std::string_view& bytes, std::uint64_t& value) {
    value = 0;
    for (unsigned i = 0; i < 10 && !bytes.empty(); ++i) {
        const auto byte = static_cast<unsigned char>(bytes.front());
        bytes.remove_prefix(1);
        if (i == 9 && byte > 1) return false;
        value |= static_cast<std::uint64_t>(byte & 127) << (i * 7);
        if (!(byte & 128)) return true;
    }
    return false;
}

struct Version { std::string version; std::uint64_t api = 0; };
inline std::optional<Version> DecodeVersion(std::string_view bytes) {
    Version result;
    bool version = false, api = false;
    while (!bytes.empty()) {
        std::uint64_t key = 0, value = 0;
        if (!Varint(bytes, key) || key == 0) return {};
        if (key == 10 && !version) {
            if (!Varint(bytes, value) || value > bytes.size()) return {};
            result.version = bytes.substr(0, value);
            bytes.remove_prefix(static_cast<std::size_t>(value));
            version = true;
        } else if (key == 16 && !api) {
            if (!Varint(bytes, result.api)) return {};
            api = true;
        } else return {};
    }
    if (!version || result.version.empty() || !api) return {};
    return result;
}
inline std::optional<std::uint64_t> DecodeStartedAt(std::string_view bytes) {
    std::uint64_t key = 0, value = 0;
    if (!Varint(bytes, key) || key != 8 || !Varint(bytes, value) || !bytes.empty() || !value ||
        value > (std::numeric_limits<std::int64_t>::max)()) return {};
    return value;
}
} // namespace clashflux::native_control
