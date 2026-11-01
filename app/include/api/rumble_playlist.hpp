#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rumble {

/// One playable rendition. `id` is "auto" (adaptive HLS master) or
/// "<height>p".
struct Quality {
    std::string id;
    std::string name;
    std::string url;
    int height = 0;
    int64_t bandwidth = 0;
};

struct SubtitleTrack {
    std::string name;
    std::string language;
    std::string url;
    bool isDefault = false;
};

/// Parses an HLS master playlist. The result starts with an "auto" entry for
/// the master itself, followed by one entry per resolution (the highest
/// bandwidth variant wins when several share a resolution), tallest first.
/// Audio-only variants are left out.
std::vector<Quality> parseMasterPlaylist(
    const std::string& masterUrl,
    const std::string& playlistText);

std::vector<SubtitleTrack> parseMasterSubtitles(
    const std::string& masterUrl,
    const std::string& playlistText);

/// Picks the rendition for a saved preference: "auto" keeps the adaptive
/// master when there is one, and "<n>p" takes the tallest rendition not above n (or the shortest one when
/// all are taller). Throws when `qualities` is empty.
Quality selectQuality(
    const std::vector<Quality>& qualities,
    const std::string& preferred);

/// Resolves a playlist entry against the playlist's own URL.
std::string absoluteUrl(const std::string& baseUrl, const std::string& candidate);

/// RFC 3986 percent-encoding of everything except unreserved characters.
std::string percentEncode(const std::string& value);

}  // namespace rumble
