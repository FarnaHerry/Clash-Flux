// qrs_codec.cpp — see qrs_codec.h. The frame layout and LT parameters mirror
// qifi-dev/qrs so a standard QRS scanner can consume these frames.
#include "qrs_codec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(CLASHFLUX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace clashflux::ui::qrs {
namespace {

constexpr std::size_t kMaxFrameBytes = 8192;
constexpr std::uint32_t kMaxSourceBlocks = 16384;
constexpr std::uint32_t kMaxBlockBytes = 4096;
constexpr std::size_t kMaxInflatedBytes = 64U * 1024U * 1024U;
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string EncodeBase64(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        out.push_back(kBase64Alphabet[(value >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(value >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size()
                          ? kBase64Alphabet[(value >> 6) & 0x3F]
                          : '=');
        out.push_back(i + 2 < bytes.size()
                          ? kBase64Alphabet[value & 0x3F]
                          : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> DecodeBase64(std::string_view text) {
    if (text.empty() || text.size() > kMaxFrameBytes || text.size() % 4 != 0) {
        return std::nullopt;
    }
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<std::uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int a = valueOf(text[i]);
        const int b = valueOf(text[i + 1]);
        const char cChar = text[i + 2];
        const char dChar = text[i + 3];
        const int c = cChar == '=' ? 0 : valueOf(cChar);
        const int d = dChar == '=' ? 0 : valueOf(dChar);
        if (a < 0 || b < 0 || c < 0 || d < 0) return std::nullopt;
        if (cChar == '=' && dChar != '=') return std::nullopt;
        if (!last && (cChar == '=' || dChar == '=')) return std::nullopt;

        const std::uint32_t value =
            (static_cast<std::uint32_t>(a) << 18) |
            (static_cast<std::uint32_t>(b) << 12) |
            (static_cast<std::uint32_t>(c) << 6) |
            static_cast<std::uint32_t>(d);
        out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        if (cChar != '=') {
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        }
        if (dChar != '=') {
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
        }
    }
    return out;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

std::uint32_t Checksum(std::string_view bytes, std::uint32_t blocks) {
    return Crc32(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                 bytes.size()) ^
           blocks;
}

std::vector<std::uint8_t> Deflate(const std::vector<std::uint8_t>& source) {
#if defined(CLASHFLUX_HAVE_ZLIB)
    uLongf bound = compressBound(static_cast<uLong>(source.size()));
    std::vector<std::uint8_t> compressed(bound);
    const int status = compress2(
        compressed.data(), &bound, source.data(),
        static_cast<uLong>(source.size()), Z_DEFAULT_COMPRESSION);
    if (status == Z_OK) {
        compressed.resize(bound);
        return compressed;
    }
#endif
    return source;
}

std::optional<std::string> Inflate(const std::vector<std::uint8_t>& source) {
#if defined(CLASHFLUX_HAVE_ZLIB)
    uLongf capacity = std::max<uLongf>(
        1024, static_cast<uLongf>(source.size()) * 4);
    while (capacity <= kMaxInflatedBytes) {
        std::vector<std::uint8_t> output(capacity);
        uLongf size = capacity;
        const int status = uncompress(output.data(), &size, source.data(),
                                      static_cast<uLong>(source.size()));
        if (status == Z_OK) {
            output.resize(size);
            return std::string(reinterpret_cast<const char*>(output.data()),
                               output.size());
        }
        if (status != Z_BUF_ERROR) return std::nullopt;
        if (capacity >= kMaxInflatedBytes) return std::nullopt;
        capacity = std::min<uLongf>(capacity * 2, kMaxInflatedBytes);
    }
#else
    static_cast<void>(source);
#endif
    return std::nullopt;
}

// Robust Soliton distribution, truncated at maxDegree and folded into the
// last bucket. This is the distribution used by qifi-dev/qrs.
std::vector<double> DegreeWeights(std::size_t blocks, int maxDegree) {
    if (blocks == 0) return {};
    const std::size_t cap = std::min<std::size_t>(
        blocks, maxDegree >= 1 ? static_cast<std::size_t>(maxDegree) : 1);
    std::vector<double> weights(cap, 0.0);
    if (blocks == 1) {
        weights[0] = 1.0;
        return weights;
    }

    constexpr double c = 0.2;
    constexpr double delta = 0.5;
    const double k = static_cast<double>(blocks);
    const double R = c * std::log(k / delta) * std::sqrt(k);
    const double spike = R > 0 ? std::max(1.0, std::floor(k / R)) : 0.0;
    for (std::size_t degree = 1; degree <= blocks; ++degree) {
        const double d = static_cast<double>(degree);
        double probability = degree == 1 ? 1.0 / k : 1.0 / (d * (d - 1.0));
        if (R > 0 && spike > 0) {
            if (d < spike) {
                probability += R / (d * k);
            } else if (d == spike) {
                probability += std::max(0.0, R * std::log(R / delta) / k);
            }
        }
        if (degree <= cap) {
            weights[degree - 1] += probability;
        } else {
            weights[cap - 1] += probability;
        }
    }

    double total = 0;
    for (double weight : weights) total += weight;
    if (total <= 0) return weights;
    for (double& weight : weights) weight /= total;
    return weights;
}

std::vector<std::uint32_t> SampleIndices(std::uint32_t blocks,
                                         std::uint32_t degree,
                                         std::mt19937& random) {
    if (blocks == 0) return {};
    const std::uint32_t count = std::clamp(degree, 1U, blocks);
    if (count * 2 > blocks) {
        std::vector<std::uint32_t> all(blocks);
        for (std::uint32_t i = 0; i < blocks; ++i) all[i] = i;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uniform_int_distribution<std::uint32_t> pick(i, blocks - 1);
            std::swap(all[i], all[pick(random)]);
        }
        all.resize(count);
        std::ranges::sort(all);
        return all;
    }

    std::unordered_set<std::uint32_t> selected;
    selected.reserve(count);
    std::uniform_int_distribution<std::uint32_t> pick(0, blocks - 1);
    while (selected.size() < count) selected.insert(pick(random));
    std::vector<std::uint32_t> indices(selected.begin(), selected.end());
    std::ranges::sort(indices);
    return indices;
}

struct XorRow {
    std::vector<std::uint64_t> bits;
    std::vector<std::uint8_t> data;
};

void XorInto(XorRow& target, const XorRow& source) {
    for (std::size_t i = 0; i < target.bits.size(); ++i) {
        target.bits[i] ^= source.bits[i];
    }
    for (std::size_t i = 0; i < target.data.size(); ++i) {
        target.data[i] ^= source.data[i];
    }
}

bool BitAt(const std::vector<std::uint64_t>& bits, std::size_t index) {
    return (bits[index / 64] >> (index % 64)) & 1U;
}

} // namespace

Sequence Encode(std::string_view payload, EncodeOptions options) {
    Sequence sequence;
    if (payload.empty()) return sequence;

    const int sliceSize = std::clamp(options.slice_size, 64, 1024);
    const int maxDegree = std::clamp(options.max_degree, 1, 64);
    std::vector<std::uint8_t> source(payload.begin(), payload.end());
    const std::vector<std::uint8_t> data =
        options.compress ? Deflate(source) : source;
    if (data.empty()) return sequence;

    const std::size_t blocks =
        (data.size() + static_cast<std::size_t>(sliceSize) - 1) /
        static_cast<std::size_t>(sliceSize);
    if (blocks == 0 || blocks > kMaxSourceBlocks) return sequence;

    const std::uint32_t checksum =
        Crc32(source.data(), source.size()) ^ static_cast<std::uint32_t>(blocks);
    std::vector<std::uint8_t> padded(
        blocks * static_cast<std::size_t>(sliceSize), 0);
    std::copy(data.begin(), data.end(), padded.begin());
    std::mt19937 random(options.seed != 0
                            ? static_cast<std::uint32_t>(options.seed)
                            : 0x9E3779B9U);
    const std::vector<double> weights =
        DegreeWeights(blocks, maxDegree);
    std::discrete_distribution<std::size_t> degreeDistribution(
        weights.begin(), weights.end());

    std::size_t frameCount = options.frame_count > 0
                                 ? static_cast<std::size_t>(options.frame_count)
                                 : blocks + std::max<std::size_t>(24, blocks / 3);
    frameCount = std::min<std::size_t>(frameCount, kMaxSourceBlocks);
    if (frameCount < blocks) return sequence;
    sequence.frames.reserve(frameCount);
    sequence.block_count = static_cast<int>(blocks);
    sequence.block_bytes = sliceSize;

    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        // Start with the systematic degree-1 blocks. Besides making the
        // first k frames immediately useful, this keeps the common qrs/QiFi
        // decoder from completing on a rank-deficient high-degree prefix.
        const std::uint32_t degree = frame < blocks
                                          ? 1
                                          : static_cast<std::uint32_t>(
                                                degreeDistribution(random) + 1);
        const std::vector<std::uint32_t> indices =
            frame < blocks
                ? std::vector<std::uint32_t>{static_cast<std::uint32_t>(frame)}
                : SampleIndices(static_cast<std::uint32_t>(blocks), degree,
                                random);

        std::vector<std::uint8_t> block(static_cast<std::size_t>(sliceSize), 0);
        for (std::uint32_t index : indices) {
            const std::size_t offset =
                static_cast<std::size_t>(index) * sliceSize;
            for (int byte = 0; byte < sliceSize; ++byte) {
                block[byte] ^= padded[offset + byte];
            }
        }

        std::vector<std::uint8_t> binary;
        binary.reserve(4 * (indices.size() + 4) + block.size());
        AppendU32(binary, static_cast<std::uint32_t>(indices.size()));
        for (std::uint32_t index : indices) AppendU32(binary, index);
        AppendU32(binary, static_cast<std::uint32_t>(blocks));
        AppendU32(binary, static_cast<std::uint32_t>(data.size()));
        AppendU32(binary, checksum);
        binary.insert(binary.end(), block.begin(), block.end());
        sequence.frames.push_back(EncodeBase64(binary));
    }
    return sequence;
}

struct Decoder::Impl {
    struct Meta {
        std::uint32_t blocks = 0;
        std::uint32_t bytes = 0;
        std::uint32_t checksum = 0;
        std::size_t block_size = 0;
    };

    std::optional<Meta> meta;
    std::map<std::uint32_t, XorRow> rows;
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

void Decoder::Reset() {
    impl_ = std::make_unique<Impl>();
}

int Decoder::DecodedBlocks() const {
    return static_cast<int>(impl_->rows.size());
}

int Decoder::TotalBlocks() const {
    return impl_->meta ? static_cast<int>(impl_->meta->blocks) : 0;
}

DecodeResult Decoder::Add(std::string_view frame, std::string& payload,
                          std::string& error) {
    payload.clear();
    error.clear();
    if (frame.size() > kMaxFrameBytes) return DecodeResult::NotQrs;
    const auto binary = DecodeBase64(frame);
    if (!binary || binary->size() < 20) return DecodeResult::NotQrs;

    const std::uint32_t degree = ReadU32(*binary, 0);
    if (degree == 0 || degree > 4096) return DecodeResult::NotQrs;
    const std::size_t headerBytes =
        4 * (static_cast<std::size_t>(degree) + 4);
    if (binary->size() <= headerBytes) return DecodeResult::NotQrs;

    const std::uint32_t blocks =
        ReadU32(*binary, 4 * (static_cast<std::size_t>(degree) + 1));
    const std::uint32_t bytes =
        ReadU32(*binary, 4 * (static_cast<std::size_t>(degree) + 2));
    const std::uint32_t checksum =
        ReadU32(*binary, 4 * (static_cast<std::size_t>(degree) + 3));
    const std::size_t blockSize = binary->size() - headerBytes;
    if (blocks == 0 || blocks > kMaxSourceBlocks ||
        blockSize == 0 || blockSize > kMaxBlockBytes ||
        bytes == 0 ||
        static_cast<std::uint64_t>(bytes) >
            static_cast<std::uint64_t>(blocks) * blockSize) {
        return DecodeResult::NotQrs;
    }

    XorRow row;
    row.bits.assign((blocks + 63) / 64, 0);
    row.data.assign(binary->begin() + static_cast<std::ptrdiff_t>(headerBytes),
                    binary->end());
    for (std::uint32_t i = 0; i < degree; ++i) {
        const std::uint32_t index = ReadU32(*binary, 4 * (i + 1));
        if (index >= blocks) return DecodeResult::NotQrs;
        row.bits[index / 64] |= 1ULL << (index % 64);
    }

    if (!impl_->meta) {
        impl_->meta = Impl::Meta{blocks, bytes, checksum, blockSize};
    } else if (impl_->meta->blocks != blocks ||
               impl_->meta->bytes != bytes ||
               impl_->meta->checksum != checksum ||
               impl_->meta->block_size != blockSize) {
        // A different share stream started. A QR scan should recover rather
        // than keep stale rows from the previous sequence.
        impl_ = std::make_unique<Impl>();
        impl_->meta = Impl::Meta{blocks, bytes, checksum, blockSize};
    }

    // Reduce against every existing pivot. Rows are kept in reduced row-echelon
    // form, so a successful reduction leaves either zero or one new pivot.
    for (const auto& [pivot, existing] : impl_->rows) {
        if (BitAt(row.bits, pivot)) XorInto(row, existing);
    }
    std::size_t pivot = 0;
    while (pivot < blocks && !BitAt(row.bits, pivot)) ++pivot;
    if (pivot == blocks) return DecodeResult::NeedMore;

    // Keep the system fully reduced: the new pivot must not appear in older
    // rows, otherwise individual source blocks are not directly recoverable.
    for (auto& [otherPivot, existing] : impl_->rows) {
        if (otherPivot != pivot && BitAt(existing.bits, pivot)) {
            XorInto(existing, row);
        }
    }
    impl_->rows.emplace(static_cast<std::uint32_t>(pivot), std::move(row));

    if (impl_->rows.size() < blocks) return DecodeResult::NeedMore;

    const Impl::Meta& meta = *impl_->meta;
    std::vector<std::uint8_t> merged(meta.bytes, 0);
    for (std::uint32_t i = 0; i < meta.blocks; ++i) {
        const auto it = impl_->rows.find(i);
        if (it == impl_->rows.end()) return DecodeResult::NeedMore;
        const std::size_t offset = static_cast<std::size_t>(i) * meta.block_size;
        const std::size_t count =
            std::min(meta.block_size, merged.size() - offset);
        std::copy_n(it->second.data.begin(),
                    static_cast<std::ptrdiff_t>(count),
                    merged.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    if (const auto inflated = Inflate(merged)) {
        if (Checksum(*inflated, meta.blocks) == meta.checksum) {
            payload = *inflated;
            return DecodeResult::Complete;
        }
    }
    const std::string_view raw(
        reinterpret_cast<const char*>(merged.data()), merged.size());
    if (Checksum(raw, meta.blocks) == meta.checksum) {
        payload.assign(raw);
        return DecodeResult::Complete;
    }
    error = "QRS 数据校验失败";
    return DecodeResult::Error;
}

} // namespace clashflux::ui::qrs
