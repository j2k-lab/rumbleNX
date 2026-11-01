#include "api/rumble.hpp"

#include "api/rumble_auth.hpp"
#include "api/rumble_parse.hpp"
#include "api/rumble_settings.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <stdexcept>

namespace rumble {
namespace {

using json = nlohmann::json;

const std::vector<std::string>& validQualities() {
    static const std::vector<std::string> values = {"auto", "1080p", "720p", "480p", "360p"};
    return values;
}

/// http.cpp reports HTTP failures as "http status <code>".
std::string readableError(const std::string& what, const std::string& subject) {
    const std::string marker = "http status ";
    const size_t pos = what.find(marker);
    if (pos == std::string::npos) return what;
    const std::string code = what.substr(pos + marker.size());
    if (code == "403" || code == "429" || code == "503")
        return "Rumble is not accepting requests from this network right now (HTTP " + code +
               "). Try again in a few minutes.";
    if (code == "404") return subject + " was not found on Rumble.";
    return "Rumble returned HTTP " + code + " for " + subject + ".";
}

json fetchJson(const std::string& url, long timeoutMs, const HTTP::Cancel& cancel, const std::string& subject) {
    std::string body;
    try {
        body = httpGet(url, requestHeaders(true), timeoutMs, cancel);
    } catch (const std::exception& exception) {
        throwIfCancelled(cancel);
        throw std::runtime_error(readableError(exception.what(), subject));
    }
    json root = json::parse(body, nullptr, false);
    if (root.is_discarded()) throw std::runtime_error("Rumble returned an unreadable reply for " + subject + ".");
    return root;
}

std::string embedIdFor(const Video& video, const HTTP::Cancel& cancel) {
    std::string embedId = parse::embedIdForVideo(video.id);
    if (!embedId.empty()) return embedId;
    if (video.url.empty()) throw std::runtime_error("This video has no Rumble address.");

    const json oembed = fetchJson(
        std::string(SITE_URL) + "/api/Media/oembed.json?url=" + percentEncode(std::string(SITE_URL) + video.url),
        15000, cancel, "this video");
    embedId = parse::embedIdFromOEmbed(oembed);
    if (embedId.empty()) throw std::runtime_error("Rumble did not return a player for this video.");
    return embedId;
}

std::string stringSetting(const json& root, const char* key, const std::string& fallback) {
    const auto it = root.find(key);
    return it != root.end() && it->is_string() ? it->get<std::string>() : fallback;
}

}  // namespace

HTTP::Header requestHeaders(bool json, bool withSession) {
    HTTP::Header headers = {
        json ? "Accept: application/json, text/plain, */*"
             : "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9",
    };
    if (withSession) {
        const Session session = loadSession();
        if (session.signedIn()) headers.push_back("Cookie: u_s=" + session.token);
    }
    return headers;
}

std::string httpGet(const std::string& url, const HTTP::Header& headers, long timeoutMs, const HTTP::Cancel& cancel) {
    if (cancel) return HTTP::get(url, headers, HTTP::Timeout{timeoutMs}, cancel);
    return HTTP::get(url, headers, HTTP::Timeout{timeoutMs});
}

void throwIfCancelled(const HTTP::Cancel& cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Request cancelled");
}

std::string fetchPage(const std::string& path, const HTTP::Cancel& cancel, bool withSession) {
    throwIfCancelled(cancel);
    try {
        return httpGet(std::string(SITE_URL) + path, requestHeaders(false, withSession), 20000, cancel);
    } catch (const std::exception& exception) {
        throwIfCancelled(cancel);
        throw std::runtime_error(readableError(exception.what(), "This page"));
    }
}

// ---------------------------------------------------------------- preferences

std::string loadPreferredQuality() {
    const std::string value = stringSetting(settings::read(settings::PREFERENCES), "preferred_quality", "auto");
    const auto& valid = validQualities();
    return std::find(valid.begin(), valid.end(), value) == valid.end() ? "auto" : value;
}

bool savePreferredQuality(const std::string& quality) {
    return settings::update(settings::PREFERENCES, [&](json& root) { root["preferred_quality"] = quality; });
}

DecoderMode loadDecoderMode() {
    const json root = settings::read(settings::PREFERENCES);
    const auto it = root.find("decoder_mode");
    if (it != root.end() && it->is_number_integer()) {
        const int value = it->get<int>();
        if (value >= static_cast<int>(DecoderMode::Software) && value <= static_cast<int>(DecoderMode::Hybrid))
            return static_cast<DecoderMode>(value);
    }
    return DecoderMode::Hybrid;
}

bool saveDecoderMode(DecoderMode mode) {
    return settings::update(settings::PREFERENCES, [&](json& root) { root["decoder_mode"] = static_cast<int>(mode); });
}

bool decoderUsesHardware(DecoderMode mode) { return mode == DecoderMode::Hardware || mode == DecoderMode::Hybrid; }

// ---------------------------------------------------------------- playback

Playback resolvePlayback(const Video& video, const std::string& preferredQuality, const HTTP::Cancel& cancel) {
    throwIfCancelled(cancel);
    const std::string embedId = embedIdFor(video, cancel);
    throwIfCancelled(cancel);

    const json root = fetchJson(
        std::string(SITE_URL) + "/embedJS/u3/?request=video&ver=2&v=" + embedId, 15000, cancel, "this video");
    const parse::EmbedVideo embed = parse::parseEmbed(root);
    throwIfCancelled(cancel);

    Playback playback;
    playback.videoId = embed.videoId.empty() ? parse::videoIdFromEmbedId(embedId) : embed.videoId;
    playback.title = embed.title.empty() ? video.title : embed.title;
    playback.channelName = embed.channelName.empty() ? video.channelName : embed.channelName;
    playback.channelPath = embed.channelPath.empty() ? video.channelPath : embed.channelPath;
    playback.channelAvatarUrl = video.channelAvatarUrl;
    playback.pageUrl = embed.pageUrl.empty() ? video.url : embed.pageUrl;
    playback.thumbnailUrl = embed.thumbnailUrl.empty() ? video.thumbnailUrl : embed.thumbnailUrl;
    playback.publishedAt = embed.publishedAt.empty() ? video.publishedAt : embed.publishedAt;
    playback.durationSeconds = embed.durationSeconds;
    playback.message = embed.systemMessage;
    playback.subtitles = embed.subtitles;

    if (embed.live == 2) {
        if (embed.hlsUrl.empty())
            throw std::runtime_error("This live stream has not published its video yet. Try again in a moment.");
        playback.kind = PlaybackKind::Live;
        const int64_t started = parse::parseIsoTime(playback.publishedAt);
        const int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (started > 0 && now > started) playback.uptimeSeconds = now - started;
    } else if (!embed.playable()) {
        if (embed.live == 1 || video.isUpcoming()) {
            playback.kind = PlaybackKind::Upcoming;
            return playback;
        }
        throw std::runtime_error(!embed.systemMessage.empty()
            ? embed.systemMessage
            : "Rumble returned no playable stream for this video. It may be private, removed or restricted.");
    }

    if (!embed.hlsUrl.empty()) {
        try {
            const std::string playlist = httpGet(embed.hlsUrl,
                {"Accept: application/vnd.apple.mpegurl, application/x-mpegURL, */*"}, 12000, cancel);
            playback.qualities = parseMasterPlaylist(embed.hlsUrl, playlist);
            for (auto& track : parseMasterSubtitles(embed.hlsUrl, playlist)) {
                const bool duplicate = std::any_of(playback.subtitles.begin(), playback.subtitles.end(),
                    [&](const SubtitleTrack& existing) { return existing.url == track.url; });
                if (!duplicate) playback.subtitles.push_back(std::move(track));
            }
        } catch (const std::exception& exception) {
            throwIfCancelled(cancel);
            // The master still plays; only the list of fixed renditions is lost.
            brls::Logger::warning("RumbleNX: could not read the HLS master playlist: {}", exception.what());
            playback.qualities = {{"auto", "Auto", embed.hlsUrl, 0, 0}};
        }
    } else {
        playback.qualities = embed.progressive;
    }

    playback.selected = selectQuality(playback.qualities, preferredQuality);
    if (playback.selected.url.empty()) throw std::runtime_error("Rumble returned an empty stream address.");
    return playback;
}

void resolvePlaybackAsync(
    const Video& video,
    const std::string& preferredQuality,
    PlaybackCallback success,
    ErrorCallback failure,
    HTTP::Cancel cancel) {
    ThreadPool::instance().submit([video, preferredQuality, success = std::move(success),
                                      failure = std::move(failure), cancel = std::move(cancel)](HTTP&) mutable {
        try {
            Playback playback = resolvePlayback(video, preferredQuality, cancel);
            brls::sync(std::bind(std::move(success), std::move(playback)));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        } catch (...) {
            brls::sync(std::bind(std::move(failure), std::string("Unknown Rumble playback error")));
        }
    });
}

void checkVideoLiveAsync(const std::string& videoId, LiveCheckCallback done) {
    ThreadPool::instance().submit([videoId, done = std::move(done)](HTTP&) mutable {
        bool known = false;
        bool live = false;
        try {
            const std::string embedId = parse::embedIdForVideo(videoId);
            if (!embedId.empty()) {
                const json root = json::parse(
                    httpGet(std::string(SITE_URL) + "/embedJS/u3/?request=video&ver=2&v=" + embedId,
                        requestHeaders(true), 10000),
                    nullptr, false);
                const parse::EmbedVideo embed = parse::parseEmbed(root);
                known = embed.hasLiveField;
                live = embed.live == 2;
            }
        } catch (const std::exception& error) {
            brls::Logger::warning("RumbleNX: live-status check failed for {}: {}", videoId, error.what());
        }
        brls::sync(std::bind(std::move(done), known, live));
    });
}

std::string mpvExtra(bool live) {
    // HTTP headers, TLS and the network timeout are configured globally in
    // MPVCore::init(), because values with spaces break the per-file option
    // parser. Live streams force the demuxer cache on and read a fixed eight
    // seconds ahead.
    return live ? "cache=yes,cache-secs=8" : "";
}

std::string videoPageUrl(const std::string& path) {
    if (path.empty()) return SITE_URL;
    return path.front() == '/' ? std::string(SITE_URL) + path : path;
}

}  // namespace rumble
