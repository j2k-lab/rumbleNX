#include "api/rumble_parse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <utility>

namespace rumble::parse {
namespace {

using json = nlohmann::json;
constexpr size_t NPOS = std::string::npos;
constexpr const char* SITE = "https://rumble.com";

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

std::string lowerAscii(std::string value) {
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// ------------------------------------------------------------- tags

/// End ('>') of the tag opening at `lt`, honouring quoted attribute values.
size_t tagEnd(const std::string& html, size_t lt) {
    char quote = 0;
    for (size_t i = lt + 1; i < html.size(); ++i) {
        const char c = html[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return NPOS;
}

/// The complete tag that contains position `pos`.
std::string enclosingTag(const std::string& html, size_t pos) {
    if (pos == NPOS || pos >= html.size()) return {};
    const size_t lt = html.rfind('<', pos);
    if (lt == NPOS) return {};
    const size_t gt = tagEnd(html, lt);
    if (gt == NPOS || gt < pos) return {};
    return html.substr(lt, gt - lt + 1);
}

std::string tagName(const std::string& tag) {
    if (tag.size() < 2 || tag[0] != '<') return {};
    size_t i = 1;
    while (i < tag.size() && isSpace(tag[i])) ++i;
    const size_t start = i;
    while (i < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[i])) || tag[i] == '-' || tag[i] == ':'))
        ++i;
    return lowerAscii(tag.substr(start, i - start));
}

/// Value of attribute `name` in an opening tag ("" when absent), entity-decoded.
std::string attribute(const std::string& tag, const std::string& name) {
    size_t i = 1;
    while (i < tag.size() && !isSpace(tag[i]) && tag[i] != '>' && tag[i] != '/') ++i;  // tag name
    const std::string wanted = lowerAscii(name);
    while (i < tag.size()) {
        while (i < tag.size() && (isSpace(tag[i]) || tag[i] == '/')) ++i;
        if (i >= tag.size() || tag[i] == '>') break;

        const size_t nameStart = i;
        while (i < tag.size() && !isSpace(tag[i]) && tag[i] != '=' && tag[i] != '>' && tag[i] != '/') ++i;
        const std::string attrName = lowerAscii(tag.substr(nameStart, i - nameStart));
        while (i < tag.size() && isSpace(tag[i])) ++i;

        std::string value;
        if (i < tag.size() && tag[i] == '=') {
            ++i;
            while (i < tag.size() && isSpace(tag[i])) ++i;
            if (i < tag.size() && (tag[i] == '"' || tag[i] == '\'')) {
                const char quote = tag[i++];
                const size_t valueStart = i;
                while (i < tag.size() && tag[i] != quote) ++i;
                value = tag.substr(valueStart, i - valueStart);
                if (i < tag.size()) ++i;
            } else {
                const size_t valueStart = i;
                while (i < tag.size() && !isSpace(tag[i]) && tag[i] != '>') ++i;
                value = tag.substr(valueStart, i - valueStart);
            }
        }
        if (attrName == wanted) return decodeEntities(value);
    }
    return {};
}

/// Position of a whole class token (or other attribute token) in [from, to).
size_t findToken(const std::string& html, const std::string& token, size_t from, size_t to) {
    to = std::min(to, html.size());
    size_t pos = from;
    while ((pos = html.find(token, pos)) != NPOS && pos + token.size() <= to) {
        const char before = pos > 0 ? html[pos - 1] : ' ';
        const char after = pos + token.size() < html.size() ? html[pos + token.size()] : ' ';
        const bool startOk = before == '"' || before == '\'' || before == '=' || isSpace(before);
        const bool endOk = after == '"' || after == '\'' || after == '>' || isSpace(after);
        if (startOk && endOk) return pos;
        pos += token.size();
    }
    return NPOS;
}

/// Opening tag of the element carrying class token `token` inside [from, to).
std::string tagWithToken(const std::string& html, const std::string& token, size_t from, size_t to) {
    return enclosingTag(html, findToken(html, token, from, to));
}

/// Raw inner HTML of the element whose opening tag contains `pos`, up to the
/// first matching closing tag (adequate for leaf elements such as h3/span).
std::string innerHtmlAt(const std::string& html, size_t pos) {
    if (pos == NPOS) return {};
    const size_t lt = html.rfind('<', pos);
    if (lt == NPOS) return {};
    const size_t gt = tagEnd(html, lt);
    if (gt == NPOS) return {};
    const std::string name = tagName(html.substr(lt, gt - lt + 1));
    if (name.empty()) return {};
    const size_t close = html.find("</" + name, gt + 1);
    if (close == NPOS) return {};
    return html.substr(gt + 1, close - gt - 1);
}

std::string innerTextWithToken(const std::string& html, const std::string& token, size_t from, size_t to) {
    return cleanText(innerHtmlAt(html, findToken(html, token, from, to)));
}

/// Maps the token after `prefix` in CSS rules such as
/// `.video-item--by-a--u4qvqm4::before { background-image: url("...") }`
/// to the image URL.
std::unordered_map<std::string, std::string> backgroundImages(
    const std::string& html, const std::string& prefix) {
    std::unordered_map<std::string, std::string> result;
    size_t pos = 0;
    while ((pos = html.find(prefix, pos)) != NPOS) {
        size_t cursor = pos + prefix.size();
        const size_t tokenStart = cursor;
        while (cursor < html.size() && isTokenChar(html[cursor])) ++cursor;
        const std::string token = html.substr(tokenStart, cursor - tokenStart);
        pos = cursor;
        if (token.empty()) continue;

        // Only a CSS rule continues with optional pseudo-elements and "{".
        size_t brace = cursor;
        while (brace < html.size() && brace < cursor + 24 && html[brace] != '{' && html[brace] != '"' &&
               html[brace] != '\'' && html[brace] != '<' && html[brace] != '>')
            ++brace;
        if (brace >= html.size() || html[brace] != '{') continue;

        const size_t close = html.find('}', brace);
        if (close == NPOS) break;
        const std::string body = html.substr(brace + 1, close - brace - 1);
        const size_t url = body.find("url(");
        if (url == NPOS) continue;
        const size_t end = body.find(')', url + 4);
        if (end == NPOS) continue;
        std::string value = trim(body.substr(url + 4, end - url - 4));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        if (!value.empty() && !result.count(token)) result.emplace(token, absoluteUrl(decodeEntities(value)));
        pos = close;
    }
    return result;
}

std::string readToken(const std::string& html, size_t from) {
    size_t end = from;
    while (end < html.size() && isTokenChar(html[end])) ++end;
    return html.substr(from, end - from);
}

/// Bounds of each item whose opening tag carries `marker`, where an item ends
/// where the next one starts (the last one is capped at `maxLength`).
std::vector<std::pair<size_t, size_t>> itemBlocks(
    const std::string& html, const std::string& marker, size_t maxLength) {
    std::vector<size_t> starts;
    size_t pos = 0;
    while ((pos = html.find(marker, pos)) != NPOS) {
        const size_t lt = html.rfind('<', pos);
        if (lt != NPOS) starts.push_back(lt);
        pos += marker.size();
    }
    std::vector<std::pair<size_t, size_t>> blocks;
    for (size_t index = 0; index < starts.size(); ++index) {
        const size_t limit = std::min(html.size(), starts[index] + maxLength);
        const size_t end = index + 1 < starts.size() ? std::min(starts[index + 1], limit) : limit;
        if (end > starts[index]) blocks.emplace_back(starts[index], end);
    }
    return blocks;
}

// ------------------------------------------------------------- json

std::string jsonString(const json& obj, const char* key) {
    if (!obj.is_object()) return {};
    const auto it = obj.find(key);
    if (it == obj.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_unsigned()) return std::to_string(it->get<unsigned long long>());
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

int64_t jsonInt(const json& obj, const char* key, int64_t fallback = 0) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<unsigned long long>());
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number_float()) return static_cast<int64_t>(std::llround(it->get<double>()));
    if (it->is_string()) {
        const std::string value = it->get<std::string>();
        if (!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
            try {
                return std::stoll(value);
            } catch (...) {
            }
        }
    }
    return fallback;
}

bool jsonTruthy(const json& obj, const char* key) {
    if (!obj.is_object()) return false;
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer() || it->is_number_unsigned()) return it->get<long long>() != 0;
    if (it->is_number_float()) return it->get<double>() != 0.0;
    if (it->is_string()) return !it->get<std::string>().empty();
    return true;
}

const json& jsonChild(const json& obj, const char* key) {
    static const json empty = json::object();
    if (!obj.is_object()) return empty;
    const auto it = obj.find(key);
    return it == obj.end() ? empty : *it;
}

// ------------------------------------------------------------- listings

void addVideo(std::vector<Video>& out, std::unordered_set<std::string>& seen, Video video) {
    if (video.id.empty() && video.url.empty()) return;
    const std::string key = video.id.empty() ? video.url : video.id;
    if (!seen.insert(key).second) return;
    if (video.title.empty()) video.title = "Untitled video";
    out.push_back(std::move(video));
}

void parseGridJson(const std::string& html, std::vector<Video>& out, std::unordered_set<std::string>& seen) {
    size_t pos = 0;
    while ((pos = html.find("<rum-videos-grid", pos)) != NPOS) {
        const size_t closeGrid = html.find("</rum-videos-grid", pos);
        const size_t script = html.find("<script type=\"application/json\"", pos);
        pos += 16;
        if (script == NPOS || (closeGrid != NPOS && script > closeGrid)) continue;
        const size_t start = tagEnd(html, script);
        const size_t end = start == NPOS ? NPOS : html.find("</script>", start);
        if (end == NPOS) continue;

        const json root = json::parse(html.begin() + static_cast<std::ptrdiff_t>(start) + 1,
            html.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
        if (root.is_discarded() || !root.is_object() || !root.contains("items") || !root["items"].is_array())
            continue;

        for (const auto& item : root["items"]) {
            if (!item.is_object()) continue;
            if (item.contains("object_type") && jsonString(item, "object_type") != "video") continue;

            Video video;
            video.id = jsonString(item, "id");
            if (!std::all_of(video.id.begin(), video.id.end(), [](unsigned char c) { return std::isdigit(c); }))
                video.id.clear();
            video.url = sitePath(jsonString(item, "relative_url"));
            if (video.url.empty()) video.url = sitePath(jsonString(item, "url"));
            video.title = trim(decodeEntities(jsonString(item, "title")));
            video.thumbnailUrl = absoluteUrl(jsonString(item, "thumb"));
            video.publishedAt = jsonString(item, "upload_date");
            video.durationSeconds = std::max<int64_t>(0, jsonInt(item, "duration"));
            video.views = std::max<int64_t>(0, jsonInt(item, "views"));
            video.watching = std::max<int64_t>(0, jsonInt(item, "watching_now"));

            const int64_t status = jsonInt(item, "livestream_status");
            const std::string scheduled = jsonString(item, "live_datetime");
            if (status == 2 || jsonTruthy(item, "live")) {
                video.live = LiveStatus::Live;
                if (!jsonString(item, "live_streamed_on").empty())
                    video.publishedAt = jsonString(item, "live_streamed_on");
            } else if (status == 1 || (!scheduled.empty() && video.durationSeconds == 0)) {
                video.live = LiveStatus::Upcoming;
                video.scheduledAt = scheduled;
            }

            const json& by = jsonChild(item, "by");
            video.channelName = trim(decodeEntities(jsonString(by, "title")));
            if (video.channelName.empty()) video.channelName = trim(decodeEntities(jsonString(by, "name")));
            video.channelPath = channelPath(jsonString(by, "relative_url"));
            if (video.channelPath.empty()) video.channelPath = channelPath(jsonString(by, "url"));
            video.channelAvatarUrl = absoluteUrl(jsonString(by, "thumb"));
            video.channelVerified = jsonTruthy(by, "verified_badge");

            addVideo(out, seen, std::move(video));
        }
    }
}

void parseVideostreamItems(const std::string& html, std::vector<Video>& out, std::unordered_set<std::string>& seen) {
    // Both "thumbnail__grid-item" (browse/live) and "thumbnail__grid--item"
    // (editor picks) variants start with this class pair.
    for (const auto& [start, end] : itemBlocks(html, "class=\"videostream thumbnail__grid", 24000)) {
        Video video;
        const std::string opening = enclosingTag(html, start + 1);
        video.id = attribute(opening, "data-video-id");
        if (!std::all_of(video.id.begin(), video.id.end(), [](unsigned char c) { return std::isdigit(c); }))
            video.id.clear();

        video.url = sitePath(attribute(tagWithToken(html, "title__link", start, end), "href"));
        if (video.url.empty())
            video.url = sitePath(attribute(tagWithToken(html, "videostream__link", start, end), "href"));

        const std::string titleTag = tagWithToken(html, "thumbnail__title", start, end);
        video.title = trim(attribute(titleTag, "title"));
        if (video.title.empty()) video.title = innerTextWithToken(html, "thumbnail__title", start, end);

        const std::string image = tagWithToken(html, "thumbnail__image", start, end);
        if (tagName(image) == "img") video.thumbnailUrl = absoluteUrl(attribute(image, "src"));

        if (findToken(html, "videostream__status--live", start, end) != NPOS) {
            video.live = LiveStatus::Live;
            video.watching = parseCount(innerTextWithToken(html, "videostream__number", start, end));
        } else if (findToken(html, "videostream__status--upcoming", start, end) != NPOS ||
                   findToken(html, "videostream__status--scheduled", start, end) != NPOS) {
            video.live = LiveStatus::Upcoming;
        }
        video.durationSeconds =
            parseClock(innerTextWithToken(html, "videostream__status--duration", start, end));

        const std::string author = tagWithToken(html, "channel__link", start, end);
        video.channelPath = channelPath(attribute(author, "href"));
        const std::string nameTag = tagWithToken(html, "channel__name", start, end);
        video.channelName = trim(attribute(nameTag, "title"));
        if (video.channelName.empty()) video.channelName = innerTextWithToken(html, "channel__name", start, end);
        const std::string avatar = tagWithToken(html, "channel__image", start, end);
        if (tagName(avatar) == "img") video.channelAvatarUrl = absoluteUrl(attribute(avatar, "src"));
        video.channelVerified = findToken(html, "channel__verified", start, end) != NPOS;

        video.publishedAt = attribute(tagWithToken(html, "videostream__time", start, end), "datetime");
        video.views = parseCount(attribute(tagWithToken(html, "videostream__views", start, end), "data-views"));

        addVideo(out, seen, std::move(video));
    }
}

void parseListingEntries(const std::string& html, std::vector<Video>& out, std::unordered_set<std::string>& seen) {
    const auto avatars = backgroundImages(html, ".video-item--by-a--");
    size_t pos = 0;
    while ((pos = html.find("<li class=\"video-listing-entry\"", pos)) != NPOS) {
        const size_t start = pos;
        size_t end = html.find("</article>", start);
        const size_t next = html.find("<li class=\"video-listing-entry\"", start + 10);
        if (end == NPOS || (next != NPOS && next < end)) end = next == NPOS ? std::min(html.size(), start + 16000) : next;
        pos = start + 10;

        Video video;
        video.url = sitePath(attribute(tagWithToken(html, "video-item--a", start, end), "href"));
        const std::string image = tagWithToken(html, "video-item--img", start, end);
        if (tagName(image) == "img") video.thumbnailUrl = absoluteUrl(attribute(image, "src"));

        video.title = innerTextWithToken(html, "video-item--title", start, end);
        if (video.title.empty()) video.title = trim(attribute(image, "alt"));

        const std::string liveTag = tagWithToken(html, "video-item--live", start, end);
        const std::string upcomingTag = tagWithToken(html, "video-item--upcoming", start, end);
        if (!liveTag.empty()) {
            video.live = LiveStatus::Live;
            video.watching = parseCount(innerTextWithToken(html, "video-item--watching", start, end));
        } else if (!upcomingTag.empty()) {
            video.live = LiveStatus::Upcoming;
        }
        video.durationSeconds =
            parseClock(attribute(tagWithToken(html, "video-item--duration", start, end), "data-value"));
        video.publishedAt = attribute(tagWithToken(html, "video-item--time", start, end), "datetime");

        const size_t authorPos = findToken(html, "video-item--by-a", start, end);
        const std::string author = enclosingTag(html, authorPos);
        video.channelPath = channelPath(attribute(author, "href"));
        if (authorPos != NPOS) {
            const size_t authorEnd = html.find("</a>", authorPos);
            const size_t limit = authorEnd == NPOS ? end : std::min(end, authorEnd);
            const size_t nameStart = tagEnd(html, html.rfind('<', authorPos));
            if (nameStart != NPOS && nameStart < limit)
                video.channelName = cleanText(html.substr(nameStart + 1, limit - nameStart - 1));

            const std::string classes = attribute(author, "class");
            const size_t marker = classes.find("video-item--by-a--");
            if (marker != NPOS) {
                const auto found = avatars.find(readToken(classes, marker + 18));
                if (found != avatars.end()) video.channelAvatarUrl = found->second;
            }
        }
        video.channelVerified = findToken(html, "video-item--by-verified", start, end) != NPOS;

        const std::string vote = tagWithToken(html, "rumbles-vote", start, end);
        if (attribute(vote, "data-type") == "1") {
            video.id = attribute(vote, "data-id");
            if (!std::all_of(video.id.begin(), video.id.end(), [](unsigned char c) { return std::isdigit(c); }))
                video.id.clear();
        }
        video.views = parseCount(innerTextWithToken(html, "video-item--views", start, end));

        addVideo(out, seen, std::move(video));
    }
}

/// "YYYY-MM-DDTHH:MM[:SS[.fff]](Z|±HH:MM)" -> "HH:MM" in UTC.
std::string utcClock(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') return {};
    int hour = 0;
    int minute = 0;
    try {
        hour = std::stoi(iso.substr(11, 2));
        minute = std::stoi(iso.substr(14, 2));
    } catch (...) {
        return {};
    }
    int offset = 0;
    const size_t zone = iso.find_first_of("+-Z", 16);
    if (zone != NPOS && iso[zone] != 'Z' && zone + 5 < iso.size() + 1) {
        try {
            const int zoneHours = std::stoi(iso.substr(zone + 1, 2));
            const int zoneMinutes = iso.size() >= zone + 6 ? std::stoi(iso.substr(zone + 4, 2)) : 0;
            offset = (zoneHours * 60 + zoneMinutes) * (iso[zone] == '-' ? -1 : 1);
        } catch (...) {
            offset = 0;
        }
    }
    int total = ((hour * 60 + minute - offset) % 1440 + 1440) % 1440;
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", total / 60, total % 60);
    return buffer;
}

/// "12.5K Followers" / "1 Follower" text inside [from, to), "" when absent.
std::string followersTextNear(const std::string& html, size_t from, size_t to) {
    const size_t word = html.find("Follower", from);
    if (word == NPOS || word >= to) return {};
    const size_t wordEnd = word + 8 + (word + 8 < html.size() && html[word + 8] == 's' ? 1 : 0);
    const size_t open = html.rfind('>', word);
    if (open == NPOS || open < from) return {};
    const std::string text = cleanText(html.substr(open + 1, wordEnd - open - 1));
    return parseCount(text) > 0 || text.rfind('0', 0) == 0 ? text : std::string();
}

std::string cssColor(const std::string& value) {
    const std::string v = lowerAscii(trim(value));
    if (v == "white") return "#ffffff";
    if (v == "black") return "#000000";
    if (v.size() == 7 && v.front() == '#') return v;
    if (v.size() == 4 && v.front() == '#') {
        return std::string("#") + v[1] + v[1] + v[2] + v[2] + v[3] + v[3];
    }
    return {};
}

}  // namespace

// ============================================================= text

std::string trim(std::string value) {
    size_t start = 0;
    while (start < value.size() && isSpace(value[start])) ++start;
    size_t end = value.size();
    while (end > start && isSpace(value[end - 1])) --end;
    return value.substr(start, end - start);
}

std::string decodeEntities(const std::string& text) {
    if (text.find('&') == NPOS) return text;
    static const std::unordered_map<std::string, uint32_t> NAMED = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
        {"bull", 0x2022}, {"hellip", 0x2026}, {"ndash", 0x2013}, {"mdash", 0x2014},
        {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D},
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"euro", 0x20AC}, {"pound", 0xA3},
        {"yen", 0xA5}, {"cent", 0xA2}, {"times", 0xD7}, {"middot", 0xB7}, {"laquo", 0xAB},
        {"raquo", 0xBB}, {"deg", 0xB0}, {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1},
        {"agrave", 0xE0}, {"iacute", 0xED}, {"oacute", 0xF3}, {"uacute", 0xFA}, {"ntilde", 0xF1},
        {"uuml", 0xFC}, {"ouml", 0xF6}, {"auml", 0xE4}, {"ccedil", 0xE7}, {"szlig", 0xDF},
    };

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out.push_back(text[i]);
            continue;
        }
        const size_t semi = text.find(';', i + 1);
        if (semi == NPOS || semi - i > 12) {
            out.push_back('&');
            continue;
        }
        const std::string entity = text.substr(i + 1, semi - i - 1);
        bool decoded = false;
        if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const std::string digits = entity.substr(hex ? 2 : 1);
            const bool valid = !digits.empty() && std::all_of(digits.begin(), digits.end(), [hex](unsigned char c) {
                return hex ? std::isxdigit(c) : std::isdigit(c);
            });
            if (valid && digits.size() <= 7) {
                appendUtf8(out, static_cast<uint32_t>(std::stoul(digits, nullptr, hex ? 16 : 10)));
                decoded = true;
            }
        } else {
            const auto found = NAMED.find(entity);
            if (found != NAMED.end()) {
                appendUtf8(out, found->second);
                decoded = true;
            }
        }
        if (decoded) {
            i = semi;
        } else {
            out.push_back('&');
        }
    }
    return out;
}

std::string stripTags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            out.push_back(html[i++]);
            continue;
        }
        const size_t gt = tagEnd(html, i);
        if (gt == NPOS) break;
        const std::string tag = html.substr(i, gt - i + 1);
        const std::string name = tagName(tag.size() > 1 && tag[1] == '/' ? "<" + tag.substr(2) : tag);
        if (name == "script" || name == "style" || name == "svg") {
            if (tag.size() > 1 && tag[1] != '/' && tag[tag.size() - 2] != '/') {
                const size_t close = html.find("</" + name, gt + 1);
                if (close != NPOS) {
                    const size_t closeEnd = html.find('>', close);
                    i = closeEnd == NPOS ? html.size() : closeEnd + 1;
                    continue;
                }
            }
        } else if (name == "br" || name == "p" || name == "div" || name == "li" || name == "h1" ||
                   name == "h2" || name == "h3") {
            out.push_back('\n');
        }
        i = gt + 1;
    }
    return out;
}

std::string cleanText(const std::string& html) {
    const std::string text = decodeEntities(stripTags(html));
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (char c : text) {
        if (isSpace(c)) {
            space = !out.empty();
        } else {
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c);
        }
    }
    return out;
}

std::string cleanMultiline(const std::string& html) {
    const std::string text = decodeEntities(stripTags(html));
    std::string out;
    std::string line;
    int blankLines = 0;
    auto flush = [&]() {
        const std::string value = cleanText(line);
        line.clear();
        if (value.empty()) {
            if (!out.empty()) ++blankLines;
            return;
        }
        if (!out.empty()) out += blankLines > 0 ? "\n\n" : "\n";
        out += value;
        blankLines = 0;
    };
    for (char c : text) {
        if (c == '\n') {
            flush();
        } else {
            line.push_back(c);
        }
    }
    flush();
    return out;
}

// ============================================================= ids and urls

std::string toBase36(uint64_t value) {
    static constexpr char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (value == 0) return "0";
    std::string out;
    while (value > 0) {
        out.push_back(DIGITS[value % 36]);
        value /= 36;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string embedIdForVideo(const std::string& numericId) {
    if (numericId.empty() || numericId.size() > 19 ||
        !std::all_of(numericId.begin(), numericId.end(), [](unsigned char c) { return std::isdigit(c); }))
        return {};
    try {
        const uint64_t value = std::stoull(numericId);
        if (value == 0) return {};
        return "v" + toBase36(value);
    } catch (...) {
        return {};
    }
}

std::string videoIdFromEmbedId(const std::string& embedId) {
    if (embedId.size() < 2 || embedId.size() > 14 || (embedId[0] != 'v' && embedId[0] != 'V')) return {};
    uint64_t value = 0;
    for (size_t i = 1; i < embedId.size(); ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(embedId[i])));
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 10;
        else return {};
        if (value > (UINT64_MAX - static_cast<uint64_t>(digit)) / 36) return {};
        value = value * 36 + static_cast<uint64_t>(digit);
    }
    return value == 0 ? std::string() : std::to_string(value);
}

std::string sitePath(const std::string& href) {
    std::string value = trim(decodeEntities(href));
    if (value.empty()) return {};
    for (const char* prefix : {"https://rumble.com", "http://rumble.com", "https://www.rumble.com",
             "http://www.rumble.com", "//rumble.com", "//www.rumble.com"}) {
        const std::string p(prefix);
        if (value.size() >= p.size() && lowerAscii(value.substr(0, p.size())) == p) {
            value.erase(0, p.size());
            break;
        }
    }
    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) return {};
    const size_t query = value.find_first_of("?#");
    if (query != NPOS) value.erase(query);
    if (value.empty() || value.front() != '/') value.insert(value.begin(), '/');
    return value;
}

std::string channelPath(const std::string& href) {
    const std::string path = sitePath(href);
    for (const char* kind : {"/c/", "/user/"}) {
        const std::string k(kind);
        if (path.size() > k.size() && path.compare(0, k.size(), k) == 0) {
            std::string name = path.substr(k.size());
            const size_t slash = name.find('/');
            if (slash != NPOS) name.erase(slash);
            if (name.empty()) return {};
            return k.substr(1) + name;
        }
    }
    return {};
}

std::string absoluteUrl(const std::string& src) {
    std::string value = trim(src);
    if (value.empty() || value.rfind("data:", 0) == 0) return {};
    if (value.rfind("//", 0) == 0) return "https:" + value;
    if (value.front() == '/') return SITE + value;
    if (value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0) return value;
    return {};
}

// ============================================================= numbers

int64_t parseCount(const std::string& text) {
    const std::string value = decodeEntities(text);
    size_t i = 0;
    while (i < value.size() && !std::isdigit(static_cast<unsigned char>(value[i]))) ++i;
    if (i >= value.size()) return 0;

    std::string digits;
    bool decimal = false;
    for (; i < value.size(); ++i) {
        const char c = value[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits.push_back(c);
        } else if (c == ',' && !decimal && i + 1 < value.size() &&
                   std::isdigit(static_cast<unsigned char>(value[i + 1]))) {
            continue;
        } else if (c == '.' && !decimal && i + 1 < value.size() &&
                   std::isdigit(static_cast<unsigned char>(value[i + 1]))) {
            decimal = true;
            digits.push_back('.');
        } else {
            break;
        }
    }
    while (i < value.size() && isSpace(value[i])) ++i;
    double multiplier = 1.0;
    if (i < value.size()) {
        const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(value[i])));
        const bool wordEnds = i + 1 >= value.size() || !std::isalpha(static_cast<unsigned char>(value[i + 1]));
        if (wordEnds && suffix == 'K') multiplier = 1e3;
        else if (wordEnds && suffix == 'M') multiplier = 1e6;
        else if (wordEnds && suffix == 'B') multiplier = 1e9;
    }
    try {
        return static_cast<int64_t>(std::llround(std::stod(digits) * multiplier));
    } catch (...) {
        return 0;
    }
}

int64_t parseClock(const std::string& text) {
    const std::string value = trim(decodeEntities(text));
    if (value.empty()) return 0;
    int64_t total = 0;
    int parts = 0;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t colon = value.find(':', start);
        const std::string part = value.substr(start, colon == NPOS ? NPOS : colon - start);
        if (part.empty() || part.size() > 4 ||
            !std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); }))
            return 0;
        total = total * 60 + std::stoll(part);
        ++parts;
        if (colon == NPOS) break;
        start = colon + 1;
    }
    return parts >= 2 && parts <= 3 ? total : 0;
}

int64_t parseIsoTime(const std::string& iso) {
    const std::string value = trim(iso);
    if (value.size() < 19 || value[4] != '-' || value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
        value[13] != ':' || value[16] != ':')
        return 0;
    auto number = [&](size_t pos, size_t length, int64_t& out) {
        out = 0;
        for (size_t i = pos; i < pos + length; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
            out = out * 10 + (value[i] - '0');
        }
        return true;
    };
    int64_t year, month, day, hour, minute, second;
    if (!number(0, 4, year) || !number(5, 2, month) || !number(8, 2, day) || !number(11, 2, hour) ||
        !number(14, 2, minute) || !number(17, 2, second))
        return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return 0;

    int64_t offset = 0;
    size_t zone = 19;
    while (zone < value.size() && (value[zone] == '.' || std::isdigit(static_cast<unsigned char>(value[zone]))))
        ++zone;
    if (zone < value.size() && (value[zone] == '+' || value[zone] == '-') && zone + 3 <= value.size()) {
        int64_t zoneHours = 0, zoneMinutes = 0;
        if (!number(zone + 1, 2, zoneHours)) return 0;
        const size_t minutesAt = zone + 3 < value.size() && value[zone + 3] == ':' ? zone + 4 : zone + 3;
        if (minutesAt + 2 <= value.size()) number(minutesAt, 2, zoneMinutes);
        offset = (zoneHours * 3600 + zoneMinutes * 60) * (value[zone] == '-' ? -1 : 1);
    }

    // Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
    const int64_t y = year - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yearOfEra = y - era * 400;
    const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = era * 146097 + dayOfEra - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second - offset;
}

// ============================================================= pages

std::vector<Video> parseVideoListing(const std::string& html) {
    std::vector<Video> out;
    std::unordered_set<std::string> seen;
    parseGridJson(html, out, seen);
    parseVideostreamItems(html, out, seen);
    parseListingEntries(html, out, seen);
    return out;
}

std::vector<Category> parseCategories(const std::string& html) {
    std::vector<Category> out;
    std::unordered_set<std::string> seen;
    size_t pos = 0;
    while ((pos = html.find("<li class=\"category\"", pos)) != NPOS) {
        const size_t start = pos;
        size_t end = html.find("</li>", start);
        if (end == NPOS) end = std::min(html.size(), start + 4000);
        pos = start + 10;

        Category category;
        const std::string link = sitePath(attribute(tagWithToken(html, "category__link", start, end), "href"));
        const std::string prefix = "/category/";
        if (link.size() <= prefix.size() || link.compare(0, prefix.size(), prefix) != 0) continue;
        category.slug = link.substr(prefix.size());
        const size_t slash = category.slug.find('/');
        if (slash != NPOS) category.slug.erase(slash);
        if (category.slug.empty() || !seen.insert(category.slug).second) continue;

        const std::string image = tagWithToken(html, "category__image", start, end);
        if (tagName(image) == "img") category.imageUrl = absoluteUrl(attribute(image, "src"));
        category.name = innerTextWithToken(html, "category__title", start, end);
        if (category.name.empty()) category.name = category.slug;
        category.viewers = parseCount(innerTextWithToken(html, "category__views", start, end));
        out.push_back(std::move(category));
    }
    return out;
}

std::vector<Channel> parseChannelSearch(const std::string& html) {
    std::vector<Channel> out;
    std::unordered_set<std::string> seen;
    const auto avatars = backgroundImages(html, "i.user-image--img--id-");
    const std::string marker = "<article class=\"flex items-center justify-between space-x-4\">";
    size_t pos = 0;
    while ((pos = html.find(marker, pos)) != NPOS) {
        const size_t start = pos;
        size_t end = html.find("</article>", start);
        if (end == NPOS) end = std::min(html.size(), start + 8000);
        pos = start + marker.size();

        Channel channel;
        size_t link = start;
        while ((link = html.find("<a ", link)) != NPOS && link < end) {
            channel.path = channelPath(attribute(enclosingTag(html, link + 1), "href"));
            if (!channel.path.empty()) break;
            link += 3;
        }
        if (channel.path.empty() || !seen.insert(lowerAscii(channel.path)).second) continue;

        const size_t avatar = html.find("user-image--img--id-", start);
        if (avatar != NPOS && avatar < end) {
            const auto found = avatars.find(readToken(html, avatar + 20));
            if (found != avatars.end()) channel.avatarUrl = found->second;
        }

        channel.name = innerTextWithToken(html, "truncate", start, end);
        if (channel.name.empty()) channel.name = channel.path.substr(channel.path.find('/') + 1);
        channel.verified = html.find("verification-badge-icon", start) < end;
        channel.followersText = followersTextNear(html, start, end);
        channel.followers = parseCount(channel.followersText);

        const size_t paragraph = html.find("<p", start);
        if (paragraph != NPOS && paragraph < end) channel.description = cleanText(innerHtmlAt(html, paragraph + 1));

        channel.id = attribute(tagWithToken(html, "followed-channel", start, end), "data-id");
        out.push_back(std::move(channel));
    }
    return out;
}

Channel parseChannelHeader(const std::string& html, const std::string& path) {
    Channel channel;
    channel.path = path;

    const std::string banner = tagWithToken(html, "channel-header--backsplash-img", 0, html.size());
    if (tagName(banner) == "img") channel.bannerUrl = absoluteUrl(attribute(banner, "src"));

    const std::string avatar = tagWithToken(html, "channel-header--img", 0, html.size());
    if (tagName(avatar) == "img") {
        channel.avatarUrl = absoluteUrl(attribute(avatar, "src"));
        channel.name = trim(attribute(avatar, "alt"));
    }

    const size_t title = findToken(html, "channel-header--title", 0, html.size());
    if (title != NPOS) {
        const size_t blockEnd = std::min(html.size(), title + 3000);
        const size_t h1 = html.find("<h1", title);
        if (h1 != NPOS && h1 < blockEnd) {
            const std::string name = cleanText(innerHtmlAt(html, h1 + 1));
            if (!name.empty()) channel.name = name;
        }
        channel.followersText = followersTextNear(html, title, blockEnd);
        channel.followers = parseCount(channel.followersText);
    }
    channel.verified = findToken(html, "channel-header--verified", 0, html.size()) != NPOS;

    const std::string follow = tagWithToken(html, "followed-channel", 0, html.size());
    channel.id = attribute(follow, "data-id");
    if (channel.name.empty()) channel.name = trim(attribute(follow, "data-title"));
    if (channel.name.empty() && !path.empty()) channel.name = path.substr(path.find('/') + 1);
    return channel;
}

std::string parseChannelDescription(const std::string& html) {
    const size_t pos = findToken(html, "channel-about--description", 0, html.size());
    if (pos == NPOS) return {};
    const size_t open = html.rfind('<', pos);
    const size_t start = tagEnd(html, open);
    if (start == NPOS) return {};
    // The block is a heading followed by paragraphs and ends at its </div>.
    const size_t end = html.find("</div>", start);
    if (end == NPOS) return {};
    std::string body = html.substr(start + 1, end - start - 1);
    const size_t heading = body.find("</h1>");
    if (heading != NPOS) body.erase(0, heading + 5);
    return cleanMultiline(body);
}

bool hasNextPage(const std::string& html) {
    size_t pos = 0;
    while ((pos = html.find("<link", pos)) != NPOS) {
        const std::string tag = enclosingTag(html, pos + 1);
        pos += 5;
        if (lowerAscii(attribute(tag, "rel")) == "next" && !attribute(tag, "href").empty()) return true;
    }
    return false;
}

std::vector<Channel> parseFollowedChannels(const std::string& html) {
    std::vector<Channel> out;
    const size_t table = html.find("<table");
    if (table == NPOS) return out;
    const size_t end = html.find("</table>", table);
    if (end == NPOS) return out;

    std::unordered_set<std::string> seen;
    size_t pos = table;
    while ((pos = html.find("<a ", pos)) != NPOS && pos < end) {
        const std::string tag = enclosingTag(html, pos + 1);
        const std::string path = channelPath(attribute(tag, "href"));
        const size_t close = html.find("</a>", pos);
        pos += 3;
        if (path.empty() || !seen.insert(lowerAscii(path)).second) continue;

        Channel channel;
        channel.path = path;
        const size_t textStart = tagEnd(html, html.rfind('<', pos));
        if (close != NPOS && textStart != NPOS && textStart < close)
            channel.name = cleanText(html.substr(textStart + 1, close - textStart - 1));
        if (channel.name.empty()) channel.name = path.substr(path.find('/') + 1);
        out.push_back(std::move(channel));
    }
    return out;
}

// ============================================================= oEmbed

std::string embedIdFromOEmbed(const nlohmann::json& oembed) {
    const std::string html = jsonString(oembed, "html");
    const std::string marker = "rumble.com/embed/";
    const size_t pos = html.find(marker);
    if (pos == NPOS) return {};
    size_t end = pos + marker.size();
    while (end < html.size() && (std::isalnum(static_cast<unsigned char>(html[end])) || html[end] == '.')) ++end;
    std::string id = html.substr(pos + marker.size(), end - pos - marker.size());
    // Publisher embeds look like "ufe9n.v5pv5f"; the video id is the last part.
    const size_t dot = id.rfind('.');
    if (dot != NPOS) id = id.substr(dot + 1);
    return videoIdFromEmbedId(id).empty() ? std::string() : lowerAscii(id);
}

// ============================================================= embed API

EmbedVideo parseEmbed(const nlohmann::json& root) {
    EmbedVideo video;
    if (!root.is_object()) return video;

    video.videoId = jsonString(root, "vid");
    video.title = trim(decodeEntities(jsonString(root, "title")));
    const json& author = jsonChild(root, "author");
    video.channelName = trim(decodeEntities(jsonString(author, "name")));
    video.channelPath = channelPath(jsonString(author, "url"));
    video.pageUrl = sitePath(jsonString(root, "l"));
    video.publishedAt = jsonString(root, "pubDate");
    video.durationSeconds = std::max<int64_t>(0, jsonInt(root, "duration"));
    video.systemMessage = trim(jsonString(jsonChild(root, "sys"), "msg"));

    if (root.contains("live") && root["live"].is_number()) {
        video.live = static_cast<int>(jsonInt(root, "live"));
        video.hasLiveField = true;
    }
    video.dvr = jsonTruthy(root, "livestream_has_dvr");

    int64_t thumbnailWidth = -1;
    if (root.contains("t") && root["t"].is_array()) {
        for (const auto& thumb : root["t"]) {
            const std::string url = absoluteUrl(jsonString(thumb, "i"));
            const int64_t width = jsonInt(thumb, "w");
            if (!url.empty() && width > thumbnailWidth) {
                video.thumbnailUrl = url;
                thumbnailWidth = width;
            }
        }
    }
    if (video.thumbnailUrl.empty()) video.thumbnailUrl = absoluteUrl(jsonString(root, "i"));

    // "ua" maps each format to {height: {url, meta}} (or a list, empty for
    // live streams); the older "u" maps each format to a single {url, meta}.
    const json* formats = nullptr;
    if (root.contains("ua") && root["ua"].is_object() && !root["ua"].empty()) formats = &root["ua"];
    else if (root.contains("u") && root["u"].is_object()) formats = &root["u"];

    std::vector<Quality> mp4;
    std::vector<Quality> webm;
    if (formats) {
        for (auto it = formats->begin(); it != formats->end(); ++it) {
            const std::string type = it.key();
            std::vector<std::pair<std::string, const json*>> entries;
            if (it->is_object()) {
                if (it->contains("url")) {
                    entries.emplace_back("", &*it);
                } else {
                    for (auto entry = it->begin(); entry != it->end(); ++entry) {
                        if (entry->is_object()) entries.emplace_back(entry.key(), &*entry);
                    }
                }
            } else if (it->is_array()) {
                for (const auto& entry : *it) {
                    if (entry.is_object()) entries.emplace_back("", &entry);
                }
            }

            for (const auto& [key, entry] : entries) {
                const std::string url = jsonString(*entry, "url");
                if (url.rfind("http", 0) != 0) continue;
                const json& meta = jsonChild(*entry, "meta");

                if (type == "hls") {
                    if (video.hlsUrl.empty()) {
                        video.hlsUrl = url;
                        video.hlsLive = meta.is_object() && meta.contains("live") && meta["live"].is_boolean() &&
                                        meta["live"].get<bool>();
                    }
                    continue;
                }

                Quality quality;
                quality.url = url;
                quality.bandwidth = std::max<int64_t>(0, jsonInt(meta, "bitrate")) * 1000;
                // audio, tar and timeline renditions are not offered.
                if (type != "mp4" && type != "webm") continue;

                int64_t height = jsonInt(meta, "h");
                if (height <= 0 && !key.empty() &&
                    std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c); }))
                    height = std::stoll(key);
                if (height <= 0 || height > 8640) continue;
                quality.height = static_cast<int>(height);
                quality.id = std::to_string(height) + "p";
                quality.name = quality.id;
                (type == "mp4" ? mp4 : webm).push_back(std::move(quality));
            }
        }
    }

    auto& progressive = mp4.empty() ? webm : mp4;
    std::sort(progressive.begin(), progressive.end(), [](const Quality& a, const Quality& b) {
        if (a.height != b.height) return a.height > b.height;
        return a.bandwidth > b.bandwidth;
    });
    for (auto& quality : progressive) {
        const bool duplicate = std::any_of(video.progressive.begin(), video.progressive.end(),
            [&](const Quality& existing) { return existing.height == quality.height; });
        if (!duplicate) video.progressive.push_back(std::move(quality));
    }

    if (root.contains("cc")) {
        const json& cc = root["cc"];
        auto addTrack = [&](const std::string& key, const json& info) {
            if (!info.is_object()) return;
            SubtitleTrack track;
            track.url = absoluteUrl(jsonString(info, "path"));
            if (track.url.empty()) return;
            track.language = key.empty() ? jsonString(info, "lang") : key;
            track.name = trim(decodeEntities(jsonString(info, "language")));
            if (track.name.empty()) track.name = track.language.empty() ? "Subtitles" : track.language;
            video.subtitles.push_back(std::move(track));
        };
        if (cc.is_object()) {
            for (auto it = cc.begin(); it != cc.end(); ++it) addTrack(it.key(), *it);
        } else if (cc.is_array()) {
            for (const auto& info : cc) addTrack("", info);
        }
    }
    return video;
}

// ============================================================= chat

std::vector<ChatFragment> splitMentions(const std::string& text) {
    std::vector<ChatFragment> fragments;
    auto pushText = [&](const std::string& value) {
        if (value.empty()) return;
        if (!fragments.empty() && fragments.back().type == ChatFragmentType::Text) {
            fragments.back().text += value;
        } else {
            fragments.push_back({ChatFragmentType::Text, value});
        }
    };

    size_t cursor = 0;
    while (cursor < text.size()) {
        const size_t at = text.find('@', cursor);
        if (at == NPOS) {
            pushText(text.substr(cursor));
            break;
        }
        const bool validStart = at == 0 || isSpace(text[at - 1]);
        size_t end = at + 1;
        while (end < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_' || text[end] == '-' ||
                text[end] == '.'))
            ++end;
        while (end > at + 1 && text[end - 1] == '.') --end;  // sentence punctuation

        if (!validStart || end <= at + 1) {
            pushText(text.substr(cursor, at - cursor + 1));
            cursor = at + 1;
            continue;
        }
        pushText(text.substr(cursor, at - cursor));
        fragments.push_back({ChatFragmentType::Mention, text.substr(at, end - at)});
        cursor = end;
    }
    return fragments;
}

std::vector<ChatFragment> splitEmotes(
    std::vector<ChatFragment> fragments, const std::unordered_map<std::string, std::string>& emotes) {
    if (emotes.empty()) return fragments;
    std::vector<ChatFragment> result;
    result.reserve(fragments.size());
    auto pushText = [&](const std::string& value) {
        if (value.empty()) return;
        if (!result.empty() && result.back().type == ChatFragmentType::Text) {
            result.back().text += value;
        } else {
            result.push_back({ChatFragmentType::Text, value});
        }
    };

    for (auto& fragment : fragments) {
        if (fragment.type != ChatFragmentType::Text) {
            result.push_back(std::move(fragment));
            continue;
        }
        const std::string& text = fragment.text;
        size_t cursor = 0;
        size_t search = 0;
        while (search < text.size()) {
            const size_t open = text.find(':', search);
            if (open == NPOS) break;
            const size_t close = text.find(':', open + 1);
            if (close == NPOS) break;
            const auto found = emotes.find(text.substr(open + 1, close - open - 1));
            if (found == emotes.end()) {
                // The closing colon may open the next code (":) :r+w:").
                search = close;
                continue;
            }
            pushText(text.substr(cursor, open - cursor));
            result.push_back({ChatFragmentType::Emote, text.substr(open, close - open + 1), found->second});
            cursor = search = close + 1;
        }
        pushText(text.substr(cursor));
    }
    return result;
}

std::unordered_map<std::string, std::string> parseEmoteList(const json& root) {
    std::unordered_map<std::string, std::string> emotes;
    const json& items = jsonChild(jsonChild(root, "data"), "items");
    if (!items.is_array()) return emotes;
    for (const auto& item : items) {
        const json& list = jsonChild(item, "emotes");
        if (!list.is_array()) continue;
        for (const auto& emote : list) {
            const std::string name = jsonString(emote, "name");
            const std::string file = jsonString(emote, "file");
            if (name.empty() || file.rfind("https://", 0) != 0) continue;
            emotes.emplace(name, file);
        }
    }
    return emotes;
}

size_t chatLength(const std::string& utf8) {
    size_t units = 0;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t length = 1;
        if (c >= 0xF0) length = 4;
        else if (c >= 0xE0) length = 3;
        else if (c >= 0xC0) length = 2;
        units += length == 4 ? 2 : 1;
        i += length;
    }
    return units;
}

ChatEvent parseChatEvent(const std::string& data, ChatState& state) {
    ChatEvent event;
    const json root = json::parse(data, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return event;

    event.name = jsonString(root, "type");
    const json& body = jsonChild(root, "data");

    if (event.name == "delete_messages" || event.name == "delete_non_rant_messages") {
        event.type = ChatEvent::Type::Delete;
        if (body.contains("message_ids") && body["message_ids"].is_array()) {
            for (const auto& id : body["message_ids"]) {
                if (id.is_string()) event.deletedIds.push_back(id.get<std::string>());
                else if (id.is_number_unsigned()) event.deletedIds.push_back(std::to_string(id.get<unsigned long long>()));
                else if (id.is_number_integer()) event.deletedIds.push_back(std::to_string(id.get<long long>()));
            }
        }
        return event;
    }

    if (event.name != "init" && event.name != "messages") {
        event.type = ChatEvent::Type::Other;
        return event;
    }
    event.type = ChatEvent::Type::Messages;

    if (event.name == "init") {
        const json& config = jsonChild(body, "config");
        const int64_t maxLength = jsonInt(config, "message_length_max");
        if (maxLength > 0) state.messageLengthMax = static_cast<size_t>(maxLength);
        const json& rants = jsonChild(config, "rants");
        if (rants.contains("levels") && rants["levels"].is_array()) {
            state.rantLevels.clear();
            for (const auto& level : rants["levels"]) {
                RantLevel parsed;
                const auto price = level.is_object() ? level.find("price_dollars") : level.end();
                if (!level.is_object() || price == level.end() || !price->is_number()) continue;
                parsed.priceCents = static_cast<int>(std::llround(price->get<double>() * 100.0));
                const json& colors = jsonChild(level, "colors");
                parsed.background = cssColor(jsonString(colors, "main"));
                parsed.foreground = cssColor(jsonString(colors, "fg"));
                state.rantLevels.push_back(std::move(parsed));
            }
            std::sort(state.rantLevels.begin(), state.rantLevels.end(),
                [](const RantLevel& a, const RantLevel& b) { return a.priceCents < b.priceCents; });
        }
    }

    // Each event lists the users and channels its messages refer to; keep the
    // maps bounded over a long stream.
    if (state.users.size() > 4000) state.users.clear();
    if (state.channels.size() > 1000) state.channels.clear();
    if (body.contains("users") && body["users"].is_array()) {
        for (const auto& user : body["users"]) {
            const std::string id = jsonString(user, "id");
            if (id.empty()) continue;
            ChatUser parsed;
            parsed.username = trim(jsonString(user, "username"));
            parsed.color = cssColor(jsonString(user, "color"));
            state.users[id] = std::move(parsed);
        }
    }
    if (body.contains("channels") && body["channels"].is_array()) {
        for (const auto& channel : body["channels"]) {
            const std::string id = jsonString(channel, "id");
            const std::string name = trim(jsonString(channel, "username"));
            if (!id.empty() && !name.empty()) state.channels[id] = name;
        }
    }

    if (!body.contains("messages") || !body["messages"].is_array()) return event;
    for (const auto& item : body["messages"]) {
        if (!item.is_object()) continue;
        ChatMessage message;
        message.id = jsonString(item, "id");
        message.timestamp = utcClock(jsonString(item, "time"));

        const std::string userId = jsonString(item, "user_id");
        const std::string channelId = jsonString(item, "channel_id");
        const auto user = state.users.find(userId);
        const auto channel = channelId.empty() ? state.channels.end() : state.channels.find(channelId);
        if (channel != state.channels.end()) message.userName = channel->second;
        else if (user != state.users.end() && !user->second.username.empty()) message.userName = user->second.username;
        else message.userName = "viewer";
        message.color = user != state.users.end() && !user->second.color.empty() ? user->second.color : "#85c742";

        message.text = jsonString(item, "text");
        if (message.text.empty() && item.contains("blocks") && item["blocks"].is_array()) {
            for (const auto& block : item["blocks"]) message.text += jsonString(jsonChild(block, "data"), "text");
        }

        const json& rant = jsonChild(item, "rant");
        if (rant.is_object() && !rant.empty()) {
            message.rantCents = static_cast<int>(std::max<int64_t>(0, jsonInt(rant, "price_cents")));
            for (const auto& level : state.rantLevels) {
                if (level.priceCents <= message.rantCents) {
                    message.rantBackground = level.background;
                    message.rantForeground = level.foreground;
                }
            }
            if (message.rantBackground.empty()) message.rantBackground = "#4a90e2";
            if (message.rantForeground.empty()) message.rantForeground = "#ffffff";
        }

        if (message.text.empty()) {
            const json& gift = jsonChild(item, "gift_purchase_notification");
            if (gift.is_object() && !gift.empty()) {
                const int64_t gifts = std::max<int64_t>(1, jsonInt(gift, "total_gifts", 1));
                message.text = "gifted " + std::to_string(gifts) + (gifts == 1 ? " membership" : " memberships");
            } else if (jsonTruthy(item, "raid_notification")) {
                message.text = "is raiding this stream";
            }
        }
        if (message.text.empty() && message.rantCents == 0) continue;
        message.fragments = splitEmotes(splitMentions(message.text), state.emotes);
        event.messages.push_back(std::move(message));
    }
    return event;
}

}  // namespace rumble::parse
