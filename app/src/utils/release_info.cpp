#include "utils/release_info.hpp"

#include <algorithm>
#include <cctype>

namespace rumblenx::updater {
namespace {

using json = nlohmann::json;

std::string jsonText(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
}

}  // namespace

std::vector<int> parseVersion(const std::string& text) {
    std::vector<int> parts;
    size_t i = 0;
    while (i < text.size() && !std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        int value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            ++i;
        }
        parts.push_back(value);
        if (i + 1 < text.size() && text[i] == '.' && std::isdigit(static_cast<unsigned char>(text[i + 1]))) ++i;
        else break;
    }
    return parts;
}

std::string joinVersion(const std::vector<int>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '.';
        out += std::to_string(parts[i]);
    }
    return out;
}

bool isNewer(const std::string& latest, const std::string& current) {
    auto a = parseVersion(latest);
    auto b = parseVersion(current);
    if (a.empty()) return false;
    // "1.2" and "1.2.0" are the same version.
    const size_t length = std::max(a.size(), b.size());
    a.resize(length, 0);
    b.resize(length, 0);
    return a > b;
}

Release parseRelease(const json& root) {
    Release release;
    if (!root.is_object()) return release;
    auto version = parseVersion(jsonText(root, "tag_name"));
    if (version.empty()) version = parseVersion(jsonText(root, "name"));
    release.version = joinVersion(version);
    release.pageUrl = jsonText(root, "html_url");

    const auto assets = root.find("assets");
    if (assets == root.end() || !assets->is_array()) return release;
    for (const auto& asset : *assets) {
        if (!asset.is_object() || jsonText(asset, "name") != ASSET_NAME) continue;
        const std::string url = jsonText(asset, "browser_download_url");
        if (url.rfind("https://", 0) != 0) continue;
        release.assetUrl = url;
        const auto size = asset.find("size");
        if (size != asset.end() && size->is_number_integer()) release.assetSize = size->get<int64_t>();
        const std::string digest = jsonText(asset, "digest");
        if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) {
            release.sha256 = digest.substr(7);
            std::transform(release.sha256.begin(), release.sha256.end(), release.sha256.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }
        break;
    }
    return release;
}

}  // namespace rumblenx::updater
