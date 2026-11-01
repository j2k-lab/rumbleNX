#pragma once

#include "api/rumble_chat_types.hpp"
#include "api/rumble_types.hpp"

#include <borealis.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif

namespace rumble {

enum class ChatMode {
    Off = 0,
    RightPanel = 1,
    Overlay = 2,
};

enum class ChatParticipation {
    ReadOnly = 0,
    Interactive = 1,
};

enum class ChatComposerMode {
    NintendoKeyboard = 0,
    RumbleNXComposer = 1,
};

enum class ChatOverlaySize {
    FullHeight = 0,
    Compact = 1,
};

enum class ChatOverlayPosition {
    TopRight = 0,
    TopLeft = 1,
    BottomRight = 2,
    BottomLeft = 3,
};

struct ChatPreferences {
    ChatMode mode = ChatMode::Overlay;
    ChatParticipation participation = ChatParticipation::ReadOnly;
    ChatComposerMode composerMode = ChatComposerMode::RumbleNXComposer;
    ChatOverlaySize overlaySize = ChatOverlaySize::FullHeight;
    ChatOverlayPosition overlayPosition = ChatOverlayPosition::TopRight;
    int fontSize = 16;
    int opacity = 40;
    int dockedWidth = 330;
    int overlayWidth = 360;
    bool timestamps = false;
};

ChatPreferences loadChatPreferences();
bool saveChatPreferences(const ChatPreferences& preferences);
brls::Event<ChatPreferences>* chatPreferencesEvent();

/// Rumble's limit for one chat message, in UTF-16 code units (the way its
/// web client counts). Updated from each chat's configuration.
size_t chatMessageLimit();
size_t chatMessageLength(const std::string& message);

/// Sending needs a signed-in session.
bool canSendChat();

// ---------------------------------------------------------------- emoji

/// The composer's picker offers Unicode emoji that the bundled Noto Emoji
/// font can draw. Rumble emotes (":r+rumblerocket:") in received messages are
/// drawn as images from the chat's emote.list.
struct Emoji {
    std::string glyph;
    std::string name;
};

const std::vector<Emoji>& emojiCatalogue();
std::vector<Emoji> loadRecentEmoji();
void rememberRecentEmoji(const Emoji& emoji);

// ---------------------------------------------------------------- live chat

using ChatMessageCallback = std::function<void(ChatMessage)>;
using ChatStatusCallback = std::function<void(const std::string&)>;
using ChatDeleteCallback = std::function<void(std::vector<std::string>)>;
using ChatSendCallback = std::function<void(const std::string& messageId)>;

void sendChatMessageAsync(
    const std::string& videoId,
    const std::string& message,
    ChatSendCallback success,
    ErrorCallback failure);

/// Reads a live stream's chat from Rumble's server-sent event stream on its
/// own thread, reconnecting with back-off. Callbacks run on that thread.
class ChatClient {
public:
    ChatClient(
        std::string videoId,
        ChatMessageCallback message,
        ChatStatusCallback status,
        ChatDeleteCallback deleted = nullptr);
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;

    void stop();

private:
    struct StreamState;

    static void* threadEntry(void* context);
    void run();
    /// One connection; returns why it ended.
    std::string streamOnce(bool& receivedEvents);
    void dispatch(const std::string& data, bool& receivedEvents);

    std::string videoId;
    ChatMessageCallback messageCallback;
    ChatStatusCallback statusCallback;
    ChatDeleteCallback deleteCallback;
    std::atomic_bool stopRequested{false};
    /// Users, rant levels and recently seen message ids; Rumble resends the
    /// chat history on every connection.
    std::unique_ptr<StreamState> streamState;

#ifdef BOREALIS_USE_STD_THREAD
    std::shared_ptr<std::thread> thread;
#else
    pthread_t thread{};
    bool threadStarted = false;
#endif
};

}  // namespace rumble
