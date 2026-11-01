#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rumble {

/// Live state shown by a listing. Listings can be a few minutes stale, so the
/// player always asks the embed API for the authoritative state before it
/// opens a video.
enum class LiveStatus {
    None = 0,
    Upcoming = 1,
    Live = 2,
};

/// A Rumble video, live stream or scheduled stream as it appears in listings.
struct Video {
    /// Numeric video id in base 10 (e.g. "449200450"). The embed id is
    /// "v" + base36(id). Empty when a listing only exposed the page URL.
    std::string id;
    /// Site-relative page path without query, e.g. "/v7hma7s-title.html".
    std::string url;
    std::string title;
    std::string thumbnailUrl;
    std::string channelName;
    /// "c/Name" or "user/Name"; empty when unknown.
    std::string channelPath;
    std::string channelAvatarUrl;
    /// ISO 8601 upload/stream start time.
    std::string publishedAt;
    /// ISO 8601 start time of a scheduled stream.
    std::string scheduledAt;
    int64_t durationSeconds = 0;
    int64_t views = 0;
    /// Current viewers of a live stream.
    int64_t watching = 0;
    LiveStatus live = LiveStatus::None;
    bool channelVerified = false;

    bool isLive() const { return live == LiveStatus::Live; }
    bool isUpcoming() const { return live == LiveStatus::Upcoming; }
};

/// A Rumble channel ("c/Name") or user page ("user/Name").
struct Channel {
    std::string path;
    /// Numeric id from the page's follow button; may be empty.
    std::string id;
    std::string name;
    std::string avatarUrl;
    std::string bannerUrl;
    std::string description;
    /// Rumble's own wording, e.g. "12.5K Followers".
    std::string followersText;
    int64_t followers = 0;
    bool verified = false;
};

struct Category {
    /// URL slug, e.g. "gaming" for rumble.com/category/gaming.
    std::string slug;
    std::string name;
    std::string imageUrl;
    int64_t viewers = 0;
};

/// One page of results. `cursor` is opaque to the UI: empty when there are
/// no further pages, otherwise passed back to load the next page.
template <typename T>
struct Page {
    std::vector<T> items;
    std::string cursor;
    bool loaded = false;
};

using VideoPage = Page<Video>;
using ChannelPage = Page<Channel>;
using CategoryPage = Page<Category>;

using ErrorCallback = std::function<void(const std::string&)>;

}  // namespace rumble
