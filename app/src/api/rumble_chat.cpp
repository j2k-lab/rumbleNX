#include "api/rumble_chat.hpp"

#include "api/http.hpp"
#include "api/rumble.hpp"
#include "api/rumble_auth.hpp"
#include "api/rumble_parse.hpp"
#include "api/rumble_settings.hpp"
#include "utils/config.hpp"
#include "utils/thread.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <mutex>
#include <random>
#include <stdexcept>
#include <unordered_set>

namespace rumble {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr const char* CHAT_API = "https://web7.rumble.com/chat/api/chat/";
constexpr const char* EMOTE_LIST = "https://rumble.com/service.php?name=emote.list&chat_id=";
constexpr size_t MAX_SEEN_IDS = 512;
constexpr size_t MAX_RECENT_EMOJI = 24;
constexpr size_t MAX_STREAM_BUFFER = 4 * 1024 * 1024;
constexpr auto IDLE_TIMEOUT = std::chrono::seconds(90);
constexpr auto SEND_COOLDOWN = std::chrono::seconds(3);

std::atomic_size_t messageLimit{200};

int intSetting(const json& root, const char* key, int fallback) {
    const auto it = root.find(key);
    return it != root.end() && it->is_number_integer() ? it->get<int>() : fallback;
}

template <typename Enum>
Enum enumSetting(const json& root, const char* key, Enum fallback, Enum last) {
    const int value = intSetting(root, key, static_cast<int>(fallback));
    return value >= 0 && value <= static_cast<int>(last) ? static_cast<Enum>(value) : fallback;
}

/// 43 random base64 characters, like Rumble's web client.
std::string makeRequestId() {
    static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::mutex mutex;
    static std::mt19937 generator{std::random_device{}()};
    std::lock_guard<std::mutex> lock(mutex);
    std::uniform_int_distribution<int> pick(0, 63);
    std::string id;
    id.reserve(43);
    for (int i = 0; i < 43; ++i) id.push_back(ALPHABET[pick(generator)]);
    return id;
}

std::string trimmed(const std::string& value) { return parse::trim(value); }

}  // namespace

// ================================================================= preferences

ChatPreferences loadChatPreferences() {
    const json root = settings::read(settings::PREFERENCES);
    ChatPreferences result;
    result.mode = enumSetting(root, "chat_mode", ChatMode::Overlay, ChatMode::Overlay);
    result.participation =
        enumSetting(root, "chat_participation", ChatParticipation::ReadOnly, ChatParticipation::Interactive);
    result.composerMode = enumSetting(
        root, "chat_composer_mode", ChatComposerMode::RumbleNXComposer, ChatComposerMode::RumbleNXComposer);
    result.overlaySize = enumSetting(root, "chat_overlay_size", ChatOverlaySize::FullHeight, ChatOverlaySize::Compact);
    result.overlayPosition =
        enumSetting(root, "chat_overlay_position", ChatOverlayPosition::TopRight, ChatOverlayPosition::BottomLeft);
    result.fontSize = std::clamp(intSetting(root, "chat_font_size", 16), 12, 22);
    result.opacity = std::clamp(intSetting(root, "chat_opacity", 40), 20, 100);
    result.dockedWidth = std::clamp(intSetting(root, "chat_docked_width", 330), 280, 420);
    result.overlayWidth = std::clamp(intSetting(root, "chat_overlay_width", 360), 280, 480);
    const auto timestamps = root.find("chat_timestamps");
    result.timestamps = timestamps != root.end() && timestamps->is_boolean() && timestamps->get<bool>();
    return result;
}

bool saveChatPreferences(const ChatPreferences& preferences) {
    const bool saved = settings::update(settings::PREFERENCES, [&](json& root) {
        root["chat_mode"] = static_cast<int>(preferences.mode);
        root["chat_participation"] = static_cast<int>(preferences.participation);
        root["chat_composer_mode"] = static_cast<int>(preferences.composerMode);
        root["chat_overlay_size"] = static_cast<int>(preferences.overlaySize);
        root["chat_overlay_position"] = static_cast<int>(preferences.overlayPosition);
        root["chat_font_size"] = preferences.fontSize;
        root["chat_opacity"] = preferences.opacity;
        root["chat_docked_width"] = preferences.dockedWidth;
        root["chat_overlay_width"] = preferences.overlayWidth;
        root["chat_timestamps"] = preferences.timestamps;
    });
    if (saved) chatPreferencesEvent()->fire(preferences);
    return saved;
}

brls::Event<ChatPreferences>* chatPreferencesEvent() {
    static brls::Event<ChatPreferences> event;
    return &event;
}

size_t chatMessageLimit() { return messageLimit.load(); }

size_t chatMessageLength(const std::string& message) { return parse::chatLength(message); }

bool canSendChat() { return loadSession().signedIn(); }

// ================================================================= emoji

const std::vector<Emoji>& emojiCatalogue() {
    // Single code points only: no variation selectors or joiner sequences,
    // which the font renderer would draw as separate glyphs.
    static const std::vector<Emoji> emoji = {
        {"😀", "grinning"}, {"😁", "beaming"}, {"😂", "tears of joy"}, {"🤣", "rolling on the floor"},
        {"😅", "sweat smile"}, {"😆", "laughing"}, {"😉", "wink"}, {"😊", "blush"},
        {"😍", "heart eyes"}, {"😘", "blowing a kiss"}, {"😎", "cool"}, {"🤔", "thinking"},
        {"🤨", "raised eyebrow"}, {"😐", "neutral"}, {"🙄", "eye roll"}, {"😮", "surprised"},
        {"😢", "crying"}, {"😭", "sobbing"}, {"😡", "angry"}, {"🤯", "mind blown"},
        {"😱", "screaming"}, {"🥳", "partying"}, {"😴", "sleeping"}, {"🤪", "zany"},
        {"😇", "halo"}, {"🙃", "upside down"}, {"🙂", "slight smile"}, {"🤗", "hugging"},
        {"🤭", "hand over mouth"}, {"🤫", "shushing"}, {"🥱", "yawning"}, {"🥴", "woozy"},
        {"🥵", "hot"}, {"🥶", "cold"}, {"😈", "devil"}, {"💀", "skull"},
        {"👽", "alien"}, {"🤖", "robot"}, {"💩", "poop"}, {"🤡", "clown"},
        {"👻", "ghost"}, {"🙈", "see no evil"}, {"🙉", "hear no evil"}, {"🙊", "speak no evil"},
        {"👍", "thumbs up"}, {"👎", "thumbs down"}, {"👏", "clapping"}, {"🙌", "raised hands"},
        {"🙏", "folded hands"}, {"💪", "flexed biceps"}, {"👋", "waving"}, {"👌", "OK hand"},
        {"✌", "victory"}, {"🤞", "fingers crossed"}, {"🤘", "rock on"}, {"🤙", "call me"},
        {"👉", "pointing right"}, {"👈", "pointing left"}, {"👆", "pointing up"}, {"👇", "pointing down"},
        {"☝", "index up"}, {"✋", "raised hand"}, {"🤝", "handshake"}, {"👀", "eyes"},
        {"🧠", "brain"}, {"🔥", "fire"}, {"💯", "hundred points"}, {"❤", "red heart"},
        {"💔", "broken heart"}, {"💙", "blue heart"}, {"💚", "green heart"}, {"💛", "yellow heart"},
        {"🧡", "orange heart"}, {"💜", "purple heart"}, {"🖤", "black heart"}, {"⭐", "star"},
        {"🌟", "glowing star"}, {"✨", "sparkles"}, {"🎉", "party popper"}, {"🎊", "confetti"},
        {"🏆", "trophy"}, {"🥇", "gold medal"}, {"🎮", "video game"}, {"🎵", "musical note"},
        {"🎶", "musical notes"}, {"🎤", "microphone"}, {"🎧", "headphones"}, {"📺", "television"},
        {"🍿", "popcorn"}, {"🍕", "pizza"}, {"🍔", "burger"}, {"🍺", "beer"},
        {"🍻", "cheers"}, {"☕", "coffee"}, {"🚀", "rocket"}, {"💰", "money bag"},
        {"💸", "money with wings"}, {"💵", "dollar"}, {"💎", "gem"}, {"👑", "crown"},
        {"🚨", "siren"}, {"⚠", "warning"}, {"✅", "check mark"}, {"❌", "cross mark"},
        {"❓", "question"}, {"❗", "exclamation"}, {"💥", "boom"}, {"💤", "zzz"},
        {"💬", "speech balloon"}, {"📢", "loudspeaker"}, {"🔔", "bell"}, {"🦅", "eagle"},
        {"🐻", "bear"}, {"🐶", "dog"}, {"🐱", "cat"}, {"🐸", "frog"},
        {"🦍", "gorilla"}, {"🐍", "snake"}, {"🌈", "rainbow"}, {"☀", "sun"},
        {"🌙", "moon"}, {"⚡", "lightning"}, {"❄", "snowflake"}, {"🌍", "globe"},
        {"🏁", "chequered flag"}, {"🚩", "red flag"}, {"⏰", "alarm clock"}, {"⌛", "hourglass"},
        {"📈", "chart up"}, {"📉", "chart down"}, {"💣", "bomb"}, {"☠", "skull and crossbones"},
        {"🎃", "jack-o-lantern"}, {"🎄", "Christmas tree"}, {"🎁", "gift"}, {"🙋", "raising hand"},
    };
    return emoji;
}

std::vector<Emoji> loadRecentEmoji() {
    const json root = settings::read(settings::PREFERENCES);
    std::vector<Emoji> result;
    const auto list = root.find("chat_recent_emoji");
    if (list == root.end() || !list->is_array()) return result;
    const auto& catalogue = emojiCatalogue();
    for (const auto& value : *list) {
        if (!value.is_string()) continue;
        const std::string glyph = value.get<std::string>();
        const auto found = std::find_if(catalogue.begin(), catalogue.end(),
            [&](const Emoji& emoji) { return emoji.glyph == glyph; });
        if (found != catalogue.end()) result.push_back(*found);
        if (result.size() >= MAX_RECENT_EMOJI) break;
    }
    return result;
}

void rememberRecentEmoji(const Emoji& emoji) {
    if (emoji.glyph.empty()) return;
    settings::update(settings::PREFERENCES, [&](json& root) {
        json saved = json::array({emoji.glyph});
        const auto list = root.find("chat_recent_emoji");
        if (list != root.end() && list->is_array()) {
            for (const auto& value : *list) {
                if (!value.is_string() || value.get<std::string>() == emoji.glyph) continue;
                saved.push_back(value);
                if (saved.size() >= MAX_RECENT_EMOJI) break;
            }
        }
        root["chat_recent_emoji"] = std::move(saved);
    });
}

// ================================================================= sending

void sendChatMessageAsync(
    const std::string& videoId,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure) {
    ThreadPool::instance().submit([videoId, message, success = std::move(success),
                                      failure = std::move(failure)](HTTP&) mutable {
        static std::mutex sendMutex;
        static Clock::time_point lastSent{};
        try {
            const std::string text = trimmed(message);
            if (videoId.empty()) throw std::runtime_error("No live chat is open.");
            if (text.empty()) throw std::runtime_error("Type a message first.");
            if (chatMessageLength(text) > chatMessageLimit())
                throw std::runtime_error(
                    "Rumble chat messages are limited to " + std::to_string(chatMessageLimit()) + " characters.");
            const Session session = loadSession();
            if (!session.signedIn()) throw std::runtime_error("Sign in to Rumble to chat.");
            {
                std::lock_guard<std::mutex> lock(sendMutex);
                const auto now = Clock::now();
                if (lastSent.time_since_epoch().count() != 0 && now - lastSent < SEND_COOLDOWN)
                    throw std::runtime_error("Wait a moment before sending another message.");
                lastSent = now;
            }

            const json body = {
                {"data",
                    {
                        {"request_id", makeRequestId()},
                        {"message", {{"text", text}}},
                        {"rant", nullptr},
                        {"channel_id", nullptr},
                    }},
            };
            const HTTP::Header headers = {
                "Content-Type: application/json",
                "Accept: application/json, text/plain, */*",
                "Cookie: u_s=" + session.token,
            };

            std::string response;
            try {
                response = HTTP::post(std::string(CHAT_API) + percentEncode(videoId) + "/message", body.dump(),
                    headers, HTTP::Timeout{15000});
            } catch (const std::exception& error) {
                const std::string what = error.what();
                if (what.find("http status 401") != std::string::npos ||
                    what.find("http status 403") != std::string::npos)
                    throw std::runtime_error(
                        "Rumble refused the message. You may be muted, the chat may be restricted, or your "
                        "session expired (sign out and sign in again).");
                if (what.find("http status 429") != std::string::npos)
                    throw std::runtime_error("You are sending messages too quickly.");
                throw;
            }

            const json root = json::parse(response, nullptr, false);
            std::string id;
            if (!root.is_discarded() && root.is_object() && root.contains("data") && root["data"].is_object()) {
                const json& data = root["data"];
                if (data.contains("id") && data["id"].is_string()) id = data["id"].get<std::string>();
                else if (data.contains("id") && data["id"].is_number()) id = data["id"].dump();
            }
            brls::sync(std::bind(std::move(success), id));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        }
    });
}

// ================================================================= live chat

struct ChatClient::StreamState {
    parse::ChatState chat;
    std::deque<std::string> seenOrder;
    std::unordered_set<std::string> seen;

    bool remember(const std::string& id) {
        if (id.empty()) return true;
        if (!seen.insert(id).second) return false;
        seenOrder.push_back(id);
        while (seenOrder.size() > MAX_SEEN_IDS) {
            seen.erase(seenOrder.front());
            seenOrder.pop_front();
        }
        return true;
    }
};

namespace {

struct StreamContext {
    std::function<void(const std::string&)> onEvent;
    std::string buffer;
    std::string data;
    Clock::time_point lastByte = Clock::now();
    bool overflow = false;
};

size_t onStreamData(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* context = static_cast<StreamContext*>(userdata);
    const size_t length = size * nmemb;
    context->lastByte = Clock::now();
    context->buffer.append(ptr, length);
    if (context->buffer.size() > MAX_STREAM_BUFFER) {
        context->overflow = true;
        return 0;
    }

    size_t newline;
    while ((newline = context->buffer.find('\n')) != std::string::npos) {
        std::string line = context->buffer.substr(0, newline);
        context->buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line.empty()) {
            // A blank line ends an event.
            if (!context->data.empty()) {
                context->onEvent(context->data);
                context->data.clear();
            }
        } else if (line.rfind("data:", 0) == 0) {
            std::string value = line.substr(5);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
            if (!context->data.empty()) context->data.push_back('\n');
            context->data += value;
        }
        // ":" comments (keep-alives), "event:", "id:" and "retry:" lines are not used by Rumble.
    }
    return length;
}

}  // namespace

ChatClient::ChatClient(
    std::string videoId,
    ChatMessageCallback message,
    ChatStatusCallback status,
    ChatDeleteCallback deleted)
    : videoId(std::move(videoId)),
      messageCallback(std::move(message)),
      statusCallback(std::move(status)),
      deleteCallback(std::move(deleted)),
      streamState(std::make_unique<StreamState>()) {
#ifdef BOREALIS_USE_STD_THREAD
    thread = std::make_shared<std::thread>(&ChatClient::threadEntry, this);
#else
    if (pthread_create(&thread, nullptr, &ChatClient::threadEntry, this) == 0) threadStarted = true;
#endif
}

ChatClient::~ChatClient() { stop(); }

void ChatClient::stop() {
    if (stopRequested.exchange(true)) return;
#ifdef BOREALIS_USE_STD_THREAD
    if (thread && thread->joinable()) thread->join();
    thread.reset();
#else
    if (threadStarted) {
        pthread_join(thread, nullptr);
        threadStarted = false;
    }
#endif
}

void* ChatClient::threadEntry(void* context) {
    static_cast<ChatClient*>(context)->run();
    return nullptr;
}

void ChatClient::run() {
    int retrySeconds = 1;
    while (!stopRequested.load()) {
        if (statusCallback) statusCallback("Connecting to Rumble chat…");
        // Emotes are optional: without the list, codes stay as text. Retried
        // on each reconnect until one load succeeds.
        if (streamState->chat.emotes.empty()) {
            try {
                const json root = json::parse(
                    HTTP::get(EMOTE_LIST + percentEncode(videoId), HTTP::Timeout{10000}), nullptr, false);
                if (!root.is_discarded()) streamState->chat.emotes = parse::parseEmoteList(root);
                brls::Logger::info("RumbleNX: loaded {} chat emotes", streamState->chat.emotes.size());
            } catch (const std::exception& exception) {
                brls::Logger::warning("RumbleNX: chat emotes unavailable: {}", exception.what());
            }
        }
        bool receivedEvents = false;
        std::string reason;
        try {
            reason = streamOnce(receivedEvents);
        } catch (const std::exception& exception) {
            reason = exception.what();
        } catch (...) {
            reason = "unknown error";
        }
        if (stopRequested.load()) break;
        if (receivedEvents) retrySeconds = 1;
        if (statusCallback) statusCallback("Chat reconnecting: " + reason);

        for (int tick = 0; tick < retrySeconds * 5 && !stopRequested.load(); ++tick)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        retrySeconds = std::min(retrySeconds * 2, 20);
    }
}

void ChatClient::dispatch(const std::string& data, bool& receivedEvents) {
    const parse::ChatEvent event = parse::parseChatEvent(data, streamState->chat);
    messageLimit.store(streamState->chat.messageLengthMax);
    if (event.type == parse::ChatEvent::Type::Messages) {
        if (!receivedEvents && statusCallback) statusCallback("Live chat");
        receivedEvents = true;
        for (const auto& message : event.messages) {
            if (!streamState->remember(message.id)) continue;
            if (messageCallback) messageCallback(message);
        }
    } else if (event.type == parse::ChatEvent::Type::Delete) {
        if (deleteCallback && !event.deletedIds.empty()) deleteCallback(event.deletedIds);
    }
}

std::string ChatClient::streamOnce(bool& receivedEvents) {
    CURL* easy = curl_easy_init();
    if (!easy) return "could not start a network request";
    CURLM* multi = curl_multi_init();
    if (!multi) {
        curl_easy_cleanup(easy);
        return "could not start a network request";
    }

    StreamContext context;
    context.onEvent = [this, &receivedEvents](const std::string& data) { dispatch(data, receivedEvents); };

    const std::string url = std::string(CHAT_API) + percentEncode(videoId) + "/stream";
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");

    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, AppVersion::getUserAgent().c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onStreamData);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(easy, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_multi_add_handle(multi, easy);

    std::string reason;
    int running = 1;
    while (running && !stopRequested.load()) {
        if (curl_multi_perform(multi, &running) != CURLM_OK) {
            reason = "network error";
            break;
        }
        if (!running) break;
        if (Clock::now() - context.lastByte > IDLE_TIMEOUT) {
            reason = "no data for 90 seconds";
            break;
        }
        int ready = 0;
        if (curl_multi_wait(multi, nullptr, 0, 200, &ready) != CURLM_OK) {
            reason = "network error";
            break;
        }
        // Nothing to wait on yet (e.g. while resolving): avoid spinning.
        if (ready == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CURLcode result = CURLE_OK;
    int queued = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &queued)) {
        if (message->msg == CURLMSG_DONE) result = message->data.result;
    }
    long status = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);

    curl_multi_remove_handle(multi, easy);
    curl_multi_cleanup(multi);
    curl_easy_cleanup(easy);
    curl_slist_free_all(headers);

    if (stopRequested.load()) return "stopped";
    if (!reason.empty()) return reason;
    if (context.overflow) return "the chat stream sent too much data";
    if (status >= 400) return "HTTP " + std::to_string(status);
    if (status == 204) return "chat is not available for this stream";
    if (result != CURLE_OK) return curl_easy_strerror(result);
    return "the chat stream closed";
}

}  // namespace rumble
