// qrs_codec: encode frames, drop/duplicate/shuffle, recover the payload.
#include "qrs_codec.h"

#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace qrs = clashflux::ui::qrs;

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string Payload() {
    std::string payload;
    payload += "mixed-port: 7890\n";
    for (int i = 0; i < 120; ++i) {
        payload += "  - DOMAIN-SUFFIX,example-" + std::to_string(i) +
                   ".test,DIRECT\n";
        payload += "  - IP-CIDR,10." + std::to_string(i % 200) +
                   ".0.0/16,REJECT\n";
    }
    return payload;
}

void RoundTrip(bool compress) {
    const std::string payload = Payload();
    qrs::Sequence sequence = qrs::Encode(
        payload, qrs::EncodeOptions{.compress = compress});
    Check(!sequence.frames.empty(), "encoder produced frames");
    Check(sequence.block_count > 1, "payload spans multiple blocks");
    Check(sequence.frames.size() > 1, "payload spans multiple QR frames");

    std::vector<std::string> frames = sequence.frames;
    std::mt19937 random(42);
    std::shuffle(frames.begin(), frames.end(), random);
    frames.push_back(frames.front());

    qrs::Decoder decoder;
    std::string decoded;
    std::string error;
    bool complete = false;
    for (const std::string& frame : frames) {
        const qrs::DecodeResult result = decoder.Add(frame, decoded, error);
        Check(result != qrs::DecodeResult::Error, "valid frame decodes");
        if (result == qrs::DecodeResult::Complete) {
            complete = true;
            break;
        }
    }
    Check(complete, "shuffled frames reconstruct payload");
    Check(decoded == payload, "decoded payload matches");
    Check(decoder.DecodedBlocks() == decoder.TotalBlocks(),
          "all source blocks are decoded");
}

void OwnSharedProfiles() {
    // These are config bytes from the sharing path, not a URL or QRS frame text
    // to be downloaded after reconstruction.
    for (const std::string content : {
            std::string{"proxies: []\nproxy-groups: []\nrules:\n  - MATCH,DIRECT\n"},
            std::string{R"({"outbounds":[{"type":"direct","tag":"DIRECT"}],"route":{"final":"DIRECT"}})"}}) {
        for (bool compress : {true, false}) {
            const auto sequence = qrs::Encode(content, {.slice_size = 24, .compress = compress});
            qrs::Decoder decoder;
            std::string decoded, error;
            bool complete = false;
            for (const auto& frame : sequence.frames) {
                const auto result = decoder.Add(frame, decoded, error);
                Check(result != qrs::DecodeResult::NotQrs && result != qrs::DecodeResult::Error,
                      "our own shared configuration frames are recognized as QRS");
                if (result == qrs::DecodeResult::Complete) { complete = true; break; }
                Check(decoded.empty(), "partial QRS frames never become a configuration import");
            }
            Check(complete && decoded == content, "own QRS sharing reconstructs exact YAML/JSON for inline import");
        }
    }
}

} // namespace

int main() {
    RoundTrip(true);
    RoundTrip(false);
    OwnSharedProfiles();

    qrs::Decoder decoder;
    std::string payload;
    std::string error;
    Check(decoder.Add("https://example.com/subscription", payload, error) ==
              qrs::DecodeResult::NotQrs,
          "plain subscription URL is not mistaken for QRS");
    Check(decoder.Add("not base64", payload, error) ==
              qrs::DecodeResult::NotQrs,
          "invalid base64 is rejected");

    const qrs::Sequence sequence = qrs::Encode(Payload());
    Check(!sequence.frames.empty(), "fixture encoder produced frames");
    if (!sequence.frames.empty()) {
        std::string corrupt = sequence.frames.front();
        corrupt[corrupt.size() / 2] =
            corrupt[corrupt.size() / 2] == 'A' ? 'B' : 'A';
        qrs::Decoder corruptDecoder;
        const qrs::DecodeResult result =
            corruptDecoder.Add(corrupt, payload, error);
        Check(result == qrs::DecodeResult::NotQrs ||
                  result == qrs::DecodeResult::NeedMore,
              "corrupted frame does not complete");
    }

    if (failures == 0) {
        std::cout << "test_qrs_codec: ok\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
