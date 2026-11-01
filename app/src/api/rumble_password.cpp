#include "api/rumble_auth.hpp"

#include "utils/md5.hpp"

#include <stdexcept>

namespace rumble {
namespace {

/// MD5 of salt + password, then re-hashed `iterations` times with the
/// password appended to the previous hex digest.
std::string stretch(const std::string& password, const std::string& salt, int iterations) {
    std::string current = rumblenx::md5Hex(salt + password);
    for (int i = 0; i < iterations; ++i) current = rumblenx::md5Hex(current + password);
    return current;
}

}  // namespace

std::string passwordHashes(const std::string& password, const std::vector<std::string>& salts) {
    if (salts.size() < 3) throw std::runtime_error("Rumble returned incomplete sign-in details.");
    const std::string first = rumblenx::md5Hex(stretch(password, salts[0], 128) + salts[1]);
    const std::string second = stretch(password, salts[2], 128);
    return first + "," + second + "," + salts[1];
}

}  // namespace rumble
