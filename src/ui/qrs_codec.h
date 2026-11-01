// qrs_codec.h — QiFi/QRS-compatible multi-frame QR transport.
//
// The wire format follows github.com/qifi-dev/qrs: a Luby Transform stream over
// 500-byte source blocks, each encoded frame serialized as little-endian
// uint32 header + block data, then standard Base64. Scanning enough independent
// frames reconstructs the original bytes even when individual frames are lost.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace clashflux::ui::qrs {

struct Sequence {
    std::vector<std::string> frames;
    int block_count = 0;
    int block_bytes = 0;
};

struct EncodeOptions {
    int slice_size = 500;
    int max_degree = 32;
    int frame_count = 0;
    std::uint64_t seed = 0;
    bool compress = true;
};

// Returns an empty Sequence when the payload is empty or cannot be encoded.
Sequence Encode(std::string_view payload, EncodeOptions options = {});

enum class DecodeResult {
    NotQrs,
    NeedMore,
    Complete,
    Error,
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    DecodeResult Add(std::string_view frame, std::string& payload,
                     std::string& error);
    void Reset();

    int DecodedBlocks() const;
    int TotalBlocks() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace clashflux::ui::qrs
