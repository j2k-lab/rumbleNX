#pragma once

#include <string>

namespace rumblenx {

/// Lowercase hex MD5 digest of `data` (RFC 1321). Used only for Rumble's
/// salted password hashing scheme, never for security decisions of our own.
std::string md5Hex(const std::string& data);

}  // namespace rumblenx
