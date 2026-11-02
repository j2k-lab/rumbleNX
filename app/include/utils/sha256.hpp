#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>

namespace rumblenx {

/// Incremental SHA-256 (FIPS 180-4). Used to check a downloaded update
/// against the digest GitHub publishes for the release asset.
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t size);
    /// Lowercase hex digest. The object must not be updated afterwards.
    std::string finishHex();

private:
    void transform(const uint8_t* block);

    uint32_t state[8];
    uint8_t buffer[64];
    size_t bufferSize = 0;
    uint64_t totalBytes = 0;
};

std::string sha256Hex(const std::string& data);
/// Digest of everything left in `in`; empty when the stream fails.
std::string sha256Hex(std::istream& in);

}  // namespace rumblenx
