#include "view/rumble_chat_composer.hpp"

#include "api/rumble_parse.hpp"
#include "view/recycling_grid.hpp"
#include "view/rumble_keyboard.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>

// ------------------------------------------------------------- draft preview

ComposerDraftPreview::ComposerDraftPreview(std::string placeholder, float fontSize)
    : placeholder(std::move(placeholder)), fontSize(fontSize) {}

void ComposerDraftPreview::setDraft(const std::string& value) {
    draft = value;
    this->invalidate();
}

void ComposerDraftPreview::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style, brls::FrameContext*) {
    nvgSave(vg);
    nvgIntersectScissor(vg, x, y, width, height);

    const float lineHeight = fontSize * 1.5f;
    constexpr int MAX_LINES = 2;

    nvgFontFaceId(vg, brls::Application::getDefaultFont());
    nvgFontSize(vg, fontSize);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

    if (draft.empty()) {
        nvgFillColor(vg, nvgRGB(160, 170, 188));
        nvgText(vg, x, y + fontSize, placeholder.c_str(), nullptr);
        nvgRestore(vg);
        return;
    }

    auto textWidth = [&](const std::string& text) {
        float bounds[4]{};
        return nvgTextBounds(vg, 0, 0, text.c_str(), nullptr, bounds);
    };

    float cursorX = x;
    float cursorY = y + fontSize;
    int line = 0;
    const float right = x + width;
    nvgFillColor(vg, nvgRGB(245, 246, 250));

    // Words wrap as units; whitespace runs are drawn as they are.
    size_t position = 0;
    while (position < draft.size() && line < MAX_LINES) {
        const bool space = std::isspace(static_cast<unsigned char>(draft[position])) != 0;
        size_t end = position + 1;
        while (end < draft.size() &&
               (std::isspace(static_cast<unsigned char>(draft[end])) != 0) == space)
            ++end;
        const std::string token = draft.substr(position, end - position);
        position = end;

        const float tokenWidth = textWidth(token);
        if (!space && cursorX > x && cursorX + tokenWidth > right) {
            cursorX = x;
            cursorY += lineHeight;
            ++line;
            if (line >= MAX_LINES) break;
        }
        nvgText(vg, cursorX, cursorY, token.c_str(), nullptr);
        cursorX += tokenWidth;
    }
    nvgRestore(vg);
}

void eraseLastCodepoint(std::string& text) {
    if (text.empty()) return;
    size_t position = text.size() - 1;
    while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xC0) == 0x80) --position;
    text.erase(position);
}

namespace {

brls::Box* makeActionButton(const std::string& text, brls::Label** output = nullptr) {
    auto* button = new brls::Box();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(8, 16, 8, 16);
    button->setCornerRadius(8);
    button->setBackgroundColor(nvgRGBA(45, 50, 65, 170));
    button->setMarginRight(10);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(16);
    label->setTextColor(nvgRGB(245, 246, 250));
    button->addView(label);

    if (output) *output = label;
    return button;
}

class EmojiCell : public RecyclingGridItem {
public:
    EmojiCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setPadding(6, 6, 6, 6);
        this->setCornerRadius(8);
        this->setBackgroundColor(nvgRGBA(31, 35, 46, 150));

        glyph = new brls::Label();
        glyph->setFontSize(34);
        glyph->setSingleLine(true);
        glyph->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        glyph->setTextColor(nvgRGB(245, 246, 250));
        this->addView(glyph);

        this->getFocusEvent()->subscribe([this](brls::View*) {
            if (focused) focused(value);
        });
    }

    void prepareForReuse() override {
        glyph->setText("");
        value = {};
        focused = nullptr;
    }

    void cacheForReuse() override { focused = nullptr; }

    void configure(const rumble::Emoji& emoji, std::function<void(const rumble::Emoji&)> onFocus) {
        value = emoji;
        focused = std::move(onFocus);
        glyph->setText(value.glyph);
    }

private:
    brls::Label* glyph = nullptr;
    rumble::Emoji value;
    std::function<void(const rumble::Emoji&)> focused;
};

class EmojiDataSource : public RecyclingGridDataSource {
public:
    using Callback = std::function<void(const rumble::Emoji&)>;

    EmojiDataSource(std::vector<rumble::Emoji> values, Callback selected, Callback focused)
        : items(std::move(values)), selected(std::move(selected)), focused(std::move(focused)) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<EmojiCell*>(recycler->dequeueReusableCell("Emoji"));
        cell->configure(items.at(index), focused);
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (selected) selected(items.at(index));
    }

    void clearData() override { items.clear(); }

private:
    std::vector<rumble::Emoji> items;
    Callback selected;
    Callback focused;
};

const char* KEYBOARD_FOOTER = "A Type   Y Delete   X Emoji   ZR/+ Send   B Back";
const char* EMOJI_FOOTER = "A Insert   Y Delete   X Keyboard   L/R Tabs   ZR/+ Send   B Back";

}  // namespace

RumbleChatComposer::RumbleChatComposer(std::string videoId)
    : videoId(std::move(videoId)), recentEmoji(rumble::loadRecentEmoji()) {
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setJustifyContent(brls::JustifyContent::FLEX_END);
    this->setBackgroundColor(nvgRGBA(0, 0, 0, 0));

    // Translucent area above the sheet; tapping it closes the composer.
    auto* dismissArea = new brls::Box();
    dismissArea->setGrow(1.0f);
    dismissArea->setWidthPercentage(100.0f);
    dismissArea->setBackgroundColor(nvgRGBA(0, 0, 0, 90));
    dismissArea->addGestureRecognizer(new brls::TapGestureRecognizer(dismissArea));
    dismissArea->registerClickAction([](brls::View*) {
        brls::Application::popActivity();
        return true;
    });
    this->addView(dismissArea);

    auto* sheet = new brls::Box();
    sheet->setAxis(brls::Axis::COLUMN);
    sheet->setWidthPercentage(100.0f);
    sheet->setHeight(540.0f);
    sheet->setCornerRadius(16);
    sheet->setBackgroundColor(nvgRGBA(15, 17, 23, 90));
    sheet->setPadding(12, 32, 10, 32);
    this->addView(sheet);

    auto* handleBox = new brls::Box();
    handleBox->setAxis(brls::Axis::ROW);
    handleBox->setJustifyContent(brls::JustifyContent::CENTER);
    handleBox->setAlignItems(brls::AlignItems::CENTER);
    handleBox->setWidthPercentage(100.0f);
    handleBox->setMarginBottom(6);
    auto* handleBar = new brls::Box();
    handleBar->setWidth(48);
    handleBar->setHeight(4);
    handleBar->setCornerRadius(2);
    handleBar->setBackgroundColor(nvgRGBA(160, 170, 195, 140));
    handleBox->addView(handleBar);
    sheet->addView(handleBox);

    auto* headingRow = new brls::Box();
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(brls::AlignItems::CENTER);
    headingRow->setMarginBottom(8);
    sheet->addView(headingRow);

    auto* heading = new brls::Label();
    heading->setText("Rumble chat");
    heading->setFontSize(22);
    heading->setTextColor(nvgRGB(245, 246, 250));
    heading->setGrow(1.0f);
    headingRow->addView(heading);

    statusLabel = new brls::Label();
    statusLabel->setText("Messages post as your signed-in Rumble account");
    statusLabel->setFontSize(14);
    statusLabel->setTextColor(nvgRGB(160, 170, 190));
    headingRow->addView(statusLabel);

    auto* composerRow = new brls::Box();
    composerRow->setAxis(brls::Axis::ROW);
    composerRow->setWidthPercentage(100);
    composerRow->setHeight(92);
    composerRow->setAlignItems(brls::AlignItems::CENTER);
    composerRow->setMarginBottom(8);
    sheet->addView(composerRow);

    draftButton = new brls::Box();
    draftButton->setFocusable(true);
    draftButton->setAxis(brls::Axis::COLUMN);
    draftButton->setGrow(1.0f);
    draftButton->setHeight(92);
    draftButton->setPadding(8, 14, 6, 14);
    draftButton->setCornerRadius(8);
    draftButton->setBackgroundColor(nvgRGBA(29, 33, 43, 170));
    draftButton->setMarginRight(12);
    draftButton->addGestureRecognizer(new brls::TapGestureRecognizer(draftButton));
    draftButton->registerClickAction([this](brls::View*) {
        setEmojiPanelVisible(!emojiPanelVisible);
        return true;
    });
    composerRow->addView(draftButton);

    sendButton = makeActionButton("Send", &sendLabel);
    sendButton->setWidth(130);
    sendButton->setHeight(92);
    sendButton->setMarginRight(0);
    sendButton->setAlignItems(brls::AlignItems::CENTER);
    sendButton->setJustifyContent(brls::JustifyContent::CENTER);
    sendButton->setBackgroundColor(nvgRGB(72, 110, 33));
    sendButton->registerClickAction([this](brls::View*) {
        sendMessage();
        return true;
    });
    composerRow->addView(sendButton);

    draftButton->setCustomNavigationRoute(brls::FocusDirection::RIGHT, sendButton);
    sendButton->setCustomNavigationRoute(brls::FocusDirection::LEFT, draftButton);

    auto* draftHeader = new brls::Box();
    draftHeader->setAxis(brls::Axis::ROW);
    draftHeader->setMarginBottom(4);
    draftButton->addView(draftHeader);

    auto* draftCaption = new brls::Label();
    draftCaption->setText("Message draft");
    draftCaption->setFontSize(13);
    draftCaption->setTextColor(nvgRGB(145, 156, 177));
    draftCaption->setGrow(1.0f);
    draftHeader->addView(draftCaption);

    counterLabel = new brls::Label();
    counterLabel->setFontSize(13);
    counterLabel->setTextColor(nvgRGB(145, 156, 177));
    draftHeader->addView(counterLabel);

    draftPreview = new ComposerDraftPreview("Press A to type a message");
    draftPreview->setWidthPercentage(100);
    draftPreview->setHeight(54);
    draftButton->addView(draftPreview);

    // The Send button beside the draft sends, so the keyboard swaps its own
    // Send key for Caps Lock, and its keys let the stream show through.
    RumbleKeyboard::Options keyboardOptions;
    keyboardOptions.keyHeight = 56.0f;
    keyboardOptions.sendKey = false;
    keyboardOptions.translucent = true;
    keyboard = new RumbleKeyboard(
        {
            [this](const std::string& value) { appendText(value); },
            [this]() { backspaceDraft(); },
            [this]() { setEmojiPanelVisible(true); },
            {},
        },
        keyboardOptions);
    keyboard->setGrow(1.0f);
    sheet->addView(keyboard);

    // Emoji picker, shown in place of the keyboard by its Emoji key.
    emojiPanel = new brls::Box();
    emojiPanel->setAxis(brls::Axis::COLUMN);
    emojiPanel->setWidthPercentage(100.0f);
    emojiPanel->setGrow(1.0f);
    emojiPanel->setVisibility(brls::Visibility::GONE);
    sheet->addView(emojiPanel);

    auto* tabRow = new brls::Box();
    tabRow->setAxis(brls::Axis::ROW);
    tabRow->setMarginBottom(8);
    emojiPanel->addView(tabRow);

    auto* keyboardTab = makeActionButton("< Keyboard");
    keyboardTab->registerClickAction([this](brls::View*) {
        setEmojiPanelVisible(false);
        return true;
    });
    keyboardTab->setCustomNavigationRoute(brls::FocusDirection::UP, draftButton);
    tabRow->addView(keyboardTab);

    recentTab = makeActionButton("Recent");
    allTab = makeActionButton("All");
    recentTab->registerClickAction([this](brls::View*) {
        setTab(Tab::Recent);
        return true;
    });
    allTab->registerClickAction([this](brls::View*) {
        setTab(Tab::All);
        return true;
    });
    recentTab->setCustomNavigationRoute(brls::FocusDirection::UP, draftButton);
    allTab->setCustomNavigationRoute(brls::FocusDirection::UP, draftButton);
    tabRow->addView(recentTab);
    tabRow->addView(allTab);

    focusedEmojiLabel = new brls::Label();
    focusedEmojiLabel->setFontSize(14);
    focusedEmojiLabel->setTextColor(nvgRGB(167, 176, 195));
    focusedEmojiLabel->setMarginBottom(6);
    emojiPanel->addView(focusedEmojiLabel);

    grid = new RecyclingGrid();
    grid->setGrow(1.0f);
    grid->spanCount = 10;
    grid->estimatedRowHeight = 78;
    grid->estimatedRowSpace = 8;
    grid->preFetchLine = 2;
    grid->setPadding(2, 2, 2, 2);
    grid->registerCell("Emoji", []() { return new EmojiCell(); });
    grid->setIndexNavigationEnabled(true);
    grid->setTopBoundaryFocusRoute([this]() -> brls::View* { return tab == Tab::Recent ? recentTab : allTab; });
    emojiPanel->addView(grid);

    footerLabel = new brls::Label();
    footerLabel->setFontSize(14);
    footerLabel->setTextColor(nvgRGB(154, 165, 187));
    footerLabel->setMarginTop(6);
    footerLabel->setText(KEYBOARD_FOOTER);
    sheet->addView(footerLabel);

    this->registerAction("Delete", brls::BUTTON_Y, [this](brls::View*) {
        backspaceDraft();
        return true;
    });
    this->registerAction("Emoji", brls::BUTTON_X, [this](brls::View*) {
        setEmojiPanelVisible(!emojiPanelVisible);
        return true;
    });
    this->registerAction("Previous tab", brls::BUTTON_LB, [this](brls::View*) {
        setTab(tab == Tab::Recent ? Tab::All : Tab::Recent);
        return true;
    });
    this->registerAction("Next tab", brls::BUTTON_RB, [this](brls::View*) {
        setTab(tab == Tab::Recent ? Tab::All : Tab::Recent);
        return true;
    });
    this->registerAction("Send", brls::BUTTON_RT, [this](brls::View*) {
        sendMessage();
        return true;
    });
    this->registerAction("Send", brls::BUTTON_START, [this](brls::View*) {
        sendMessage();
        return true;
    });
    this->registerAction("Back", brls::BUTTON_B, [](brls::View*) { return brls::Application::popActivity(); });

    // Without recent emoji, open the picker on the full list.
    if (recentEmoji.empty()) tab = Tab::All;
    updateDraft();
    updateTabs();
    refreshGrid();
    updateSendRoute();

    brls::sync([this]() { brls::Application::giveFocus(keyboard->getStartKey()); });

    std::weak_ptr<bool> weakAlive = aliveToken;
    orientationSubscription =
        rumblenx::portrait::OrientationController::instance().getOrientationChanged()->subscribe(
            [weakAlive](rumblenx::portrait::DisplayOrientation) {
                brls::sync([weakAlive]() {
                    if (weakAlive.expired()) return;
                    brls::Application::popActivity();
                });
            });
}

RumbleChatComposer::~RumbleChatComposer() {
    if (orientationSubscription) {
        rumblenx::portrait::OrientationController::instance().getOrientationChanged()->unsubscribe(
            *orientationSubscription);
        orientationSubscription.reset();
    }
    brls::Logger::debug("RumbleChatComposer: deleted");
}

brls::View* RumbleChatComposer::getDefaultFocus() {
    if (!emojiPanelVisible && keyboard) return keyboard->getStartKey();
    return draftButton ? draftButton : this;
}

void RumbleChatComposer::setEmojiPanelVisible(bool visible) {
    if (emojiPanelVisible == visible) return;
    emojiPanelVisible = visible;

    keyboard->setVisibility(visible ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    emojiPanel->setVisibility(visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    footerLabel->setText(visible ? EMOJI_FOOTER : KEYBOARD_FOOTER);
    updateSendRoute();

    if (visible) {
        grid->invalidate();
        brls::sync([this]() {
            if (!emojiPanelVisible) return;
            if (!visibleEmoji().empty()) {
                brls::Application::giveFocus(grid);
                return;
            }
            brls::Application::giveFocus(tab == Tab::Recent ? recentTab : allTab);
        });
    } else {
        brls::Application::giveFocus(keyboard->getStartKey());
    }
    this->invalidate();
}

void RumbleChatComposer::appendText(const std::string& value) {
    if (rumble::chatMessageLength(draft + value) > rumble::chatMessageLimit()) {
        statusLabel->setText("Rumble chat messages are limited to " +
                             std::to_string(rumble::chatMessageLimit()) + " characters");
        return;
    }
    draft += value;
    updateDraft();
}

void RumbleChatComposer::updateSendRoute() {
    if (!sendButton) return;
    sendButton->setCustomNavigationRoute(brls::FocusDirection::DOWN,
        emojiPanelVisible ? static_cast<brls::View*>(tab == Tab::Recent ? recentTab : allTab)
                          : static_cast<brls::View*>(keyboard));
}

void RumbleChatComposer::insertEmoji(const rumble::Emoji& emoji) {
    if (emoji.glyph.empty()) return;
    const size_t before = draft.size();
    appendText(emoji.glyph);
    if (draft.size() == before) return;  // over the limit

    rumble::rememberRecentEmoji(emoji);
    recentEmoji.erase(std::remove_if(recentEmoji.begin(), recentEmoji.end(),
                          [&](const rumble::Emoji& current) { return current.glyph == emoji.glyph; }),
        recentEmoji.end());
    recentEmoji.insert(recentEmoji.begin(), emoji);
    if (recentEmoji.size() > 24) recentEmoji.resize(24);
    if (tab == Tab::Recent) refreshGrid();
}

void RumbleChatComposer::backspaceDraft() {
    eraseLastCodepoint(draft);
    updateDraft();
}

void RumbleChatComposer::sendMessage() {
    if (sending) return;

    const std::string message = rumble::parse::trim(draft);
    if (message.empty()) {
        brls::Application::notify("Enter a chat message first");
        return;
    }

    sending = true;
    if (sendLabel) sendLabel->setText("Sending...");
    statusLabel->setText("Sending message…");

    this->ptrLock();
    rumble::sendChatMessageAsync(
        videoId, message,
        [this](const std::string&) {
            sending = false;
            statusLabel->setText("Message sent");
            brls::Application::notify("RumbleNX: chat message sent");
            this->ptrUnlock();
            brls::Application::popActivity();
        },
        [this](const std::string& error) {
            sending = false;
            if (sendLabel) sendLabel->setText("Send");
            statusLabel->setText(error);
            brls::Application::notify("RumbleNX chat error: " + error);
            this->ptrUnlock();
        });
}

void RumbleChatComposer::setTab(Tab value) {
    tab = value;
    updateTabs();
    refreshGrid();
}

void RumbleChatComposer::updateDraft() {
    draftPreview->setDraft(draft);
    counterLabel->setText(std::to_string(rumble::chatMessageLength(draft)) + " / " +
                          std::to_string(rumble::chatMessageLimit()));
}

void RumbleChatComposer::updateTabs() {
    const NVGcolor active = nvgRGB(72, 110, 33);
    const NVGcolor inactive = nvgRGB(45, 50, 65);
    recentTab->setBackgroundColor(tab == Tab::Recent ? active : inactive);
    allTab->setBackgroundColor(tab == Tab::All ? active : inactive);
    updateSendRoute();
}

std::vector<rumble::Emoji> RumbleChatComposer::visibleEmoji() const {
    return tab == Tab::All ? rumble::emojiCatalogue() : recentEmoji;
}

void RumbleChatComposer::refreshGrid() {
    const auto values = visibleEmoji();
    if (values.empty()) {
        focusedEmojiLabel->setText(tab == Tab::Recent ? "No recent emoji yet" : "No emoji available");
    } else {
        focusedEmojiLabel->setText(std::to_string(values.size()) + " emoji");
    }

    grid->setVisibility(brls::Visibility::GONE);
    grid->setDataSource(new EmojiDataSource(
        values, [this](const rumble::Emoji& emoji) { insertEmoji(emoji); },
        [this](const rumble::Emoji& emoji) { focusedEmojiLabel->setText(emoji.glyph + "  " + emoji.name); }));
    // RecyclingGrid may keep an empty layout when its data changes while it
    // is shown; toggling visibility forces a fresh measurement.
    grid->setVisibility(brls::Visibility::VISIBLE);
    grid->invalidate();
}
