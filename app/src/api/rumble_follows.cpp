#include "api/rumble_follows.hpp"

#include "api/rumble.hpp"
#include "api/rumble_auth.hpp"
#include "api/rumble_parse.hpp"
#include "api/rumble_settings.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <stdexcept>

namespace rumble::follows {
namespace {

using json = nlohmann::json;

constexpr size_t MAX_FOLLOWS = 500;

std::atomic_bool changed{false};

std::string key(const std::string& path) {
    std::string value = path;
    for (char& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string text(const json& obj, const char* name) {
    const auto it = obj.find(name);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::vector<Channel> fromJson(const json& root) {
    std::vector<Channel> result;
    const auto list = root.find("channels");
    if (list == root.end() || !list->is_array()) return result;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        Channel channel;
        channel.path = parse::channelPath("/" + text(item, "path"));
        if (channel.path.empty()) continue;
        channel.name = text(item, "name");
        channel.avatarUrl = text(item, "avatar_url");
        if (channel.name.empty()) channel.name = channel.path.substr(channel.path.find('/') + 1);
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const Channel& existing) { return key(existing.path) == key(channel.path); });
        if (!duplicate) result.push_back(std::move(channel));
    }
    return result;
}

json toJson(const std::vector<Channel>& channels) {
    json list = json::array();
    for (const auto& channel : channels) {
        list.push_back({{"path", channel.path}, {"name", channel.name}, {"avatar_url", channel.avatarUrl}});
    }
    return json{{"channels", list}};
}

}  // namespace

std::vector<Channel> load() { return fromJson(settings::read(settings::FOLLOWS)); }

bool isFollowing(const std::string& channelPath) {
    const std::string wanted = key(channelPath);
    if (wanted.empty()) return false;
    const auto channels = load();
    return std::any_of(channels.begin(), channels.end(),
        [&](const Channel& channel) { return key(channel.path) == wanted; });
}

bool follow(const Channel& channel) {
    if (channel.path.empty()) return false;
    const bool saved = settings::update(settings::FOLLOWS, [&](json& root) {
        auto channels = fromJson(root);
        channels.erase(std::remove_if(channels.begin(), channels.end(),
                           [&](const Channel& existing) { return key(existing.path) == key(channel.path); }),
            channels.end());
        Channel entry;
        entry.path = channel.path;
        entry.name = channel.name.empty() ? channel.path.substr(channel.path.find('/') + 1) : channel.name;
        entry.avatarUrl = channel.avatarUrl;
        channels.insert(channels.begin(), std::move(entry));
        if (channels.size() > MAX_FOLLOWS) channels.resize(MAX_FOLLOWS);
        root = toJson(channels);
    });
    if (saved) changed.store(true);
    return saved;
}

bool unfollow(const std::string& channelPath) {
    const bool saved = settings::update(settings::FOLLOWS, [&](json& root) {
        auto channels = fromJson(root);
        channels.erase(std::remove_if(channels.begin(), channels.end(),
                           [&](const Channel& existing) { return key(existing.path) == key(channelPath); }),
            channels.end());
        root = toJson(channels);
    });
    if (saved) changed.store(true);
    return saved;
}

void refreshDetails(const std::vector<Channel>& updates) {
    if (updates.empty()) return;
    settings::update(settings::FOLLOWS, [&](json& root) {
        auto channels = fromJson(root);
        bool touched = false;
        for (auto& channel : channels) {
            for (const auto& update : updates) {
                if (key(update.path) != key(channel.path)) continue;
                if (!update.name.empty() && update.name != channel.name) {
                    channel.name = update.name;
                    touched = true;
                }
                if (!update.avatarUrl.empty() && update.avatarUrl != channel.avatarUrl) {
                    channel.avatarUrl = update.avatarUrl;
                    touched = true;
                }
            }
        }
        if (touched) root = toJson(channels);
    });
}

bool consumeChanged() { return changed.exchange(false); }

void importFromAccountAsync(std::function<void(int added, int found)> success, ErrorCallback failure) {
    ThreadPool::instance().submit([success = std::move(success), failure = std::move(failure)](HTTP&) mutable {
        try {
            if (!loadSession().signedIn()) throw std::runtime_error("Sign in to import the channels you follow.");
            const std::string html = fetchPage("/account/channel/subscriptions", nullptr, true);
            const auto found = parse::parseFollowedChannels(html);
            if (found.empty())
                throw std::runtime_error(
                    "No followed channels were found on your Rumble account page. "
                    "You can still follow channels from their pages.");

            int added = 0;
            // Oldest first, so the account's first channel ends up on top.
            for (auto it = found.rbegin(); it != found.rend(); ++it) {
                if (isFollowing(it->path)) continue;
                if (follow(*it)) ++added;
            }
            brls::sync([success = std::move(success), added, total = static_cast<int>(found.size())]() {
                if (success) success(added, total);
            });
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        }
    });
}

}  // namespace rumble::follows
