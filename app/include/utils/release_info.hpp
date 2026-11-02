#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

/// GitHub release metadata for the self-updater (no UI or network code).
namespace rumblenx::updater {

/// The release asset the updater installs.
constexpr const char* ASSET_NAME = "RumbleNX.nro";

struct Release {
    /// "1.2.3", read from the tag (or the release name).
    std::string version;
    std::string pageUrl;
    /// Empty when the release has no RumbleNX.nro asset.
    std::string assetUrl;
    int64_t assetSize = 0;
    /// Lowercase hex SHA-256 from GitHub's asset digest; empty if absent.
    std::string sha256;

    bool installable() const { return !assetUrl.empty() && assetSize > 0; }
};

/// The first "X.Y.Z" in text as numbers, e.g. "v0.1.0" -> {0, 1, 0}.
std::vector<int> parseVersion(const std::string& text);
std::string joinVersion(const std::vector<int>& parts);
bool isNewer(const std::string& latest, const std::string& current);

/// Reads a GitHub releases/latest response.
Release parseRelease(const nlohmann::json& root);

}  // namespace rumblenx::updater
