#pragma once

#include "api/rumble_chat.hpp"
#include "utils/orientation.hpp"

#include <borealis.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

class RecyclingGrid;
class ComposerDraftPreview;
class RumbleKeyboard;

/// Bottom-sheet message composer drawn over the live stream: an on-screen
/// keyboard, an emoji picker and a Send button.
class RumbleChatComposer : public brls::Box {
public:
    explicit RumbleChatComposer(std::string videoId);
    ~RumbleChatComposer() override;

    bool isTranslucent() override { return true; }

    brls::View* getDefaultFocus() override;

private:
    // Closes this overlay when the display rotates, since its fixed-size
    // layout would otherwise be left covering the portrait keyboard.
    std::optional<brls::Event<rumblenx::portrait::DisplayOrientation>::Subscription> orientationSubscription;
    std::shared_ptr<bool> aliveToken = std::make_shared<bool>(true);

    enum class Tab {
        Recent = 0,
        All = 1,
    };

    void setEmojiPanelVisible(bool visible);
    void appendText(const std::string& value);
    void updateSendRoute();
    void insertEmoji(const rumble::Emoji& emoji);
    void backspaceDraft();
    void sendMessage();
    void setTab(Tab value);
    void refreshGrid();
    void updateDraft();
    void updateTabs();
    std::vector<rumble::Emoji> visibleEmoji() const;

    std::string videoId;
    std::string draft;
    std::vector<rumble::Emoji> recentEmoji;
    Tab tab = Tab::Recent;
    bool sending = false;
    bool emojiPanelVisible = false;

    brls::Label* statusLabel = nullptr;
    brls::Label* footerLabel = nullptr;
    RumbleKeyboard* keyboard = nullptr;
    brls::Box* emojiPanel = nullptr;
    ComposerDraftPreview* draftPreview = nullptr;
    brls::Label* counterLabel = nullptr;
    brls::Label* focusedEmojiLabel = nullptr;
    brls::Label* sendLabel = nullptr;
    brls::Box* draftButton = nullptr;
    brls::Box* sendButton = nullptr;
    brls::Box* recentTab = nullptr;
    brls::Box* allTab = nullptr;
    RecyclingGrid* grid = nullptr;
};

/// Draws a message draft over two wrapped lines (shared by the landscape and
/// portrait composers).
class ComposerDraftPreview : public brls::View {
public:
    explicit ComposerDraftPreview(std::string placeholder, float fontSize = 19.0f);

    void setDraft(const std::string& value);

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

private:
    std::string draft;
    std::string placeholder;
    float fontSize;
};

/// Removes the last UTF-8 code point of `text` (a whole emoji included).
void eraseLastCodepoint(std::string& text);
