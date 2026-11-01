#pragma once

#include <string>
#include <vector>

namespace rumble {

enum class ChatFragmentType {
    Text = 0,
    Mention = 1,
    /// A Rumble emote code (":r+rumblerocket:") with an image.
    Emote = 2,
};

struct ChatFragment {
    ChatFragmentType type = ChatFragmentType::Text;
    std::string text;
    /// Emote image (PNG, or GIF for animated emotes).
    std::string emoteUrl;
};

struct ChatMessage {
    std::string id;
    std::string userName;
    std::string text;
    /// "#rrggbb" username colour.
    std::string color;
    /// "HH:MM" (UTC) the message was posted.
    std::string timestamp;
    std::vector<ChatFragment> fragments;
    /// Rants are paid, highlighted messages. 0 for a regular message.
    int rantCents = 0;
    /// "#rrggbb" rant highlight and text colours from the chat's rant levels.
    std::string rantBackground;
    std::string rantForeground;
};

}  // namespace rumble
