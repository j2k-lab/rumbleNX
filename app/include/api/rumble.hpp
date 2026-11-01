#pragma once

#include "api/http.hpp"
#include "api/rumble_playlist.hpp"
#include "api/rumble_types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rumble {

constexpr const char* SITE_URL = "https://rumble.com";

/// Headers for rumble.com pages (`json` = false) and APIs (`json` = true).
/// With `withSession`, the signed-in session cookie is attached.
HTTP::Header requestHeaders(bool json, bool withSession = false);

/// GET https://rumble.com<path>. Throws std::runtime_error with a readable
/// message on failure. `withSession` sends the signed-in session cookie.
std::string fetchPage(const std::string& path, const HTTP::Cancel& cancel = nullptr, bool withSession = false);

/// GET with the given headers, cancellable when `cancel` is set.
std::string httpGet(
    const std::string& url,
    const HTTP::Header& headers,
    long timeoutMs,
    const HTTP::Cancel& cancel = nullptr);

void throwIfCancelled(const HTTP::Cancel& cancel);

// ---------------------------------------------------------------- preferences

/// "auto" (adaptive), "1080p", "720p", "480p" or "360p". Anything else
/// saved (such as the old "audio") reads back as "auto".
std::string loadPreferredQuality();
bool savePreferredQuality(const std::string& quality);

enum class DecoderMode {
    Software = 0,
    Hardware = 1,
    Hybrid = 2,
};

// Software decoding is the most compatible. Hardware uses NVTEGRA. Hybrid
// starts in hardware and moves a live stream to software when its format
// changes mid-stream, restoring hardware once playback is stable again.
DecoderMode loadDecoderMode();
bool saveDecoderMode(DecoderMode mode);
bool decoderUsesHardware(DecoderMode mode);

// ---------------------------------------------------------------- playback

enum class PlaybackKind {
    Video,
    Live,
    Upcoming,
};

struct Playback {
    std::string videoId;
    std::string title;
    std::string channelName;
    std::string channelPath;
    std::string channelAvatarUrl;
    std::string pageUrl;
    std::string thumbnailUrl;
    std::string publishedAt;
    /// Rumble's own notice (e.g. why a video is unavailable or when a
    /// scheduled stream starts), if any.
    std::string message;
    int64_t durationSeconds = 0;
    /// Seconds a live stream has been running, 0 when unknown.
    int64_t uptimeSeconds = 0;
    PlaybackKind kind = PlaybackKind::Video;
    /// For HLS: "auto" (the master playlist) followed by each resolution;
    /// for MP4 uploads: each resolution.
    std::vector<Quality> qualities;
    Quality selected;
    std::vector<SubtitleTrack> subtitles;

    bool isLive() const { return kind == PlaybackKind::Live; }
};

/// Asks Rumble's embed API how to play `video`. Uses the numeric id when the
/// listing provided one, otherwise resolves the page URL through oEmbed.
/// Blocking; call from a worker thread.
Playback resolvePlayback(
    const Video& video,
    const std::string& preferredQuality,
    const HTTP::Cancel& cancel = nullptr);

using PlaybackCallback = std::function<void(Playback)>;

void resolvePlaybackAsync(
    const Video& video,
    const std::string& preferredQuality,
    PlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

/// Asks Rumble whether a live stream is still live. `known` is false when the
/// answer could not be determined (network error, unexpected reply), so a
/// failed lookup is never treated as "ended".
using LiveCheckCallback = std::function<void(bool known, bool live)>;
void checkVideoLiveAsync(const std::string& videoId, LiveCheckCallback done);

/// Per-file mpv options.
std::string mpvExtra(bool live);

/// Absolute rumble.com address of a video page, for sharing.
std::string videoPageUrl(const std::string& path);

}  // namespace rumble
