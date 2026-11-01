#include "api/rumble_playlist.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <stdexcept>

namespace rumble {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::map<std::string, std::string> parseAttributes(const std::string& line) {
    std::map<std::string, std::string> result;
    const auto colon = line.find(':');
    if (colon == std::string::npos) return result;

    const std::string body = line.substr(colon + 1);
    size_t cursor = 0;
    while (cursor < body.size()) {
        const size_t equals = body.find('=', cursor);
        if (equals == std::string::npos) break;

        const std::string key = trim(body.substr(cursor, equals - cursor));
        const size_t valueStart = equals + 1;
        std::string value;

        if (valueStart < body.size() && body[valueStart] == '"') {
            const size_t endQuote = body.find('"', valueStart + 1);
            if (endQuote == std::string::npos) break;
            value = body.substr(valueStart + 1, endQuote - valueStart - 1);
            cursor = endQuote + 1;
        } else {
            const size_t comma = body.find(',', valueStart);
            value = trim(body.substr(valueStart,
                comma == std::string::npos ? std::string::npos : comma - valueStart));
            cursor = comma == std::string::npos ? body.size() : comma;
        }

        if (!key.empty()) result[key] = value;
        if (cursor < body.size() && body[cursor] == ',') ++cursor;
    }
    return result;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(trim(line));
    }
    return lines;
}

int resolutionHeight(const std::map<std::string, std::string>& attrs) {
    const auto resolution = attrs.find("RESOLUTION");
    if (resolution == attrs.end()) return 0;
    const auto x = resolution->second.find('x');
    if (x == std::string::npos) return 0;
    try {
        return std::max(0, std::stoi(resolution->second.substr(x + 1)));
    } catch (...) {
        return 0;
    }
}

int64_t bandwidthOf(const std::map<std::string, std::string>& attrs) {
    for (const char* key : {"BANDWIDTH", "AVERAGE-BANDWIDTH"}) {
        const auto found = attrs.find(key);
        if (found == attrs.end()) continue;
        try {
            return std::max<int64_t>(0, std::stoll(found->second));
        } catch (...) {
        }
    }
    return 0;
}

bool isAudioOnlyVariant(const std::map<std::string, std::string>& attrs) {
    if (attrs.count("RESOLUTION")) return false;
    const auto codecs = attrs.find("CODECS");
    if (codecs == attrs.end()) return false;
    const std::string value = lower(codecs->second);
    for (const char* video : {"avc", "hvc", "hev", "vp0", "vp8", "vp9", "av01"}) {
        if (value.find(video) != std::string::npos) return false;
    }
    return value.find("mp4a") != std::string::npos ||
           value.find("opus") != std::string::npos ||
           value.find("ac-3") != std::string::npos;
}

std::string qualityName(int height, const std::map<std::string, std::string>& attrs) {
    std::string name = std::to_string(height) + "p";
    const auto fps = attrs.find("FRAME-RATE");
    if (fps != attrs.end() && !fps->second.empty()) {
        try {
            const int rounded = static_cast<int>(std::stod(fps->second) + 0.5);
            if (rounded >= 50) name += std::to_string(rounded);
        } catch (...) {
        }
    }
    return name;
}

std::string normalizePath(const std::string& origin, const std::string& path) {
    std::vector<std::string> segments;
    std::istringstream input(path);
    for (std::string segment; std::getline(input, segment, '/');) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!segments.empty()) segments.pop_back();
            continue;
        }
        segments.push_back(segment);
    }

    std::string result = origin + "/";
    for (size_t index = 0; index < segments.size(); ++index) {
        if (index) result += "/";
        result += segments[index];
    }
    if (!path.empty() && path.back() == '/' && !segments.empty()) result += "/";
    return result;
}

}  // namespace

std::string absoluteUrl(const std::string& baseUrl, const std::string& candidate) {
    if (candidate.empty()) return {};
    if (candidate.rfind("http://", 0) == 0 || candidate.rfind("https://", 0) == 0)
        return candidate;

    const auto scheme = baseUrl.find("://");
    if (scheme == std::string::npos) return candidate;
    if (candidate.rfind("//", 0) == 0) return baseUrl.substr(0, scheme + 1) + candidate;

    // The base's own query and fragment never apply to a relative reference.
    const std::string base = baseUrl.substr(0, baseUrl.find_first_of("?#"));
    const auto firstPath = base.find('/', scheme + 3);
    const std::string origin = firstPath == std::string::npos ? base : base.substr(0, firstPath);

    std::string reference = candidate;
    std::string suffix;
    const auto queryStart = reference.find_first_of("?#");
    if (queryStart != std::string::npos) {
        suffix = reference.substr(queryStart);
        reference.erase(queryStart);
    }

    if (!reference.empty() && reference.front() == '/')
        return normalizePath(origin, reference) + suffix;

    const std::string directory = firstPath == std::string::npos
        ? std::string("/")
        : base.substr(firstPath, base.rfind('/') - firstPath + 1);
    return normalizePath(origin, directory + reference) + suffix;
}

std::vector<Quality> parseMasterPlaylist(
    const std::string& masterUrl,
    const std::string& playlistText) {
    std::vector<Quality> variants;
    const auto lines = splitLines(playlistText);

    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("#EXT-X-STREAM-INF:", 0) != 0) continue;
        const auto attrs = parseAttributes(lines[i]);

        size_t next = i + 1;
        while (next < lines.size() && (lines[next].empty() || lines[next].front() == '#'))
            ++next;
        if (next >= lines.size()) continue;

        const std::string url = absoluteUrl(masterUrl, lines[next]);
        if (url.empty()) continue;

        Quality quality;
        quality.url = url;
        quality.bandwidth = bandwidthOf(attrs);
        quality.height = resolutionHeight(attrs);
        if (quality.height > 0) {
            quality.id = std::to_string(quality.height) + "p";
            quality.name = qualityName(quality.height, attrs);
        } else if (isAudioOnlyVariant(attrs)) {
            continue;
        } else {
            quality.id = "variant-" + std::to_string(variants.size() + 1);
            quality.name = "Variant " + std::to_string(variants.size() + 1);
        }

        // Several renditions can share a resolution (Rumble VODs list two
        // 1440p and two 360p tiers); keep the highest bitrate of each.
        const auto duplicate = std::find_if(variants.begin(), variants.end(),
            [&](const Quality& existing) { return existing.id == quality.id || existing.url == quality.url; });
        if (duplicate == variants.end()) {
            variants.push_back(std::move(quality));
        } else if (quality.bandwidth > duplicate->bandwidth) {
            *duplicate = std::move(quality);
        }
    }

    std::stable_sort(variants.begin(), variants.end(),
        [](const Quality& a, const Quality& b) { return a.height > b.height; });

    std::vector<Quality> result;
    result.push_back({"auto", "Auto", masterUrl, 0, 0});
    result.insert(result.end(), variants.begin(), variants.end());
    return result;
}

std::vector<SubtitleTrack> parseMasterSubtitles(
    const std::string& masterUrl,
    const std::string& playlistText) {
    std::vector<SubtitleTrack> result;
    for (const auto& line : splitLines(playlistText)) {
        if (line.rfind("#EXT-X-MEDIA:", 0) != 0) continue;

        const auto attrs = parseAttributes(line);
        const auto type = attrs.find("TYPE");
        if (type == attrs.end() || type->second != "SUBTITLES") continue;

        const auto uri = attrs.find("URI");
        if (uri == attrs.end() || uri->second.empty()) continue;

        const std::string url = absoluteUrl(masterUrl, uri->second);
        if (url.empty()) continue;

        SubtitleTrack track;
        track.url = url;
        if (attrs.count("NAME")) track.name = attrs.at("NAME");
        if (attrs.count("LANGUAGE")) track.language = attrs.at("LANGUAGE");
        if (track.name.empty()) track.name = track.language.empty() ? "Subtitles" : track.language;
        track.isDefault = attrs.count("DEFAULT") && attrs.at("DEFAULT") == "YES";

        const auto duplicate = std::find_if(result.begin(), result.end(),
            [&](const SubtitleTrack& existing) { return existing.url == url; });
        if (duplicate == result.end()) result.push_back(std::move(track));
    }
    return result;
}

Quality selectQuality(
    const std::vector<Quality>& qualities,
    const std::string& preferred) {
    if (qualities.empty()) throw std::runtime_error("Rumble returned no playable renditions");

    const std::string wanted = lower(trim(preferred));
    const auto isVideo = [](const Quality& quality) { return quality.id != "auto"; };

    if (wanted.empty() || wanted == "auto") {
        const auto automatic = std::find_if(qualities.begin(), qualities.end(),
            [](const Quality& quality) { return quality.id == "auto"; });
        if (automatic != qualities.end()) return *automatic;
    }

    int limit = 0;
    if (!wanted.empty() && std::isdigit(static_cast<unsigned char>(wanted.front()))) {
        try {
            limit = std::stoi(wanted);
        } catch (...) {
            limit = 0;
        }
    }

    // "auto" without a master (plain MP4 uploads) falls back to the tallest
    // video up to 1080p, which the Switch decodes comfortably.
    if (limit <= 0) limit = 1080;

    const Quality* best = nullptr;
    const Quality* shortest = nullptr;
    for (const auto& quality : qualities) {
        if (!isVideo(quality)) continue;
        if (quality.height > 0 && quality.height <= limit && (!best || quality.height > best->height))
            best = &quality;
        if (!shortest || (quality.height > 0 && (shortest->height <= 0 || quality.height < shortest->height)))
            shortest = &quality;
    }
    if (best) return *best;
    if (shortest) return *shortest;

    // Only "auto" exists.
    return qualities.front();
}

std::string percentEncode(const std::string& value) {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size() * 3);
    for (unsigned char c : value) {
        const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(HEX[(c >> 4) & 0x0F]);
            result.push_back(HEX[c & 0x0F]);
        }
    }
    return result;
}

}  // namespace rumble
