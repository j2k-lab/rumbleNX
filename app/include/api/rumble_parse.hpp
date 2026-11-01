#pragma once

#include "api/rumble_chat_types.hpp"
#include "api/rumble_playlist.hpp"
#include "api/rumble_types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/// Network-free parsing of Rumble's pages and JSON. Everything here is pure so
/// it can be unit tested against saved responses.
namespace rumble::parse {

// ---------------------------------------------------------------- text

std::string trim(std::string value);
/// Decodes named (&amp; &quot; &#39; &nbsp; ...) and numeric (&#123; &#x1F;) entities.
std::string decodeEntities(const std::string& text);
/// Removes tags, turning <br> and block ends into newlines.
std::string stripTags(const std::string& html);
/// stripTags + decodeEntities + whitespace collapsed to single spaces.
std::string cleanText(const std::string& html);
/// Like cleanText but keeps line breaks (for descriptions).
std::string cleanMultiline(const std::string& html);

// ---------------------------------------------------------------- ids and urls

std::string toBase36(uint64_t value);
/// "449200450" -> "v7ffxaa" (the id the embed API expects). "" when invalid.
std::string embedIdForVideo(const std::string& numericId);
/// "v5pv5f" -> "9604851". "" when invalid.
std::string videoIdFromEmbedId(const std::string& embedId);
/// Site-relative path without query or fragment: "https://rumble.com/v1-a.html?x" -> "/v1-a.html".
std::string sitePath(const std::string& href);
/// "c/Name" or "user/Name" for a channel link, "" for anything else.
std::string channelPath(const std::string& href);
/// Makes protocol-relative and site-relative image URLs absolute.
std::string absoluteUrl(const std::string& src);

// ---------------------------------------------------------------- numbers

/// "1.44K" -> 1440, "251,197" -> 251197, "12.5K Followers" -> 12500, "2M" -> 2000000.
int64_t parseCount(const std::string& text);
/// "8:08:59" -> 29339, "1:23" -> 83. 0 when malformed.
int64_t parseClock(const std::string& text);
/// Unix time of "YYYY-MM-DDTHH:MM:SS[.fff](Z|±HH:MM)". 0 when malformed.
int64_t parseIsoTime(const std::string& iso);

// ---------------------------------------------------------------- pages

/// Videos from any Rumble listing markup: the "videostream" grid (browse/live,
/// editor picks), the older "video-listing-entry" list (trending, search) and
/// the JSON embedded in <rum-videos-grid> (channels, categories).
std::vector<Video> parseVideoListing(const std::string& html);

/// Categories from rumble.com/browse, in Rumble's order (busiest first).
std::vector<Category> parseCategories(const std::string& html);

/// Channels from rumble.com/search/channel.
std::vector<Channel> parseChannelSearch(const std::string& html);

/// Header of a channel page (rumble.com/c/Name or /user/Name).
Channel parseChannelHeader(const std::string& html, const std::string& path);

/// Description text from a channel's /about page.
std::string parseChannelDescription(const std::string& html);

/// True when the page links a following page with <link rel=next>.
bool hasNextPage(const std::string& html);

/// Channel links from the signed-in "followed channels" account page.
std::vector<Channel> parseFollowedChannels(const std::string& html);

// ---------------------------------------------------------------- oEmbed

/// Embed id ("v5pv5f") from an oEmbed response's iframe HTML. "" when absent.
std::string embedIdFromOEmbed(const nlohmann::json& oembed);

// ---------------------------------------------------------------- embed API

/// Parsed https://rumble.com/embedJS/u3/?request=video&ver=2&v=<embedId>.
struct EmbedVideo {
    std::string videoId;
    std::string title;
    std::string channelName;
    std::string channelPath;
    std::string pageUrl;
    std::string thumbnailUrl;
    std::string publishedAt;
    std::string systemMessage;
    int64_t durationSeconds = 0;
    /// Rumble's "live" field: 0 not live, 1 upcoming or just ended, 2 live.
    int live = 0;
    bool hasLiveField = false;
    bool dvr = false;
    /// The HLS rendition is still a live playlist (a stream that just ended).
    bool hlsLive = false;
    std::string hlsUrl;
    /// MP4 (or WebM when there is no MP4) renditions, tallest first.
    std::vector<Quality> progressive;
    std::vector<SubtitleTrack> subtitles;

    bool playable() const { return !hlsUrl.empty() || !progressive.empty(); }
};

EmbedVideo parseEmbed(const nlohmann::json& root);

// ---------------------------------------------------------------- chat (SSE)

struct ChatUser {
    std::string username;
    std::string color;
};

struct RantLevel {
    int priceCents = 0;
    std::string background;
    std::string foreground;
};

/// Users, channels and rant levels carried across SSE events.
struct ChatState {
    std::unordered_map<std::string, ChatUser> users;
    std::unordered_map<std::string, std::string> channels;
    std::vector<RantLevel> rantLevels;
    size_t messageLengthMax = 200;
    /// Emote name ("r+rumblerocket") to image URL, from emote.list.
    std::unordered_map<std::string, std::string> emotes;
};

struct ChatEvent {
    enum class Type { None, Messages, Delete, Other };
    Type type = Type::None;
    /// Rumble's event name ("init", "messages", "delete_messages", ...).
    std::string name;
    std::vector<ChatMessage> messages;
    std::vector<std::string> deletedIds;
};

/// Parses the JSON of one SSE "data:" line. Unknown events come back as
/// Type::Other; malformed data as Type::None.
ChatEvent parseChatEvent(const std::string& data, ChatState& state);

/// Splits message text into plain text and @mentions.
std::vector<ChatFragment> splitMentions(const std::string& text);

/// Splits ":name:" codes that are in `emotes` out of the text fragments.
std::vector<ChatFragment> splitEmotes(
    std::vector<ChatFragment> fragments, const std::unordered_map<std::string, std::string>& emotes);

/// Reads service.php?name=emote.list: every emote pack (Rumble's own and
/// the channel's) as name -> image URL.
std::unordered_map<std::string, std::string> parseEmoteList(const nlohmann::json& root);

/// Length the way Rumble's web client counts it (UTF-16 code units).
size_t chatLength(const std::string& utf8);

}  // namespace rumble::parse
