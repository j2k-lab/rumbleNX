#include "tab/remote_view.hpp"
#include "utils/debug_log.hpp"
#include "view/recycling_grid.hpp"
#include "view/svg_image.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "activity/rumble_channel.hpp"
#include "view/player_setting.hpp"
#include "view/rumble_chat_composer.hpp"
#include "view/rumble_keyboard.hpp"
#include "api/rumble.hpp"
#include "api/rumble_chat.hpp"
#include "api/rumble_parse.hpp"
#include "utils/thread.hpp"
#include "utils/misc.hpp"
#include "utils/config.hpp"
#include "utils/image.hpp"
#include "utils/haptics.hpp"
#include "utils/orientation.hpp"
#include "utils/theme.hpp"
#include <borealis/core/cache_helper.hpp>

#include <utility>
#include <algorithm>
#include <atomic>
#include <deque>
#include <memory>
#include <optional>
#include <sstream>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <unordered_map>
#include <vector>
#include <limits>
#include <stb_image.h>

using namespace brls::literals;

namespace {

NVGcolor chatColor(const std::string& value) {
    if (value.size() != 7 || value.front() != '#')
        return nvgRGB(238, 240, 246);
    try {
        const int red = std::stoi(value.substr(1, 2), nullptr, 16);
        const int green = std::stoi(value.substr(3, 2), nullptr, 16);
        const int blue = std::stoi(value.substr(5, 2), nullptr, 16);
        return nvgRGB(red, green, blue);
    } catch (...) {
        return nvgRGB(238, 240, 246);
    }
}

/// "$5.00"-style label for a rant amount.
std::string rantAmount(int cents) {
    return fmt::format("${}.{:02}", cents / 100, cents % 100);
}

/// Static (PNG) chat emotes, shared by every chat row. Textures are kept for
/// the life of the app; Rumble's emote set is small.
class EmoteTexturePool {
public:
    static EmoteTexturePool& instance() {
        static auto* pool = new EmoteTexturePool();
        return *pool;
    }

    brls::Image* get(const std::string& url) {
        if (url.empty()) return nullptr;
        auto found = entries.find(url);
        if (found != entries.end()) return found->second;
        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning("RumbleNX: emote texture pool reached {} entries; new emotes use text", MAX_ENTRIES);
            }
            return nullptr;
        }
        auto* image = new brls::Image();
        image->setFreeTexture(false);
        entries.emplace(url, image);
        Image::with(image, url);
        return image;
    }

private:
    static constexpr size_t MAX_ENTRIES = 192;
    std::unordered_map<std::string, brls::Image*> entries;
    bool capacityWarningShown = false;
};

/// Animated (GIF) chat emotes, decoded off the UI thread into one texture per
/// frame.
class AnimatedEmoteTexturePool {
public:
    struct Entry {
        std::vector<int> textures;
        std::vector<int> delaysMs;
        int totalDurationMs = 0;
        bool ready = false;
        bool failed = false;
        std::chrono::steady_clock::time_point startedAt{};
    };

    static AnimatedEmoteTexturePool& instance() {
        static auto* pool = new AnimatedEmoteTexturePool();
        return *pool;
    }

    Entry* get(const std::string& url) {
        if (url.empty()) return nullptr;
        auto found = entries.find(url);
        if (found != entries.end()) return found->second.get();
        if (entries.size() >= MAX_ENTRIES) {
            if (!capacityWarningShown) {
                capacityWarningShown = true;
                brls::Logger::warning(
                    "RumbleNX: animated emote pool reached {} entries; new emotes use their first frame", MAX_ENTRIES);
            }
            return nullptr;
        }

        auto entry = std::make_shared<Entry>();
        Entry* result = entry.get();
        entries.emplace(url, entry);

        ThreadPool::instance().submit([entry, url](HTTP& http) {
            try {
                std::ostringstream body;
                HTTP::set_option(http, HTTP::Timeout{12000});
                http._get(url, &body);
                const std::string data = body.str();
                if (data.empty() || data.size() > MAX_DOWNLOAD_BYTES)
                    throw std::runtime_error("animated emote download is empty or too large");

                int* sourceDelays = nullptr;
                int width = 0;
                int height = 0;
                int frameCount = 0;
                int components = 0;
                stbi_uc* decoded = stbi_load_gif_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                    static_cast<int>(data.size()), &sourceDelays, &width, &height, &frameCount, &components, 4);
                if (!decoded || width <= 0 || height <= 0 || frameCount <= 0) {
                    if (sourceDelays) stbi_image_free(sourceDelays);
                    if (decoded) stbi_image_free(decoded);
                    throw std::runtime_error("animated emote GIF could not be decoded");
                }

                const size_t pixelsPerFrame = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
                const size_t decodedBytes = pixelsPerFrame * static_cast<size_t>(frameCount);
                if (width > MAX_DIMENSION || height > MAX_DIMENSION || frameCount > MAX_FRAMES ||
                    decodedBytes > MAX_DECODED_BYTES) {
                    stbi_image_free(decoded);
                    if (sourceDelays) stbi_image_free(sourceDelays);
                    throw std::runtime_error("animated emote exceeds RumbleNX safety limits");
                }

                auto pixels = std::make_shared<std::vector<uint8_t>>(decoded, decoded + decodedBytes);
                stbi_image_free(decoded);

                std::vector<int> delays;
                delays.reserve(static_cast<size_t>(frameCount));
                for (int index = 0; index < frameCount; ++index)
                    delays.push_back(std::clamp(sourceDelays ? sourceDelays[index] : 100, 20, 1000));
                if (sourceDelays) stbi_image_free(sourceDelays);

                brls::sync([entry, pixels, delays = std::move(delays), width, height, frameCount,
                               pixelsPerFrame]() mutable {
                    NVGcontext* vg = brls::Application::getNVGContext();
                    if (!vg) {
                        entry->failed = true;
                        return;
                    }
                    std::vector<int> textures;
                    textures.reserve(static_cast<size_t>(frameCount));
                    for (int index = 0; index < frameCount; ++index) {
                        const uint8_t* frame = pixels->data() + pixelsPerFrame * static_cast<size_t>(index);
                        const int texture = nvgCreateImageRGBA(vg, width, height, 0, frame);
                        if (texture <= 0) {
                            for (const int created : textures) nvgDeleteImage(vg, created);
                            entry->failed = true;
                            return;
                        }
                        textures.push_back(texture);
                    }
                    entry->textures = std::move(textures);
                    entry->delaysMs = std::move(delays);
                    entry->totalDurationMs = 0;
                    for (const int delay : entry->delaysMs) entry->totalDurationMs += delay;
                    entry->startedAt = std::chrono::steady_clock::now();
                    entry->ready = !entry->textures.empty() && entry->totalDurationMs > 0;
                });
            } catch (const std::exception& exception) {
                brls::Logger::warning("RumbleNX: animated emote failed: {}", exception.what());
                brls::sync([entry]() { entry->failed = true; });
            } catch (...) {
                brls::sync([entry]() { entry->failed = true; });
            }
        });
        return result;
    }

    /// The frame to show now, or 0 while loading or after a failure.
    int texture(const Entry* entry) const {
        if (!entry || !entry->ready || entry->textures.empty() ||
            entry->delaysMs.size() != entry->textures.size() || entry->totalDurationMs <= 0)
            return 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - entry->startedAt)
                                 .count();
        int position = static_cast<int>(elapsed % entry->totalDurationMs);
        for (size_t index = 0; index < entry->textures.size(); ++index) {
            if (position < entry->delaysMs[index]) return entry->textures[index];
            position -= entry->delaysMs[index];
        }
        return entry->textures.back();
    }

private:
    static constexpr size_t MAX_ENTRIES = 48;
    static constexpr size_t MAX_DOWNLOAD_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_DECODED_BYTES = 8 * 1024 * 1024;
    static constexpr int MAX_DIMENSION = 128;
    // Rumble's 72px emotes run to about 100 frames.
    static constexpr int MAX_FRAMES = 160;

    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
    bool capacityWarningShown = false;
};

bool isGifUrl(const std::string& url) {
    const std::string path = url.substr(0, url.find_first_of("?#"));
    return path.size() >= 4 && path.compare(path.size() - 4, 4, ".gif") == 0;
}

class ChatMessageView : public brls::View {
public:
    /// Docked chat follows the app theme; the overlay on top of the video
    /// keeps its light-on-dark colours.
    void setThemed(bool value) { themed = value; }

    void configure(
        const rumble::ChatMessage& value,
        const rumble::ChatPreferences& preferences,
        float availableWidth,
        float availableHeight) {
        message = value;
        fontSize = preferences.fontSize;
        showTimestamp = preferences.timestamps;
        contentWidth = std::max(100.0f, availableWidth);

        emotes.clear();
        for (const auto& fragment : message.fragments) {
            if (fragment.type != rumble::ChatFragmentType::Emote) continue;
            EmoteVisual visual;
            if (isGifUrl(fragment.emoteUrl)) visual.animation = AnimatedEmoteTexturePool::instance().get(fragment.emoteUrl);
            // Static emotes, and the first frame of a GIF the animated pool
            // has no room for.
            if (!visual.animation) visual.staticImage = EmoteTexturePool::instance().get(fragment.emoteUrl);
            emotes.push_back(visual);
        }
        contentHeightLimit = std::max(fontSize * 1.28f + 7.0f, availableHeight);

        this->setWidth(contentWidth);
        this->setHeight(estimateHeightFor(message, preferences, contentWidth, contentHeightLimit));
        this->setVisibility(brls::Visibility::VISIBLE);
        this->invalidate();
    }

    void clear() {
        message = {};
        emotes.clear();
        this->setHeight(0);
        this->setVisibility(brls::Visibility::GONE);
    }

    static float estimateHeightFor(
        const rumble::ChatMessage& message,
        const rumble::ChatPreferences& preferences,
        float availableWidth,
        float availableHeight) {
        const float fontSize = preferences.fontSize;
        const bool rant = message.rantCents > 0;
        const float contentWidth = std::max(100.0f, availableWidth) - (rant ? RANT_INSET * 2.0f : 0.0f);
        const float contentHeightLimit = std::max(fontSize * 1.28f + 7.0f, availableHeight);
        const float lineHeight = fontSize * 1.28f;
        const int maxLines = maxLineCountFor(fontSize, contentHeightLimit);

        NVGcontext* vg = brls::Application::getNVGContext();
        if (!vg) return lineHeight + 7.0f;

        nvgSave(vg);
        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(vg, 0, 0, value.c_str(), nullptr, bounds);
        };

        float cursor = 0.0f;
        int lineCount = 1;
        auto consumeWord = [&](const std::string& value) {
            if (value.empty() || lineCount > maxLines) return;
            const float wordWidth = textWidth(value);
            if (cursor > 0.0f && cursor + wordWidth > contentWidth) {
                cursor = 0.0f;
                lineCount = std::min(lineCount + 1, maxLines + 1);
            }
            cursor += wordWidth;
        };
        auto consumeWrappedText = [&](const std::string& value) {
            forEachWord(value, [&](const std::string& word, bool newline) {
                consumeWord(word);
                if (newline) {
                    cursor = 0.0f;
                    lineCount = std::min(lineCount + 1, maxLines + 1);
                }
            });
        };

        if (preferences.timestamps && !message.timestamp.empty()) consumeWord("[" + message.timestamp + "] ");
        if (rant) consumeWord(rantAmount(message.rantCents) + " ");
        consumeWord(message.userName + ": ");
        const float emoteWidth = emoteSizeFor(fontSize) + EMOTE_GAP;
        for (const auto& fragment : message.fragments) {
            if (fragment.type != rumble::ChatFragmentType::Emote) {
                consumeWrappedText(fragment.text);
                continue;
            }
            if (lineCount > maxLines) break;
            if (cursor > 0.0f && cursor + emoteWidth > contentWidth) {
                cursor = 0.0f;
                lineCount = std::min(lineCount + 1, maxLines + 1);
            }
            cursor += emoteWidth;
        }

        nvgRestore(vg);
        return std::clamp(lineCount, 1, maxLines) * lineHeight + 7.0f + (rant ? RANT_INSET : 0.0f);
    }

    void draw(
        NVGcontext* vg,
        float x,
        float y,
        float width,
        float height,
        brls::Style style,
        brls::FrameContext* ctx) override {
        if (message.text.empty() && message.rantCents <= 0) return;

        // Never allow one message to paint into the next row.
        nvgSave(vg);
        nvgIntersectScissor(vg, x, y, width, height);

        const bool rant = message.rantCents > 0;
        if (rant) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y + 1.0f, width, height - 3.0f, 6.0f);
            nvgFillColor(vg, chatColor(message.rantBackground));
            nvgFill(vg);
        }

        const float inset = rant ? RANT_INSET : 0.0f;
        const float lineHeight = fontSize * 1.28f;
        const float left = x + inset;
        const float right = x + width - inset;
        float cursorX = left;
        float cursorY = y + fontSize + inset * 0.5f;
        int line = 0;
        const int maxLines = maxLineCountFor(fontSize, contentHeightLimit);

        nvgFontFaceId(vg, brls::Application::getDefaultFont());
        nvgFontSize(vg, fontSize);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);

        auto textWidth = [&](const std::string& value) {
            if (value.empty()) return 0.0f;
            float bounds[4]{};
            return nvgTextBounds(vg, 0, 0, value.c_str(), nullptr, bounds);
        };
        auto nextLine = [&]() {
            cursorX = left;
            cursorY += lineHeight;
            ++line;
        };
        auto drawWord = [&](const std::string& value, NVGcolor color) {
            if (value.empty() || line >= maxLines) return;
            const float wordWidth = textWidth(value);
            if (cursorX > left && cursorX + wordWidth > right) nextLine();
            if (line >= maxLines) return;
            nvgFillColor(vg, color);
            nvgText(vg, cursorX, cursorY, value.c_str(), nullptr);
            cursorX += wordWidth;
        };
        auto drawWrappedText = [&](const std::string& value, NVGcolor color) {
            forEachWord(value, [&](const std::string& word, bool newline) {
                drawWord(word, color);
                if (newline) nextLine();
            });
        };

        const NVGcolor messageColor = rant ? chatColor(message.rantForeground)
            : themed                       ? rumblenx::theme::current().textPrimary
                                           : nvgRGB(245, 246, 250);
        const NVGcolor faintColor = rant ? messageColor
            : themed                     ? rumblenx::theme::current().textFaint
                                         : nvgRGB(145, 154, 174);
        const NVGcolor mentionColor = rant ? messageColor
            : themed                       ? rumblenx::theme::current().accent
                                           : nvgRGB(133, 199, 66);

        if (showTimestamp && !message.timestamp.empty()) drawWord("[" + message.timestamp + "] ", faintColor);
        if (rant) drawWord(rantAmount(message.rantCents) + " ", messageColor);
        drawWord(message.userName + ": ", rant ? messageColor : chatColor(message.color));
        const float emoteSize = emoteSizeFor(fontSize);
        size_t emoteIndex = 0;
        for (const auto& fragment : message.fragments) {
            if (line >= maxLines) break;
            if (fragment.type != rumble::ChatFragmentType::Emote) {
                drawWrappedText(fragment.text,
                    fragment.type == rumble::ChatFragmentType::Mention ? mentionColor : messageColor);
                continue;
            }

            const EmoteVisual* visual = emoteIndex < emotes.size() ? &emotes[emoteIndex] : nullptr;
            ++emoteIndex;
            int texture = 0;
            if (visual && visual->animation) texture = AnimatedEmoteTexturePool::instance().texture(visual->animation);
            if (texture <= 0 && visual && visual->staticImage) texture = visual->staticImage->getTexture();

            // The emote's space is kept while it loads so the row does not
            // reflow; the code is shown only when the image can never load.
            const bool unavailable = !visual || (visual->animation ? visual->animation->failed : !visual->staticImage);
            if (unavailable) {
                drawWord(fragment.text + " ", messageColor);
                continue;
            }
            if (cursorX > left && cursorX + emoteSize > right) nextLine();
            if (line >= maxLines) break;
            if (texture > 0) {
                const float imageY = cursorY - fontSize - (emoteSize - fontSize) * 0.35f;
                NVGpaint paint = nvgImagePattern(vg, cursorX, imageY, emoteSize, emoteSize, 0, texture, 1.0f);
                nvgBeginPath(vg);
                nvgRect(vg, cursorX, imageY, emoteSize, emoteSize);
                nvgFillPaint(vg, paint);
                nvgFill(vg);
            }
            cursorX += emoteSize + EMOTE_GAP;
        }

        if (!rant) {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, y + height - 1);
            nvgLineTo(vg, x + width, y + height - 1);
            nvgStrokeColor(vg, themed ? rumblenx::theme::current().divider : nvgRGBA(255, 255, 255, 18));
            nvgStrokeWidth(vg, 1.0f);
            nvgStroke(vg);
        }
        nvgRestore(vg);
    }

private:
    struct EmoteVisual {
        brls::Image* staticImage = nullptr;
        AnimatedEmoteTexturePool::Entry* animation = nullptr;
    };

    static constexpr float RANT_INSET = 6.0f;
    static constexpr float EMOTE_GAP = 3.0f;

    static float emoteSizeFor(float fontSize) { return std::max(18.0f, fontSize * 1.35f); }

    /// Calls `fn(word, endsWithNewline)` for each space-terminated word.
    template <typename Fn>
    static void forEachWord(const std::string& value, Fn fn) {
        std::string token;
        for (size_t index = 0; index < value.size(); ++index) {
            token.push_back(value[index]);
            const bool boundary = value[index] == ' ' || value[index] == '\n' || index + 1 == value.size();
            if (!boundary) continue;
            const bool newline = !token.empty() && token.back() == '\n';
            if (newline) token.pop_back();
            fn(token, newline);
            token.clear();
        }
    }

    static int maxLineCountFor(float fontSize, float contentHeightLimit) {
        const float lineHeight = fontSize * 1.28f;
        return std::max(1,
            static_cast<int>(std::floor(std::max(0.0f, contentHeightLimit - 7.0f) / std::max(1.0f, lineHeight))));
    }

    rumble::ChatMessage message;
    std::vector<EmoteVisual> emotes;
    float fontSize = 16.0f;
    float contentWidth = 300.0f;
    float contentHeightLimit = brls::Application::contentHeight;
    bool showTimestamp = false;
    bool themed = false;
};

class PortraitEmojiCell : public RecyclingGridItem {
public:
    PortraitEmojiCell() {
        this->setAxis(brls::Axis::COLUMN);
        this->setFocusable(true);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setPadding(5, 5, 5, 5);
        this->setCornerRadius(6.0f);
        this->setBackgroundColor(nvgRGB(31, 35, 46));

        glyph = new brls::Label();
        glyph->setFontSize(32.0f);
        glyph->setSingleLine(true);
        glyph->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        glyph->setTextColor(nvgRGB(245, 246, 250));
        this->addView(glyph);
    }

    void prepareForReuse() override { glyph->setText(""); }

    void configure(const rumble::Emoji& emoji) { glyph->setText(emoji.glyph); }

private:
    brls::Label* glyph = nullptr;
};

class PortraitEmojiDataSource : public RecyclingGridDataSource {
public:
    PortraitEmojiDataSource(std::vector<rumble::Emoji> emoji, std::function<void(const rumble::Emoji&)> selected)
        : emoji(std::move(emoji)), selected(std::move(selected)) {}

    size_t getItemCount() override { return emoji.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<PortraitEmojiCell*>(recycler->dequeueReusableCell("PortraitEmoji"));
        cell->configure(emoji.at(index));
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (selected) selected(emoji.at(index));
    }

    void clearData() override { emoji.clear(); }

private:
    std::vector<rumble::Emoji> emoji;
    std::function<void(const rumble::Emoji&)> selected;
};

/// Portrait layout's message area: draft, touch keyboard and emoji sheet.
class PortraitChatComposer : public brls::Box {
public:
    using SendCallback = std::function<bool(const std::string&)>;

    explicit PortraitChatComposer(SendCallback sendRequested)
        : sendRequested(std::move(sendRequested)), recentEmoji(rumble::loadRecentEmoji()) {
        this->setAxis(brls::Axis::COLUMN);
        this->setWidthPercentage(100.0f);
        this->setHeight(485.0f);
        this->setPadding(10, 12, 10, 12);
        this->setBackgroundColor(nvgRGB(10, 12, 18));
        this->setVisibility(brls::Visibility::GONE);

        auto* draftBox = new brls::Box();
        draftBox->setAxis(brls::Axis::COLUMN);
        draftBox->setWidthPercentage(100.0f);
        draftBox->setHeight(88.0f);
        draftBox->setPadding(9, 14, 8, 14);
        draftBox->setMarginBottom(8.0f);
        draftBox->setCornerRadius(6.0f);
        draftBox->setBackgroundColor(nvgRGB(28, 33, 43));
        draftBox->registerClickAction([this](brls::View*) {
            setEmojiPanelVisible(!emojiPanelVisible);
            return true;
        });
        draftBox->addGestureRecognizer(new brls::TapGestureRecognizer(draftBox));
        this->addView(draftBox);

        auto* draftHeader = new brls::Box();
        draftHeader->setAxis(brls::Axis::ROW);
        draftHeader->setWidthPercentage(100.0f);
        draftHeader->setHeight(22.0f);
        draftBox->addView(draftHeader);

        auto* caption = new brls::Label();
        caption->setText("Message");
        caption->setFontSize(14.0f);
        caption->setTextColor(nvgRGB(168, 178, 197));
        caption->setHorizontalAlign(brls::HorizontalAlign::LEFT);
        caption->setGrow(1.0f);
        draftHeader->addView(caption);

        counter = new brls::Label();
        counter->setFontSize(13.0f);
        counter->setTextColor(nvgRGB(142, 151, 170));
        counter->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        draftHeader->addView(counter);

        draftPreview = new ComposerDraftPreview("Type a message...", 20.0f);
        draftPreview->setGrow(1.0f);
        draftPreview->setWidthPercentage(100.0f);
        draftBox->addView(draftPreview);

        keyboardContainer = new RumbleKeyboard(
            {
                [this](const std::string& value) { append(value); },
                [this]() {
                    eraseLastCodepoint(draft);
                    refreshDraft();
                },
                [this]() { setEmojiPanelVisible(true); },
                [this]() { send(); },
            },
            RumbleKeyboard::Options{});
        keyboardContainer->setGrow(1.0f);
        this->addView(keyboardContainer);

        buildEmojiPanel();
        refreshDraft();
    }

    void setFrameSize(float width, float height) {
        this->setWidth(width);
        this->setHeight(height);
    }

    void setEmojiPanelVisible(bool visible) {
        if (emojiPanelVisible == visible) return;
        emojiPanelVisible = visible;
        keyboardContainer->setVisibility(visible ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        emojiPanel->setVisibility(visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        if (visible) {
            emojiGrid->invalidate();
            brls::sync([this]() {
                if (emojiPanelVisible) brls::Application::giveFocus(emojiGrid);
            });
        }
        this->invalidate();
    }

    bool isEmojiPanelVisible() const { return emojiPanelVisible; }

private:
    void append(const std::string& value) {
        if (rumble::chatMessageLength(draft + value) > rumble::chatMessageLimit()) {
            brls::Application::notify(
                "Rumble chat messages are limited to " + std::to_string(rumble::chatMessageLimit()) + " characters");
            return;
        }
        draft += value;
        refreshDraft();
    }

    void insertEmoji(const rumble::Emoji& emoji) {
        const size_t before = draft.size();
        append(emoji.glyph);
        if (draft.size() == before) return;
        rumble::rememberRecentEmoji(emoji);
        recentEmoji = rumble::loadRecentEmoji();
        if (showRecent) refreshEmojiGrid();
    }

    void buildEmojiPanel() {
        emojiPanel = new brls::Box();
        emojiPanel->setAxis(brls::Axis::COLUMN);
        emojiPanel->setWidthPercentage(100.0f);
        emojiPanel->setGrow(1.0f);
        emojiPanel->setVisibility(brls::Visibility::GONE);
        this->addView(emojiPanel);

        auto* headerBar = new brls::Box();
        headerBar->setAxis(brls::Axis::ROW);
        headerBar->setWidthPercentage(100.0f);
        headerBar->setHeight(38.0f);
        headerBar->setAlignItems(brls::AlignItems::CENTER);
        headerBar->setMarginBottom(8.0f);
        emojiPanel->addView(headerBar);

        auto makeTab = [headerBar](const std::string& text, float marginLeft) {
            auto* button = new brls::Button();
            button->setText(text);
            button->setFontSize(14.0f);
            button->setHeight(36.0f);
            button->setPadding(4, 12, 4, 12);
            button->setMarginLeft(marginLeft);
            button->setStyle(&brls::BUTTONSTYLE_DEFAULT);
            button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
            headerBar->addView(button);
            return button;
        };

        auto* backToKeyboard = makeTab("< Keyboard", 0.0f);
        backToKeyboard->registerClickAction([this](brls::View*) {
            setEmojiPanelVisible(false);
            return true;
        });
        btnRecentTab = makeTab("Recent", 8.0f);
        btnAllTab = makeTab("All", 4.0f);
        btnRecentTab->registerClickAction([this](brls::View*) {
            showRecent = true;
            refreshEmojiGrid();
            return true;
        });
        btnAllTab->registerClickAction([this](brls::View*) {
            showRecent = false;
            refreshEmojiGrid();
            return true;
        });

        emojiStatus = new brls::Label();
        emojiStatus->setFontSize(13.0f);
        emojiStatus->setTextColor(nvgRGB(168, 178, 197));
        emojiStatus->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        emojiStatus->setGrow(1.0f);
        headerBar->addView(emojiStatus);

        emojiGrid = new RecyclingGrid();
        emojiGrid->setGrow(1.0f);
        emojiGrid->spanCount = 6;
        emojiGrid->estimatedRowHeight = 72.0f;
        emojiGrid->estimatedRowSpace = 6.0f;
        emojiGrid->preFetchLine = 2;
        emojiGrid->setPadding(2, 2, 2, 2);
        emojiGrid->registerCell("PortraitEmoji", []() { return new PortraitEmojiCell(); });
        emojiGrid->setIndexNavigationEnabled(true);
        emojiPanel->addView(emojiGrid);

        showRecent = !recentEmoji.empty();
        refreshEmojiGrid();
    }

    void refreshEmojiGrid() {
        const auto list = showRecent ? recentEmoji : rumble::emojiCatalogue();
        btnRecentTab->setStyle(showRecent ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
        btnAllTab->setStyle(showRecent ? &brls::BUTTONSTYLE_DEFAULT : &brls::BUTTONSTYLE_PRIMARY);
        emojiStatus->setText(list.empty() ? "No recent emoji" : fmt::format("{} emoji", list.size()));
        emojiGrid->setVisibility(brls::Visibility::GONE);
        emojiGrid->setDataSource(
            new PortraitEmojiDataSource(list, [this](const rumble::Emoji& emoji) { insertEmoji(emoji); }));
        emojiGrid->setVisibility(brls::Visibility::VISIBLE);
        emojiGrid->invalidate();
    }

    void send() {
        if (draft.empty() || !sendRequested) return;
        if (sendRequested(draft)) {
            draft.clear();
            keyboardContainer->resetShift();
            refreshDraft();
        }
    }

    void refreshDraft() {
        draftPreview->setDraft(draft);
        counter->setText(fmt::format("{} / {}", rumble::chatMessageLength(draft), rumble::chatMessageLimit()));
        this->invalidate();
    }

    SendCallback sendRequested;
    std::string draft;
    ComposerDraftPreview* draftPreview = nullptr;
    brls::Label* counter = nullptr;
    RumbleKeyboard* keyboardContainer = nullptr;
    brls::Box* emojiPanel = nullptr;
    brls::Label* emojiStatus = nullptr;
    RecyclingGrid* emojiGrid = nullptr;
    brls::Button* btnRecentTab = nullptr;
    brls::Button* btnAllTab = nullptr;
    std::vector<rumble::Emoji> recentEmoji;
    bool showRecent = true;
    bool emojiPanelVisible = false;
};

class ChatPanel : public brls::Box {
public:
    ChatPanel(
        bool docked,
        std::function<void()> sendRequested,
        std::function<bool()> backRequested)
        : docked(docked),
          sendRequested(std::move(sendRequested)),
          backRequested(std::move(backRequested)) {
        this->setAxis(brls::Axis::COLUMN);
        this->setWidth(docked ? 330.0f : 360.0f);
        panelHeight = brls::Application::contentHeight;
        this->setHeight(panelHeight);
        if (!docked)
            this->setPositionType(brls::PositionType::ABSOLUTE);
        this->setPadding(18, 12, 12, 12);
        this->setVisibility(brls::Visibility::GONE);
        this->setFocusable(false);

        headerBox = new brls::Box();
        headerBox->setAxis(brls::Axis::ROW);
        headerBox->setAlignItems(brls::AlignItems::CENTER);
        headerBox->setMarginBottom(7);

        // Docked chat sits beside the video and follows the app theme; the
        // overlay is drawn over the video and keeps its light-on-dark look.
        const auto& palette = rumblenx::theme::current();
        const NVGcolor primaryText = docked ? palette.textPrimary : nvgRGB(245, 246, 250);
        const NVGcolor mutedText = docked ? palette.textMuted : nvgRGB(173, 181, 199);

        titlePrefix = new brls::Label();
        titlePrefix->setText("LIVE CHAT");
        titlePrefix->setFontSize(16);
        titlePrefix->setTextColor(primaryText);

        headerBox->addView(titlePrefix);
        this->addView(headerBox);

        status = new brls::Label();
        status->setText("");
        status->setFontSize(13);
        status->setTextColor(mutedText);
        status->setMarginBottom(7);
        status->setVisibility(brls::Visibility::GONE);
        this->addView(status);

        messagesBox = new brls::Box();
        messagesBox->setAxis(brls::Axis::COLUMN);
        messagesBox->setGrow(1.0f);
        this->addView(messagesBox);

        for (size_t index = 0; index < ROW_POOL; ++index) {
            auto* row = new ChatMessageView();
            row->setThemed(docked);
            row->setVisibility(brls::Visibility::GONE);
            messagesBox->addView(row);
            rows.push_back(row);
        }

        // An outlined button whose text and outline follow the page text
        // colour. setTextColor re-applies
        // the bordered style, so the outline is set after it.
        const NVGcolor composeColor = docked ? primaryText : nvgRGB(255, 255, 255);
        composeButton = new brls::Button();
        composeButton->setText("Send message");
        composeButton->setFontSize(15);
        composeButton->setHeight(56);
        composeButton->setStyle(&brls::BUTTONSTYLE_BORDERED);
        composeButton->setCornerRadius(8);
        composeButton->setTextColor(composeColor);
        composeButton->setBorderThickness(2.0f);
        composeButton->setBorderColor(composeColor);
        composeButton->setAxis(brls::Axis::COLUMN);
        composeButton->setPaddingTop(0);
        composeButton->setPaddingBottom(0);
        composeButton->setMarginTop(8);
        composeButton->setVisibility(brls::Visibility::GONE);
        composeButton->registerClickAction(
            [this](brls::View*) {
                if (this->sendRequested) this->sendRequested();
                return true;
            });

        // The chat panel is outside VideoView's focus tree. If this button
        // receives focus after closing a composer/IME, route B to the same
        // playback back action instead of swallowing it.
        composeButton->registerAction(
            "Back",
            brls::BUTTON_B,
            [this](brls::View*) {
                return this->backRequested
                    ? this->backRequested()
                    : false;
            });

        auto* composeHint = new brls::Label();
        composeHint->setText("Tap or press ZL");
        composeHint->setFontSize(12);
        composeHint->setTextColor(mutedText);
        composeHint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        composeHint->setMarginTop(2);
        composeButton->addView(composeHint);

        this->addView(composeButton);
    }

    void setComposeVisible(bool visible) {
        composeButton->setVisibility(
            visible ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }

    void apply(const rumble::ChatPreferences& value) {
        preferences = value;
        panelWidth = static_cast<float>(
            docked ? value.dockedWidth : value.overlayWidth);
        this->setWidth(panelWidth);

        if (docked) {
            panelHeight = brls::Application::contentHeight;
            this->setHeight(panelHeight);
            this->setBackgroundColor(rumblenx::theme::current().background);
        } else {
            const bool compact =
                value.overlaySize == rumble::ChatOverlaySize::Compact;
            panelHeight = compact
                ? std::max(
                      280.0f,
                      brls::Application::contentHeight * 0.5f)
                : brls::Application::contentHeight;
            this->setHeight(panelHeight);

            const float inset = compact ? 12.0f : 0.0f;
            const bool onLeft =
                value.overlayPosition ==
                    rumble::ChatOverlayPosition::TopLeft ||
                value.overlayPosition ==
                    rumble::ChatOverlayPosition::BottomLeft;
            const bool onBottom =
                compact &&
                (value.overlayPosition ==
                     rumble::ChatOverlayPosition::BottomRight ||
                 value.overlayPosition ==
                     rumble::ChatOverlayPosition::BottomLeft);

            this->setPositionTop(
                onBottom ? brls::View::AUTO : inset);
            this->setPositionBottom(
                onBottom ? inset : brls::View::AUTO);
            this->setPositionLeft(
                onLeft ? inset : brls::View::AUTO);
            this->setPositionRight(
                onLeft ? brls::View::AUTO : inset);

            const int alpha =
                std::clamp(value.opacity * 255 / 100, 0, 255);
            this->setBackgroundColor(nvgRGBA(12, 14, 20, alpha));
        }

        setComposeVisible(
            value.participation ==
            rumble::ChatParticipation::Interactive);

        refresh();
    }

    void setStatus(const std::string& value) {
        if (value.empty() ||
            value == "Live chat" ||
            value == "Chat disabled" ||
            value == "Connected") {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
            return;
        }

        status->setText(value);
        status->setVisibility(brls::Visibility::VISIBLE);
    }

    /// Drops messages Rumble deleted (moderation).
    void remove(const std::vector<std::string>& ids) {
        const auto before = messages.size();
        messages.erase(std::remove_if(messages.begin(), messages.end(),
                           [&](const rumble::ChatMessage& message) {
                               return std::find(ids.begin(), ids.end(), message.id) != ids.end();
                           }),
            messages.end());
        if (messages.size() != before) refresh();
    }

    void append(rumble::ChatMessage message) {
        if (status->getVisibility() == brls::Visibility::VISIBLE) {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
        }
        messages.push_back(std::move(message));
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
        refresh();
    }

    void replaceMessages(
        const std::deque<rumble::ChatMessage>& value) {
        if (!value.empty() && status->getVisibility() == brls::Visibility::VISIBLE) {
            status->setText("");
            status->setVisibility(brls::Visibility::GONE);
        }
        messages = value;
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
        refresh();
    }

    const std::deque<rumble::ChatMessage>& visibleMessages() const {
        return messages;
    }

    float width() const { return panelWidth; }

    void setPortraitFrame(float width, float height) {
        panelWidth = width;
        panelHeight = height;
        this->setWidth(width);
        this->setHeight(height);
        this->setBackgroundColor(rumblenx::theme::current().background);
        setComposeVisible(false);
        refresh();
    }

private:
    void refresh() {
        for (auto* row : rows) row->clear();
        if (messages.empty()) return;

        const float innerWidth =
            std::max(120.0f, panelWidth - 24.0f);
        const float reserved =
            18.0f + 28.0f +
            (status->getVisibility() == brls::Visibility::VISIBLE
                ? 28.0f
                : 0.0f) +
            (composeButton->getVisibility() == brls::Visibility::VISIBLE
                ? 68.0f
                : 0.0f);
        const float availableHeight =
            std::max(
                120.0f,
                panelHeight - reserved);

        std::vector<const rumble::ChatMessage*> selected;
        float consumed = 0.0f;

        for (auto iterator = messages.rbegin();
             iterator != messages.rend() &&
             selected.size() < rows.size();
             ++iterator) {
            const float measuredHeight =
                ChatMessageView::estimateHeightFor(
                *iterator,
                preferences,
                innerWidth,
                availableHeight);

            if (!selected.empty() &&
                consumed + measuredHeight > availableHeight)
                break;

            selected.push_back(&*iterator);
            consumed += measuredHeight;
        }

        std::reverse(selected.begin(), selected.end());
        std::deque<rumble::ChatMessage> visibleMessages;
        for (size_t index = 0;
             index < selected.size() && index < rows.size();
             ++index) {
            rows[index]->configure(
                *selected[index],
                preferences,
                innerWidth,
                availableHeight);
            visibleMessages.push_back(*selected[index]);
        }

        messages = std::move(visibleMessages);
    }

    static constexpr size_t ROW_POOL = 20;
    static constexpr size_t MAX_MESSAGES = ROW_POOL;

    bool docked = false;
    float panelWidth = 330.0f;
    float panelHeight = brls::Application::contentHeight;
    rumble::ChatPreferences preferences;
    std::function<void()> sendRequested;
    std::function<bool()> backRequested;
    std::deque<rumble::ChatMessage> messages;
    brls::Box* headerBox = nullptr;
    brls::Label* titlePrefix = nullptr;
    brls::Label* status = nullptr;
    brls::Box* messagesBox = nullptr;
    brls::Button* composeButton = nullptr;
    std::vector<ChatMessageView*> rows;
};

struct ChatUiState {
    std::atomic_bool alive{true};
    ChatPanel* activePanel = nullptr;
    std::deque<rumble::ChatMessage> messages;
    std::string status;

    void append(rumble::ChatMessage message) {
        status.clear();
        if (activePanel) {
            activePanel->setStatus("");
            activePanel->append(std::move(message));
            messages = activePanel->visibleMessages();
            return;
        }

        messages.push_back(std::move(message));
        while (messages.size() > MAX_MESSAGES)
            messages.pop_front();
    }

    void setStatus(const std::string& value) {
        if (!messages.empty() && value.rfind("Connecting", 0) == 0) {
            return;
        }
        status = value;
        if (activePanel) activePanel->setStatus(value);
    }

    void remove(const std::vector<std::string>& ids) {
        if (activePanel) {
            activePanel->remove(ids);
            messages = activePanel->visibleMessages();
            return;
        }
        messages.erase(std::remove_if(messages.begin(), messages.end(),
                           [&](const rumble::ChatMessage& message) {
                               return std::find(ids.begin(), ids.end(), message.id) != ids.end();
                           }),
            messages.end());
    }

    void activate(ChatPanel* panel) {
        if (activePanel == panel) return;

        activePanel = panel;
        if (!activePanel) return;

        if (!messages.empty() && status.rfind("Connecting", 0) == 0) {
            status.clear();
        }

        activePanel->replaceMessages(messages);
        messages = activePanel->visibleMessages();
        activePanel->setStatus(status);
    }

private:
    static constexpr size_t MAX_MESSAGES = 20;
};


struct LivePlaybackRecoveryState {
    std::atomic_bool alive{true};
    std::atomic_bool recovering{false};
    std::atomic_bool retryScheduled{false};
    std::atomic_bool reconfigArmed{false};
    std::atomic_bool hardwareDecode{false};
    std::atomic_bool hybridSoftware{false};
    std::atomic_bool hybridRestoreEligible{false};
    std::atomic_bool decoderSwitching{false};
    // Hardware-only recovery must not replace the playlist until MPV has
    // confirmed that the old NVTEGRA-backed file has stopped. This prevents
    // a new load from racing an in-flight hardware decode job.
    std::atomic_bool waitingForHardwareStop{false};
    std::atomic_int pendingRecoveryAttempt{0};
    std::atomic_uint hardwareStopGeneration{0};
    std::atomic_int decoderMode{
        static_cast<int>(rumble::DecoderMode::Software)};
    std::atomic_int attempt{0};
    std::atomic_int reconfigCount{0};
    std::atomic_uint restoreGeneration{0};
    std::chrono::steady_clock::time_point lastLoadedAt{};
    std::chrono::steady_clock::time_point lastReconfigRecovery{};
    // Flap protection: when a busy channel's playlist keeps ending, resolving
    // a brand-new playlist every time hands out a different edge host and
    // restarts the stream over and over. Remember the last playlist that
    // actually opened so recovery can retry it first, and back off when
    // interruptions cluster together.
    std::string lastUrl;
    std::chrono::steady_clock::time_point lastRecoveryAt{};
    int recentRecoveries = 0;
    int reuseStreak = 0;
    std::atomic_bool backoffDone{false};
    // Set once Rumble has confirmed the broadcast ended; all further
    // recovery attempts are abandoned while the player exits.
    std::atomic_bool offlineHandled{false};
    std::atomic_bool offlineCheckRunning{false};
    std::atomic_int offlineStrikes{0};
    // Leaves the player once the stream has ended; set by the player, and
    // only called while `alive` is set.
    std::function<void()> exitPlayer;
    std::string videoId;
    std::string channelName;
};

constexpr int LIVE_RECOVERY_MAX_ATTEMPTS = 5;
constexpr int HYBRID_RESTORE_STABLE_SECONDS = 8;
constexpr int HYBRID_REARM_SECONDS = 3;

void performLivePlaybackRecovery(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    int attempt);

void resolveReplacementPlaylist(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    int attempt);

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<LivePlaybackRecoveryState>& state);

void scheduleHybridHardwareRestore(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() ||
        !state->hybridSoftware.load() ||
        !state->hybridRestoreEligible.load())
        return;

    const unsigned int generation =
        state->restoreGeneration.fetch_add(1) + 1;

    brls::Logger::info(
        "RumbleNX Hybrid: normal presentation returned; waiting {} seconds "
        "before restoring hardware decoding",
        HYBRID_RESTORE_STABLE_SECONDS);

    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0;
                 elapsed < HYBRID_RESTORE_STABLE_SECONDS &&
                 state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    generation != state->restoreGeneration.load() ||
                    state->recovering.load() ||
                    rumble::loadDecoderMode() !=
                        rumble::DecoderMode::Hybrid ||
                    !state->hybridSoftware.load() ||
                    !state->hybridRestoreEligible.load())
                    return;

                state->decoderSwitching.store(true);
                state->reconfigArmed.store(false);
                state->hybridRestoreEligible.store(false);

                auto& mpv = MPVCore::instance();
                mpv.command("set", "vd-lavc-dr", "no");
                mpv.command(
                    "set",
                    "hwdec",
                    MPVCore::PLAYER_HWDEC_METHOD.c_str());
                mpv.command("set", "pause", "no");

                brls::Logger::warning(
                    "RumbleNX Hybrid: requested hardware decoder restoration");

                ThreadPool::instance().submit(
                    [state, generation](HTTP&) {
                        for (int elapsed = 0;
                             elapsed < HYBRID_REARM_SECONDS &&
                             state->alive.load();
                             ++elapsed) {
                            std::this_thread::sleep_for(
                                std::chrono::seconds(1));
                        }

                        if (!state->alive.load()) return;

                        brls::sync([state, generation]() {
                            if (!state->alive.load() ||
                                generation !=
                                    state->restoreGeneration.load() ||
                                rumble::loadDecoderMode() !=
                                    rumble::DecoderMode::Hybrid)
                                return;

                            auto& mpv = MPVCore::instance();
                            const std::string active =
                                mpv.getString("hwdec-current");

                            state->decoderSwitching.store(false);
                            state->lastLoadedAt =
                                std::chrono::steady_clock::now();

                            if (active.empty() || active == "no") {
                                state->hybridSoftware.store(true);
                                state->hardwareDecode.store(false);
                                state->reconfigArmed.store(true);
                                mpv.command("set", "hwdec", "no");
                                brls::Logger::warning(
                                    "RumbleNX Hybrid: hardware decoder did not "
                                    "reactivate; software remains active");
                                return;
                            }

                            state->hybridSoftware.store(false);
                            state->hardwareDecode.store(true);
                            state->reconfigArmed.store(true);
#if defined(RUMBLENX_PLAYBACK_PERF_DEBUG)
                            mpv.logHardwareDecoderState(
                                "hybrid-hardware-restored");
#endif
                        });
                    });
            });
        });
}

void scheduleHardwareActivationCheck(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    rumble::DecoderMode decoderMode) {
    if (!state || !state->alive.load() ||
        !rumble::decoderUsesHardware(decoderMode) ||
        state->hybridSoftware.load())
        return;

    ThreadPool::instance().submit(
        [state, decoderMode](HTTP&) {
            for (int elapsed = 0;
                 elapsed < 8 && state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(
                    std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, decoderMode]() {
                if (!state->alive.load() ||
                    state->hybridSoftware.load() ||
                    rumble::loadDecoderMode() != decoderMode)
                    return;

                auto& mpv = MPVCore::instance();
                const std::string codec = mpv.getString("video-codec");
                const int64_t width = mpv.getInt("video-params/w");
                const int64_t height = mpv.getInt("video-params/h");
                const std::string active =
                    mpv.getString("hwdec-current");

                if (codec.empty() || width <= 0 || height <= 0) {
                    brls::Logger::info(
                        "RumbleNX: hardware activation check postponed; "
                        "no video stream is active yet "
                        "(codec='{}' size={}x{})",
                        codec,
                        static_cast<long long>(width),
                        static_cast<long long>(height));
                    return;
                }

                if (active.empty() || active == "no") {
                    brls::Logger::warning(
                        "RumbleNX: hardware decoder did not activate; "
                        "hwdec-current='{}'",
                        active.empty() ? "<empty>" : active);

                    if (decoderMode == rumble::DecoderMode::Hybrid) {
                        state->hybridSoftware.store(true);
                        state->hardwareDecode.store(false);
                        mpv.command("set", "hwdec", "no");
                        brls::Logger::warning(
                            "RumbleNX Hybrid: hardware did not activate; "
                            "continuing in software");
                    } else {
                        brls::Logger::warning(
                            "RumbleNX: experimental hardware decoding did not activate");
                    }
                    return;
                }

                state->hardwareDecode.store(true);
                brls::Logger::info(
                    "RumbleNX: hardware decoder active: {}",
                    active);
            });
        });
}

constexpr int STREAM_END_CONFIRMATIONS = 2;
constexpr long STREAM_END_EXIT_DELAY_MS = 3500;
constexpr long STREAM_END_RECHECK_MS = 3000;
constexpr long STREAM_END_POLL_MS = 20000;
// Opening the chat composer while a stream is still starting competes with
// playback for the Switch's few TLS connections (SSL connect errors, and
// sometimes a crash), so ZL and the Send message button wait for the stream
// to load and for this long after the player opens.
constexpr auto CHAT_OPEN_DELAY = std::chrono::seconds(5);

void handleStreamEnded(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
    if (!state || !state->alive.load()) return;
    if (state->offlineHandled.exchange(true)) return;

    state->retryScheduled.store(false);
    state->recovering.store(false);

    const std::string name = state->channelName.empty() ? "The stream" : state->channelName;
    brls::Logger::info("RumbleNX: {} ended; leaving the player", name);
    brls::Application::notify(name + " ended the live stream. Returning to the previous screen…");

    // `alive` is cleared when the player is torn down, so a viewer who already
    // backed out never has an unrelated screen closed underneath them.
    brls::delay(STREAM_END_EXIT_DELAY_MS, [state]() {
        if (!state->alive.load()) return;
        if (state->exitPlayer) state->exitPlayer();
        else VideoView::close(true);
    });
}

// Called whenever playback is interrupted. Brief CDN drops leave the stream
// live, so the player is only closed after Rumble reports it is no longer
// live on consecutive checks. A failed lookup never counts as ended.
void checkStreamEnded(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;
    if (state->videoId.empty()) return;

    bool expected = false;
    if (!state->offlineCheckRunning.compare_exchange_strong(expected, true)) return;

    rumble::checkVideoLiveAsync(state->videoId, [state](bool known, bool live) {
        state->offlineCheckRunning.store(false);
        if (!state->alive.load() || state->offlineHandled.load()) return;

        if (!known || live) {
            if (known) state->offlineStrikes.store(0);
            return;
        }

        if (state->offlineStrikes.fetch_add(1) + 1 >= STREAM_END_CONFIRMATIONS) {
            handleStreamEnded(state);
            return;
        }
        brls::delay(STREAM_END_RECHECK_MS, [state]() {
            checkStreamEnded(state);
        });
    });
}

// A stream that ends often just stalls without any playback event, so
// interruptions alone never notice it. Ask Rumble every 20 seconds as well,
// until the player closes or the stream is confirmed offline.
void watchStreamEnded(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
    std::weak_ptr<LivePlaybackRecoveryState> weak = state;
    brls::delay(STREAM_END_POLL_MS, [weak]() {
        auto current = weak.lock();
        if (!current || !current->alive.load() || current->offlineHandled.load()) return;
        checkStreamEnded(current);
        watchStreamEnded(current);
    });
}

void finishRecoveryFailure(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    const std::string& error) {
    state->retryScheduled.store(false);
    state->recovering.store(false);
    state->attempt.store(0);

    if (state->alive.load()) {
        brls::Logger::warning("RumbleNX could not resume the live stream: {}", error);
        checkStreamEnded(state);
    }
}

void schedulePlaybackRetry(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    const std::string& error) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;
    checkStreamEnded(state);

    bool expected = false;
    if (!state->retryScheduled.compare_exchange_strong(expected, true))
        return;

    const int nextAttempt = state->attempt.fetch_add(1) + 1;
    if (nextAttempt > LIVE_RECOVERY_MAX_ATTEMPTS) {
        finishRecoveryFailure(state, error);
        return;
    }

    const int delaySeconds = std::min(nextAttempt, 4);
    ThreadPool::instance().submit(
        [state, nextAttempt, delaySeconds](HTTP&) {
            for (int elapsed = 0;
                 elapsed < delaySeconds && state->alive.load();
                 ++elapsed) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            if (!state->alive.load()) return;

            brls::sync([state, nextAttempt]() {
                if (!state->alive.load() ||
                    !state->recovering.load())
                    return;

                state->retryScheduled.store(false);
                performLivePlaybackRecovery(state, nextAttempt);
            });
        });
}

void useSoftwareDecoder(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    const std::string& reason) {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "pause", "yes");
    mpv.command("drop-buffers");
    mpv.command("set", "vd-lavc-dr", "no");
    mpv.command("set", "hwdec", "no");

    if (state) {
        state->hybridSoftware.store(true);
        state->hardwareDecode.store(false);
        state->restoreGeneration.fetch_add(1);
    }

    brls::Logger::warning(
        "RumbleNX Hybrid: switched to software decoding ({})",
        reason);
}

void resolveReplacementPlaylist(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    // Edge Flap Protection:
    // If we have the current playlist URL and haven't exhausted edge retries yet (attempt < 2),
    // try to reload the same edge URL first. This prevents switching CDN edge hosts
    // (e.g. from use12 to use14/use15) on temporary packet drops or brief segment delays.
    if (attempt < 2 && !state->lastUrl.empty()) {
        brls::Logger::info("RumbleNX: retrying the same edge server playlist (attempt {})", attempt);
        MPVCore::BOTTOM_BAR = false;
        MPVCore::instance().setUrl(state->lastUrl, rumble::mpvExtra(true));
        return;
    }

    rumble::Video video;
    video.id = state->videoId;
    video.live = rumble::LiveStatus::Live;

    rumble::resolvePlaybackAsync(
        video,
        rumble::loadPreferredQuality(),
        [state](rumble::Playback result) {
            if (!state->alive.load() || !state->recovering.load()) return;

            if (!result.isLive()) {
                handleStreamEnded(state);
                return;
            }

            MPVCore::BOTTOM_BAR = false;
            MPVCore::instance().setUrl(
                result.selected.url,
                rumble::mpvExtra(true));

            // Recovery remains active until MPV_LOADED confirms that this
            // replacement playlist actually opened.
        },
        [state, attempt](const std::string& error) {
            if (!state->alive.load()) return;
            schedulePlaybackRetry(state, error);
        });
}

void continueHardwareRecoveryAfterStop(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
    if (!state || !state->alive.load() || !state->recovering.load())
        return;

    bool expected = true;
    if (!state->waitingForHardwareStop.compare_exchange_strong(
            expected,
            false))
        return;

    const int attempt = state->pendingRecoveryAttempt.load();
    brls::Logger::warning(
        "RumbleNX: old hardware-decoded file stopped; "
        "resolving a fresh playlist without toggling hwdec");
    resolveReplacementPlaylist(state, attempt);
}

void scheduleHardwareStopWatchdog(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    unsigned int generation) {
    ThreadPool::instance().submit(
        [state, generation](HTTP&) {
            for (int elapsed = 0; elapsed < 5 && state->alive.load(); ++elapsed)
                std::this_thread::sleep_for(std::chrono::seconds(1));

            if (!state->alive.load()) return;

            brls::sync([state, generation]() {
                if (!state->alive.load() ||
                    !state->recovering.load() ||
                    !state->waitingForHardwareStop.load() ||
                    generation != state->hardwareStopGeneration.load())
                    return;

                if (MPVCore::instance().isStopped()) {
                    brls::Logger::warning(
                        "RumbleNX: hardware stop confirmation event was delayed; "
                        "continuing after MPV reported the file stopped");
                    continueHardwareRecoveryAfterStop(state);
                    return;
                }

                state->waitingForHardwareStop.store(false);
                finishRecoveryFailure(
                    state,
                    "the hardware decoder did not stop cleanly");
                brls::Logger::error(
                    "RumbleNX: aborted hardware recovery because MPV did not "
                    "confirm a clean stop within 5 seconds");
            });
        });
}

void performLivePlaybackRecovery(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    int attempt) {
    if (!state || !state->alive.load() || state->offlineHandled.load()) return;

    const rumble::DecoderMode mode = rumble::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == rumble::DecoderMode::Hardware) {
        auto& mpv = MPVCore::instance();
        state->hardwareDecode.store(true);
        state->hybridSoftware.store(false);
        state->reconfigArmed.store(false);
        state->pendingRecoveryAttempt.store(attempt);

        // Never toggle hwdec or discard decoder buffers while NVTEGRA may
        // still be waiting on an in-flight frame. Stop the old file first and
        // load the replacement only after MPV confirms MPV_STOP.
        if (mpv.isStopped()) {
            state->waitingForHardwareStop.store(false);
            brls::Logger::warning(
                "RumbleNX: hardware-decoded file had already ended; "
                "resolving a fresh playlist");
            resolveReplacementPlaylist(state, attempt);
            return;
        }

        state->waitingForHardwareStop.store(true);
        const unsigned int generation =
            state->hardwareStopGeneration.fetch_add(1) + 1;

        brls::Logger::warning(
            "RumbleNX: stopping the old hardware-decoded file before "
            "loading its replacement");
        mpv.stop();
        scheduleHardwareStopWatchdog(state, generation);
        return;
    }

    state->waitingForHardwareStop.store(false);
    state->hardwareStopGeneration.fetch_add(1);

    if (mode == rumble::DecoderMode::Hybrid) {
        // The first dangerous format transition moves Hybrid to software.
        // Subsequent recovery events keep software active until the return to
        // the normal presentation is confirmed and stable.
        useSoftwareDecoder(
            state,
            state->hybridRestoreEligible.load()
                ? "return transition"
                : "stream format transition");
    } else {
        auto& mpv = MPVCore::instance();
        mpv.command("set", "pause", "yes");
        mpv.command("drop-buffers");
        state->hardwareDecode.store(false);
        state->hybridSoftware.store(false);
    }

    resolveReplacementPlaylist(state, attempt);
}

bool requestLivePlaybackRecovery(
    const std::shared_ptr<LivePlaybackRecoveryState>& state,
    MpvEventEnum event) {
#if defined(RUMBLENX_PLAYBACK_DEBUG)
    rumblenx::debug::log(
        "TW_RECOVERY",
        "request event=%s(%d) state=%p alive=%d recovering=%d waiting_stop=%d switching=%d",
        rumblenx::debug::playerEventName(static_cast<int>(event)),
        static_cast<int>(event),
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state && state->waitingForHardwareStop.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0);
#endif
    if (!state || !state->alive.load()) return true;
    if (state->offlineHandled.load()) return true;

    if (state->decoderSwitching.load()) {
        brls::Logger::debug(
            "RumbleNX Hybrid: ignoring decoder-generated event {}",
            static_cast<int>(event));
        return true;
    }

    if (state->waitingForHardwareStop.load()) {
        brls::Logger::debug(
            "RumbleNX: ignoring event {} while waiting for the old "
            "hardware-decoded file to stop",
            static_cast<int>(event));
        return true;
    }

    // Decoder reset itself can generate VIDEO_RECONFIG. Ignore those while a
    // recovery is already in progress instead of creating recursive retries.
    if (event == MpvEventEnum::VIDEO_RECONFIG &&
        state->recovering.load())
        return true;

    const rumble::DecoderMode mode = rumble::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    // In Hardware mode, VIDEO_RECONFIG events are suppressed to avoid tearing down
    // NVTEGRA during in-flight frames. For actual stream dropouts (END_OF_FILE,
    // MPV_FILE_ERROR), we allow hardware recovery to run via performLivePlaybackRecovery
    // which cleanly stops the hardware file before reloading, preventing premature exit
    // to the home screen when a stream briefly stalls.
    if (mode == rumble::DecoderMode::Hardware && event == MpvEventEnum::VIDEO_RECONFIG) {
        brls::Logger::warning(
            "RumbleNX Hardware: video reconfig recovery suppressed "
            "to avoid tearing down an active NVTEGRA frame");
        return true;
    }

    // A second transition while Hybrid is already in software means the
    // stream changed format again. Hardware is
    // restored only after the replacement stream has loaded and stayed stable.
    if (mode == rumble::DecoderMode::Hybrid &&
        state->hybridSoftware.load()) {
        state->hybridRestoreEligible.store(true);
    }

    bool expected = false;
    if (state->recovering.compare_exchange_strong(expected, true)) {
        state->attempt.store(0);
        state->retryScheduled.store(false);
        state->restoreGeneration.fetch_add(1);

        const auto incidentTime = std::chrono::steady_clock::now();
        if (state->lastRecoveryAt.time_since_epoch().count() != 0 &&
            incidentTime - state->lastRecoveryAt < std::chrono::seconds(60)) {
            state->recentRecoveries++;
        } else {
            state->recentRecoveries = 1;
            state->reuseStreak = 0;
        }
        state->lastRecoveryAt = incidentTime;
        state->backoffDone.store(false);

        brls::Logger::warning(
            "RumbleNX: live playback interrupted (event {}), "
            "resolving a fresh playlist",
            static_cast<int>(event));

        checkStreamEnded(state);
        performLivePlaybackRecovery(state, 0);
    } else {
        // If the replacement playlist fails before MPV_LOADED, schedule one
        // controlled retry rather than starting overlapping resolver jobs.
        schedulePlaybackRetry(
            state,
            "the replacement playlist did not open");
    }

    return true;
}

void handleVideoReconfig(
    const std::shared_ptr<LivePlaybackRecoveryState>& state) {
#if defined(RUMBLENX_PLAYBACK_DEBUG)
    rumblenx::debug::log(
        "TW_RECONFIG",
        "enter state=%p alive=%d switching=%d armed=%d recovering=%d count=%d",
        state.get(),
        state && state->alive.load() ? 1 : 0,
        state && state->decoderSwitching.load() ? 1 : 0,
        state && state->reconfigArmed.load() ? 1 : 0,
        state && state->recovering.load() ? 1 : 0,
        state ? state->reconfigCount.load() : -1);
#endif
    if (!state || !state->alive.load() ||
        state->decoderSwitching.load())
        return;

    const rumble::DecoderMode mode = rumble::loadDecoderMode();
    state->decoderMode.store(static_cast<int>(mode));

    if (mode == rumble::DecoderMode::Software) return;

    const int count = state->reconfigCount.fetch_add(1) + 1;
    const auto now = std::chrono::steady_clock::now();

    // Ignore startup/reload reconfiguration. A later format change (the
    // streamer switching resolution or encoder) is the dangerous transition.
    if (!state->reconfigArmed.load() ||
        state->lastLoadedAt.time_since_epoch().count() == 0 ||
        now - state->lastLoadedAt < std::chrono::seconds(5)) {
        brls::Logger::debug(
            "RumbleNX: ignoring startup video reconfig {}",
            count);
        return;
    }

    if (state->lastReconfigRecovery.time_since_epoch().count() != 0 &&
        now - state->lastReconfigRecovery < std::chrono::seconds(8)) {
        brls::Logger::debug(
            "RumbleNX: throttling repeated video reconfig {}",
            count);
        return;
    }

    if (mode == rumble::DecoderMode::Hardware) {
        // Diagnostic/safety behavior: do not stop, reload, drop buffers, or
        // change hwdec in response to VIDEO_RECONFIG. Any of those operations
        // can race a hardware frame currently being downloaded by MPV.
        brls::Logger::warning(
            "RumbleNX Hardware: video reconfiguration {} detected; "
            "leaving MPV/NVTEGRA untouched",
            count);
        return;
    }

    state->lastReconfigRecovery = now;

    if (mode == rumble::DecoderMode::Hybrid) {
        if (state->hybridSoftware.load()) {
            state->hybridRestoreEligible.store(true);
            brls::Logger::warning(
                "RumbleNX Hybrid: second stream format transition "
                "detected; hardware restore will be attempted after stable "
                "software playback");
        } else {
            brls::Logger::warning(
                "RumbleNX Hybrid: stream format transition detected; "
                "switching to software before consuming the new frames");
        }
    } else {
        brls::Logger::warning(
            "RumbleNX: post-startup video reconfiguration detected; "
            "resetting hardware decoder before new frames are consumed");
    }

    requestLivePlaybackRecovery(
        state,
        MpvEventEnum::VIDEO_RECONFIG);
}

const char* configuredVideoRotation() {
    switch (MPVCore::VIDEO_ROTATION) {
        case 1: return "90";
        case 2: return "180";
        case 3: return "270";
        default: return "0";
    }
}

void resetPlayerVideoLayout() {
    auto& mpv = MPVCore::instance();
    mpv.command("set", "video-rotate", configuredVideoRotation());
    mpv.command("set", "video-margin-ratio-left", "0.0");
    mpv.command("set", "video-margin-ratio-right", "0.0");
    mpv.command("set", "video-margin-ratio-top", "0.0");
    mpv.command("set", "video-margin-ratio-bottom", "0.0");
    mpv.command("set", "video-zoom", "0.0");
    mpv.command("set", "video-pan-x", "0.0");
    mpv.command("set", "video-pan-y", "0.0");
    mpv.command("set", "video-align-x", "0");
    mpv.command("set", "video-align-y", "0");
    mpv.command("set", "video-recenter", "yes");
    mpv.setAspect(MPVCore::VIDEO_ASPECT);
}

std::atomic_uint64_t nextPlaybackSession{0};
std::atomic_uint64_t activePlaybackSession{0};

}  // namespace

/// What a RemotePlayer plays besides the stream itself.
struct PlayerOptions {
    /// Numeric id of a live stream, which enables chat and live recovery;
    /// empty for every other video.
    std::string liveVideoId;
    std::string channelName;
    std::string channelPath;
    std::string channelAvatarUrl;
    std::vector<rumble::SubtitleTrack> subtitles;
    int64_t uptimeSeconds = 0;
    /// The video as listed, so settings can resolve it again.
    rumble::Video video;
};

class RemotePlayer : public brls::Box {
public:
    RemotePlayer(const remote::DirEntry& item, PlayerOptions options)
        : liveVideoId(std::move(options.liveVideoId)),
          channelPath(std::move(options.channelPath)),
          videoSubtitles(std::move(options.subtitles)),
          playerVideo(std::move(options.video)),
          playbackSessionId(++nextPlaybackSession) {
        activePlaybackSession.store(playbackSessionId);
#if defined(RUMBLENX_PLAYBACK_DEBUG)
        rumblenx::debug::log(
            "TW_PLAYER",
            "construct session=%llu live_video=%s item_type=%d item_name=%s",
            static_cast<unsigned long long>(playbackSessionId),
            this->liveVideoId.empty() ? "<not-live>" : this->liveVideoId.c_str(),
            static_cast<int>(item.type),
            item.name.c_str());
#endif
        MPVCore::LIVE_STREAM_RECOVERY = !this->liveVideoId.empty();
        resetPlayerVideoLayout();
        float width = brls::Application::contentWidth;
        float height = brls::Application::contentHeight;
        view->setDimensions(width, height);
        view->setGrow(1.0f);
        view->setId("video");
        view->setTitie(item.name);
        view->hideVideoQuality();
        view->showLoading();
        this->setDimensions(width, height);
        this->setAxis(brls::Axis::ROW);
        this->addView(view);

        VideoView::setGlobalCloseHandler([this]() {
            deactivatePlayback();
        });

        auto composeChat = [this]() {
            openChatComposer();
        };

        auto closePlayback = [this]() {
            deactivatePlayback();
            return VideoView::close(true);
        };

        this->registerAction(
            "hints/back"_i18n,
            brls::BUTTON_B,
            [this](brls::View*) {
                if (portraitLayout && portraitComposer && portraitComposer->isEmojiPanelVisible()) {
                    portraitComposer->setEmojiPanelVisible(false);
                    return true;
                }
                deactivatePlayback();
                return VideoView::close(true);
            },
            true);

        chatOverlayPanel =
            new ChatPanel(
                false,
                composeChat,
                closePlayback);
        view->setContentOverlay(chatOverlayPanel);

        chatDockedPanel =
            new ChatPanel(
                true,
                composeChat,
                closePlayback);
        this->addView(chatDockedPanel);

        if (!this->liveVideoId.empty()) {
            portraitComposer = new PortraitChatComposer(
                [this](const std::string& message) {
                    return sendPortraitMessage(message);
                });
            this->addView(portraitComposer);
        }

        chatUi = std::make_shared<ChatUiState>();

        if (this->liveVideoId.empty()) {
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
        }

        // The decoder preference applies to every video; live streams also
        // get the recovery logic below.
        const rumble::DecoderMode decoderMode =
            rumble::loadDecoderMode();
        const bool hardwareDecode =
            rumble::decoderUsesHardware(decoderMode);
#if defined(RUMBLENX_PLAYBACK_DEBUG)
        rumblenx::debug::log(
            "TW_PLAYER",
            "session=%llu decoder_mode=%d hardware=%d",
            static_cast<unsigned long long>(playbackSessionId),
            static_cast<int>(decoderMode),
            hardwareDecode ? 1 : 0);
#endif
        if (hardwareDecode) {
            MPVCore::instance().command(
                "set",
                "hwdec",
                MPVCore::PLAYER_HWDEC_METHOD.c_str());
            MPVCore::instance().command(
                "set",
                "vd-lavc-dr",
                "no");
            brls::Logger::info("RumbleNX: {} decoding enabled",
                decoderMode == rumble::DecoderMode::Hybrid ? "Hybrid" : "hardware");
        } else {
            MPVCore::instance().command("set", "hwdec", "no");
            brls::Logger::info("RumbleNX: software decoding enabled");
        }

        if (!this->channelPath.empty()) {
            channelSubscribeID =
                view->getChannelEvent()->subscribe(
                    [this]() {
                        openChannelPage();
                    });
            channelEventSubscribed = true;
            view->showChannelButton(options.channelAvatarUrl);
        }

        if (!this->liveVideoId.empty()) {
            view->setLiveMode(true, options.uptimeSeconds);
            MPVCore::LIVE_STREAM_RECOVERY = true;

            playbackRecovery =
                std::make_shared<LivePlaybackRecoveryState>();
            playbackRecovery->videoId = this->liveVideoId;
            playbackRecovery->channelName = options.channelName;
            playbackRecovery->hardwareDecode.store(hardwareDecode);
            playbackRecovery->decoderMode.store(
                static_cast<int>(decoderMode));
            playbackRecovery->hybridSoftware.store(false);
            playbackRecovery->hybridRestoreEligible.store(false);
            playbackRecovery->exitPlayer = [this]() { leaveEndedStream(); };
            watchStreamEnded(playbackRecovery);

            std::weak_ptr<LivePlaybackRecoveryState> weakRecovery =
                playbackRecovery;
            view->setPlaybackRecoveryHandler(
                [weakRecovery](MpvEventEnum event) {
                    auto state = weakRecovery.lock();
                    if (!state) return true;
                    return requestLivePlaybackRecovery(state, event);
                });

            chatPreferenceSubscribeID =
                rumble::chatPreferencesEvent()->subscribe(
                    [this](rumble::ChatPreferences preferences) {
                        applyChatPreferences(preferences);
                    });
            applyChatPreferences(
                rumble::loadChatPreferences());

            auto& orientationController =
                rumblenx::portrait::OrientationController::instance();
            orientationSubscribeID =
                orientationController.getOrientationChanged()->subscribe(
                    [this](rumblenx::portrait::DisplayOrientation orientation) {
                        applyDisplayOrientation(orientation);
                    });
            applyDisplayOrientation(orientationController.orientation());

            chatModeAction = this->registerAction(
                "Chat layout",
                brls::BUTTON_START,
                [this](brls::View*) {
                    cycleChatMode();
                    return true;
                },
                false);

        }

        if (item.type == remote::EntryType::PLAYLIST) {
            view->hideVideoProgressSlider();
        } else if (item.name.size() > 0) {
            titles.push_back(item.name);
        }

        auto& mpv = MPVCore::instance();
        eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
            if (playerShuttingDown ||
                activePlaybackSession.load() != playbackSessionId)
                return;

#if defined(RUMBLENX_PLAYBACK_DEBUG)
            rumblenx::debug::log(
                "TW_EVENT",
                "session=%llu event=%s(%d) recovery=%p recovering=%d armed=%d "
                "hw=%d hybrid_sw=%d waiting_stop=%d",
                static_cast<unsigned long long>(playbackSessionId),
                rumblenx::debug::playerEventName(static_cast<int>(event)),
                static_cast<int>(event),
                playbackRecovery.get(),
                playbackRecovery && playbackRecovery->recovering.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->reconfigArmed.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hardwareDecode.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->hybridSoftware.load() ? 1 : 0,
                playbackRecovery && playbackRecovery->waitingForHardwareStop.load() ? 1 : 0);
#endif

            auto& mpv = MPVCore::instance();
            switch (event) {
            case MpvEventEnum::MPV_LOADED: {
                streamLoaded = true;
                if (!this->liveVideoId.empty()) {
                    applyVideoLayout(currentChatPreferences);
                    if (playbackRecovery) {
                        const rumble::DecoderMode decoderMode =
                            rumble::loadDecoderMode();
                        const bool hardwareDecode =
                            rumble::decoderUsesHardware(decoderMode) &&
                            !playbackRecovery->hybridSoftware.load();

                        playbackRecovery->decoderMode.store(
                            static_cast<int>(decoderMode));
                        playbackRecovery->hardwareDecode.store(
                            hardwareDecode);
                        playbackRecovery->lastLoadedAt =
                            std::chrono::steady_clock::now();
                        {
                            const std::string loadedPath =
                                mpv.getString("path");
                            if (!loadedPath.empty())
                                playbackRecovery->lastUrl = loadedPath;
                        }
                        playbackRecovery->waitingForHardwareStop.store(false);
                        playbackRecovery->hardwareStopGeneration.fetch_add(1);
                        playbackRecovery->reconfigArmed.store(
                            decoderMode != rumble::DecoderMode::Software);

                        if (decoderMode == rumble::DecoderMode::Software) {
                            playbackRecovery->hybridSoftware.store(false);
                            playbackRecovery->hybridRestoreEligible.store(false);
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
                        } else if (decoderMode == rumble::DecoderMode::Hybrid &&
                                   playbackRecovery->hybridSoftware.load()) {
                            MPVCore::instance().command(
                                "set",
                                "hwdec",
                                "no");
#if defined(RUMBLENX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                "hybrid-software-transition");
#endif
                        } else {
#if defined(RUMBLENX_PLAYBACK_PERF_DEBUG)
                            MPVCore::instance().logHardwareDecoderState(
                                decoderMode == rumble::DecoderMode::Hybrid
                                    ? "hybrid-hardware"
                                    : "live-hardware");
#endif
                            scheduleHardwareActivationCheck(
                                playbackRecovery,
                                decoderMode);
                        }
                    }
                }

                const bool recoveryCompleted =
                    playbackRecovery &&
                    playbackRecovery->recovering.exchange(false);
                if (recoveryCompleted) {
                    playbackRecovery->retryScheduled.store(false);
                    playbackRecovery->attempt.store(0);
                    brls::Logger::info("RumbleNX: live stream resumed");
                }

                if (playbackRecovery &&
                    rumble::loadDecoderMode() == rumble::DecoderMode::Hybrid &&
                    playbackRecovery->hybridSoftware.load() &&
                    playbackRecovery->hybridRestoreEligible.load()) {
                    scheduleHybridHardwareRestore(playbackRecovery);
                }

                if (titles.empty()) this->loadList();
                mpv.command("set", "pause", "no");
                view->getProfile()->init("Local");
                // Rumble's subtitles are small WebVTT files, so every track is
                // added and the viewer picks one in player settings.
                for (const auto& sub : this->videoSubtitles) {
                    mpv.command(
                        "sub-add",
                        sub.url.c_str(),
                        "auto",
                        sub.name.c_str(),
                        sub.language.c_str());
                }
                const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
                for (auto& it : this->subtitles) {
                    mpv.command("sub-add", it.second.c_str(), flag, it.first.c_str());
                }
                break;
            }
            case MpvEventEnum::MPV_STOP:
                if (playbackRecovery &&
                    playbackRecovery->waitingForHardwareStop.load())
                    continueHardwareRecoveryAfterStop(playbackRecovery);
                break;
            case MpvEventEnum::VIDEO_RECONFIG:
                if (playbackRecovery)
                    handleVideoReconfig(playbackRecovery);
                break;
            default:;
            }
        });
        settingSubscribeID = view->getSettingEvent()->subscribe([this]() {
            PlayerContext context;
            context.video = playerVideo;
            context.live = !this->liveVideoId.empty();
            brls::View* setting = new PlayerSetting(context);
            brls::Application::pushActivity(new brls::Activity(setting));
        });
    }

    void deactivatePlayback() {
        if (playerShuttingDown) return;
        playerShuttingDown = true;
        if (playerAlive) playerAlive->store(false);

        VideoView::setGlobalCloseHandler(nullptr);

        if (activePlaybackSession.load() == playbackSessionId)
            activePlaybackSession.store(0);

        MPVCore::LIVE_STREAM_RECOVERY = false;
        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatUi) chatUi->alive.store(false);
        stopChat();
        resetPlayerVideoLayout();

        auto& mpv = MPVCore::instance();
        mpv.stop();
        const char* configuredHwdec = MPVCore::HARDWARE_DEC
            ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
            : "no";
        mpv.command("set", "hwdec", configuredHwdec);
    }

    void deactivatePlaybackForNavigation() {
        deactivatePlayback();
    }

    // Called once Rumble confirms the stream ended. VideoView::close pops
    // whatever is on top, so with player settings, the chat composer or a
    // dialog open it closed that instead and left the dead player behind.
    // Close those first, one per pass, then the player itself.
    void leaveEndedStream() {
        if (playerShuttingDown) return;
        brls::Activity* own = this->getParentActivity();
        const auto stack = brls::Application::getActivitiesStack();
        if (!own || stack.empty()) return;

        if (stack.back() != own) {
            brls::Application::popActivity(brls::TransitionAnimation::NONE);
            std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
            // Give the popped screen time to be removed before looking again;
            // popping twice in one go would pop the same screen twice.
            brls::delay(300, [this, weakAlive]() {
                auto alive = weakAlive.lock();
                if (!alive || !alive->load()) return;
                leaveEndedStream();
            });
            return;
        }

        deactivatePlayback();
        VideoView::close(true);
    }

    ~RemotePlayer() override {
        if (playerAlive) playerAlive->store(false);
        deactivatePlayback();
        if (asyncCancelToken)
            asyncCancelToken->store(true);
#if defined(RUMBLENX_PLAYBACK_DEBUG)
        rumblenx::debug::log(
            "TW_PLAYER",
            "destruct session=%llu live_video=%s owns=%d",
            static_cast<unsigned long long>(playbackSessionId),
            this->liveVideoId.empty() ? "<not-live>" : this->liveVideoId.c_str(),
            activePlaybackSession.load() == playbackSessionId ? 1 : 0);
#endif
        playerShuttingDown = true;
        const bool ownsPlayback =
            activePlaybackSession.load() == playbackSessionId;
        if (ownsPlayback)
            activePlaybackSession.store(0);

        if (ownsPlayback && !this->liveVideoId.empty()) {
            auto resetPreferences = currentChatPreferences;
            resetPreferences.mode = rumble::ChatMode::Off;
            applyVideoLayout(resetPreferences);
        }

        if (ownsPlayback) {
            MPVCore::LIVE_STREAM_RECOVERY = false;
            resetPlayerVideoLayout();

            const char* configuredHwdec = MPVCore::HARDWARE_DEC
                ? MPVCore::PLAYER_HWDEC_METHOD.c_str()
                : "no";
            MPVCore::instance().command(
                "set",
                "hwdec",
                configuredHwdec);
        }

        if (playbackRecovery) {
            playbackRecovery->alive.store(false);
            playbackRecovery->recovering.store(false);
            playbackRecovery->retryScheduled.store(false);
            playbackRecovery->reconfigArmed.store(false);
            playbackRecovery->decoderSwitching.store(false);
            playbackRecovery->waitingForHardwareStop.store(false);
            playbackRecovery->hardwareStopGeneration.fetch_add(1);
            playbackRecovery->restoreGeneration.fetch_add(1);
        }
        view->setPlaybackRecoveryHandler({});

        if (chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }

        if (chatModeAction != ACTION_NONE) {
            this->unregisterAction(chatModeAction);
            chatModeAction = ACTION_NONE;
        }

        if (chatUi) chatUi->alive.store(false);
        chatClient.reset();
        if (!this->liveVideoId.empty()) {
            rumble::chatPreferencesEvent()->unsubscribe(
                chatPreferenceSubscribeID);
            if (orientationSubscribeID) {
                rumblenx::portrait::OrientationController::instance()
                    .getOrientationChanged()->unsubscribe(*orientationSubscribeID);
                orientationSubscribeID.reset();
            }
        }

        if (portraitLayout) {
            brls::Application::setContentOrientation(
                brls::ContentOrientation::LANDSCAPE);
            portraitLayout = false;
        }

        auto& mpv = MPVCore::instance();
        mpv.getEvent()->unsubscribe(eventSubscribeID);
        view->getPlayEvent()->unsubscribe(playSubscribeID);
        if (channelEventSubscribed)
            view->getChannelEvent()->unsubscribe(
                channelSubscribeID);
        view->getSettingEvent()->unsubscribe(settingSubscribeID);
        if (this->liveVideoId.empty()) {
            mpv.command("write-watch-later-config");
        } else {
            brls::Logger::debug(
                "RumbleNX: skipped watch-later write for live playback");
        }
    }

    void willDisappear(bool resetState) override {
        deactivatePlayback();
#ifdef ANDROID
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
#else
        restoreClearColor();
#endif
    }

    void willAppear(bool resetState) override {
#ifdef ANDROID
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
#else
        // While MPV has no frame to present (initial load, live reconnects)
        // the window clear colour shows through. Keep it black instead of the
        // theme grey so buffering never flashes a grey screen.
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 255));
#endif
        if (!this->liveVideoId.empty())
            restorePlaybackFocus();
    }

    static void restoreClearColor() {
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGB(235, 235, 235));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGB(45, 45, 45));
    }

    void setList(const DirList& list, size_t index, const std::string& extra) {
        // æ’­æ”¾åˆ—è¡¨
        DirList urls;
        for (size_t i = 1; i < list.size(); i++) {
            auto& it = list.at(i);
            if (it.type == remote::EntryType::VIDEO) {
                if (i == index) index = urls.size();
                titles.push_back(it.name);
                urls.push_back(it);
            }
        }
        if (titles.size() > 1) view->setList(titles, index);

        playSubscribeID = view->getPlayEvent()->subscribe([this, list, urls, extra](int index) {
            if (index < 0 || index >= (int)urls.size()) {
                return VideoView::close(true);
            }
            MPVCore::instance().reset();
            auto& item = urls.at(index);

            std::string name = item.name;
            auto pos = name.find_last_of(".");
            if (pos != std::string::npos) {
                name = name.substr(0, pos);
            }

            this->subtitles.clear();
            for (auto& s : list) {
                if (s.type == remote::EntryType::SUBTITLE) {
                    if (!s.name.rfind(name, 0)) {
                        this->subtitles.insert(std::make_pair(s.name.substr(pos), s.url()));
                    }
                }
            }
            this->url = item.url();
            MPVCore::instance().setUrl(this->url, extra);
            view->setTitie(name);
            return true;
        });

        view->getPlayEvent()->fire(index);
    }

    uint64_t getPlaybackSessionId() const { return playbackSessionId; }

    void setPlaybackCancelToken(std::shared_ptr<std::atomic_bool> cancel) {
        asyncCancelToken = std::move(cancel);
    }

    void setTitle(const std::string& title) {
        if (view) view->setTitie(title);
    }

    void showLoading(const std::string& note = "") {
        if (view) {
            if (!note.empty()) view->setLoadingNote(note);
            view->showLoading();
        }
    }

    void setUrl(const std::string& path, const std::string& extra = "") {
        if (path.empty()) return;
        playSubscribeID = view->getPlayEvent()->subscribe([](int index) { return VideoView::close(true); });
        MPVCore::instance().command("set", "pause", "no");
        MPVCore::instance().setUrl(path, extra);
    }

    void loadList() {
        auto& mpv = MPVCore::instance();
        int64_t count = mpv.getInt("playlist-count");
        for (int64_t n = 0; n < count; n++) {
            auto key = fmt::format("playlist/{}/title", n);
            titles.push_back(mpv.getString(key));
        }
        if (titles.size() > 1) view->setList(titles, 0);
        view->setTitie(titles.front());

        playSubscribeID = view->getPlayEvent()->subscribe([this, &mpv](int index) {
            if (index < 0 || index >= (int)titles.size()) {
                return VideoView::close();
            }
            MPVCore::instance().reset();
            view->setTitie(titles.at(index));
            mpv.command("playlist-play-index", std::to_string(index).c_str());
            return true;
        });
    }


    void openChannelPage() {
        if (channelPath.empty() || navigatingToChannel) return;
        navigatingToChannel = true;

        const std::string channel = channelPath;
        deactivatePlaybackForNavigation();

        if (!brls::Application::popActivity(
                brls::TransitionAnimation::NONE)) {
            navigatingToChannel = false;
            return;
        }

        // Push on the next UI turn so the player activity and VideoView are
        // fully destroyed before the channel page becomes active.
        brls::sync([channel]() {
            brls::Application::pushActivity(
                new brls::Activity(
                    new RumbleChannelPage(channel)),
                brls::TransitionAnimation::NONE);
        });
    }

    void restorePlaybackFocus() {
        std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
        brls::sync([this, weakAlive]() {
            auto alive = weakAlive.lock();
            if (!alive || !alive->load()) return;
            if (!playerShuttingDown &&
                activePlaybackSession.load() == playbackSessionId)
                brls::Application::giveFocus(view);
        });
    }

    void cycleChatMode() {
        auto preferences =
            rumble::loadChatPreferences();

        const char* description = "Off";
        switch (preferences.mode) {
        case rumble::ChatMode::Off:
            preferences.mode =
                rumble::ChatMode::Overlay;
            description = "Overlay";
            break;

        case rumble::ChatMode::Overlay:
            preferences.mode =
                rumble::ChatMode::RightPanel;
            description = "Right panel";
            break;

        case rumble::ChatMode::RightPanel:
        default:
            preferences.mode =
                rumble::ChatMode::Off;
            description = "Off";
            break;
        }

        if (!rumble::saveChatPreferences(
                preferences)) {
            brls::Application::notify(
                "RumbleNX: could not change chat layout");
            return;
        }

        brls::Application::notify(
            std::string("Chat layout: ") +
            description);
    }

    void updateChatInteractionAction(
        const rumble::ChatPreferences& preferences) {
        const bool interactive =
            preferences.participation ==
            rumble::ChatParticipation::Interactive;

        if (interactive &&
            chatSendAction == ACTION_NONE) {
            chatSendAction = this->registerAction(
                "Chat",
                brls::BUTTON_LT,
                [this](brls::View*) {
                    openChatComposer();
                    return true;
                },
                false);
        } else if (!interactive &&
            chatSendAction != ACTION_NONE) {
            this->unregisterAction(chatSendAction);
            chatSendAction = ACTION_NONE;
        }
    }


    bool sendPortraitMessage(const std::string& message) {
        const auto preferences = rumble::loadChatPreferences();
        if (preferences.participation !=
            rumble::ChatParticipation::Interactive) {
            brls::Application::notify(
                "Enable Interactive under Chat participation first");
            return false;
        }
        if (!rumble::canSendChat()) {
            brls::Application::notify(
                "Sign in to Rumble on the Home screen to chat");
            return false;
        }
        if (message.empty()) return false;

        rumble::sendChatMessageAsync(
            liveVideoId,
            message,
            [](const std::string&) {
                brls::Application::notify("RumbleNX: chat message sent");
            },
            [](const std::string& error) {
                brls::Application::notify("RumbleNX chat error: " + error);
            });
        return true;
    }

    void openChatComposer() {
        if (!streamLoaded ||
            std::chrono::steady_clock::now() - openedAt <
                CHAT_OPEN_DELAY) {
            brls::Application::notify(
                "Chat opens once the stream has started");
            return;
        }

        const auto preferences =
            rumble::loadChatPreferences();

        if (preferences.participation !=
            rumble::ChatParticipation::Interactive) {
            brls::Application::notify(
                "Enable Interactive under Chat participation first");
            return;
        }

        if (!rumble::canSendChat()) {
            brls::Application::notify(
                "Sign in to Rumble on the Home screen to chat");
            return;
        }

        if (preferences.composerMode ==
            rumble::ChatComposerMode::NintendoKeyboard) {
            auto* ime =
                brls::Application::getImeManager();
            if (!ime) {
                brls::Application::notify(
                    "The software keyboard is unavailable");
                return;
            }

            std::weak_ptr<std::atomic_bool> weakAlive = playerAlive;
            const std::string videoId = liveVideoId;
            ime->openForText(
                [this, weakAlive, videoId](std::string message) {
                    // The keyboard can return after the stream was closed and
                    // this player destroyed; never touch it in that case.
                    auto alive = weakAlive.lock();
                    const bool playerExists = alive && alive->load();
                    if (playerExists) restorePlaybackFocus();

                    if (message.empty()) return;

                    brls::Application::notify(
                        "RumbleNX: sending chat message…");

                    rumble::sendChatMessageAsync(
                        videoId,
                        message,
                        [](const std::string&) {
                            brls::Application::notify(
                                "RumbleNX: chat message sent");
                        },
                        [](const std::string& error) {
                            brls::Application::notify(
                                "RumbleNX chat error: " + error);
                        });
                },
                "Rumble chat",
                "Send a message as your signed-in Rumble account",
                static_cast<int>(rumble::chatMessageLimit()),
                "");
            return;
        }

        brls::Application::pushActivity(
            new brls::Activity(
                new RumbleChatComposer(
                    liveVideoId)));
    }

    void stopChat() {
        chatClient.reset();
        if (chatUi) {
            chatUi->setStatus("Chat disabled");
        }
    }

    void startChat() {
        if (chatClient || liveVideoId.empty()) return;

        std::weak_ptr<ChatUiState> weak = chatUi;
        chatClient = std::make_unique<rumble::ChatClient>(
            liveVideoId,
            [weak](rumble::ChatMessage message) {
                brls::sync([weak, message = std::move(message)]() mutable {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->append(std::move(message));
                });
            },
            [weak](const std::string& status) {
                brls::sync([weak, status]() {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->setStatus(status);
                });
            },
            [weak](std::vector<std::string> ids) {
                brls::sync([weak, ids = std::move(ids)]() {
                    auto state = weak.lock();
                    if (!state || !state->alive.load()) return;
                    state->remove(ids);
                });
            });
    }

    void applyDisplayOrientation(
        rumblenx::portrait::DisplayOrientation orientation) {
        portraitOrientation = orientation;
        portraitLayout =
            orientation != rumblenx::portrait::DisplayOrientation::Landscape;

        brls::Application::setContentOrientation(
            orientation == rumblenx::portrait::DisplayOrientation::PortraitClockwise
                ? brls::ContentOrientation::PORTRAIT_CLOCKWISE
                : orientation == rumblenx::portrait::DisplayOrientation::PortraitCounterClockwise
                    ? brls::ContentOrientation::PORTRAIT_COUNTER_CLOCKWISE
                    : brls::ContentOrientation::LANDSCAPE);

        if (!portraitLayout && portraitComposer)
            portraitComposer->setEmojiPanelVisible(false);

        applyChatPreferences(currentChatPreferences);

        if (!portraitLayout && !playerShuttingDown)
            brls::Application::giveFocus(view);
    }

    void applyVideoLayout(
        const rumble::ChatPreferences& preferences) {
        const rumble::ChatMode mode = preferences.mode;
        auto& mpv = MPVCore::instance();

        if (portraitLayout) {
            const float occupied = std::clamp(
                portraitVideoHeight /
                    std::max(1.0f, brls::Application::contentHeight),
                0.0f,
                1.0f);
            const float unused = 1.0f - occupied;
            const bool clockwise =
                portraitOrientation ==
                rumblenx::portrait::DisplayOrientation::PortraitClockwise;

            const std::string left =
                fmt::format("{:.6f}", clockwise ? unused : 0.0f);
            const std::string right =
                fmt::format("{:.6f}", clockwise ? 0.0f : unused);

            mpv.command("set", "video-margin-ratio-left", left.c_str());
            mpv.command("set", "video-margin-ratio-right", right.c_str());
            mpv.command("set", "video-margin-ratio-top", "0.0");
            mpv.command("set", "video-margin-ratio-bottom", "0.0");
            mpv.command("set", "video-rotate", clockwise ? "90" : "270");
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-zoom", "0.0");
            mpv.command("set", "video-pan-x", "0.0");
            mpv.command("set", "video-pan-y", "0.0");
            mpv.command("set", "video-align-x", "0");
            mpv.command("set", "video-align-y", "0");
            mpv.command("set", "video-recenter", "yes");
            return;
        }

        mpv.command("set", "video-rotate", configuredVideoRotation());

        // MPV renders directly into the full Switch framebuffer. Making the
        // Borealis VideoView narrower changes the OSD bounds, but does not by
        // itself constrain MPV's video rectangle. Reserve the docked chat area
        // through MPV's own window-relative video margin.
        const float contentWidth = brls::Application::contentWidth;
        const float rightMargin =
            mode == rumble::ChatMode::RightPanel && contentWidth > 0.0f
                ? static_cast<float>(preferences.dockedWidth) /
                    contentWidth
                : 0.0f;

        const std::string rightMarginValue =
            fmt::format("{:.6f}", rightMargin);

        mpv.command(
            "set",
            "video-margin-ratio-left",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-right",
            rightMarginValue.c_str());
        mpv.command(
            "set",
            "video-margin-ratio-top",
            "0.0");
        mpv.command(
            "set",
            "video-margin-ratio-bottom",
            "0.0");

        mpv.command("set", "video-zoom", "0.0");
        mpv.command("set", "video-pan-x", "0.0");
        mpv.command("set", "video-pan-y", "0.0");
        mpv.command("set", "video-align-y", "0");
        mpv.command("set", "video-recenter", "yes");

        if (mode == rumble::ChatMode::RightPanel) {
            // Fit the complete source image into the remaining area. Any
            // aspect-ratio mismatch becomes black letterboxing rather than
            // cropping the stream behind the chat panel.
            mpv.command("set", "keepaspect", "yes");
            mpv.command("set", "keepaspect-window", "no");
            mpv.command("set", "video-aspect-override", "no");
            mpv.command("set", "panscan", "0.0");
            mpv.command("set", "video-align-x", "-1");

            brls::Logger::info(
                "RumbleNX: docked video fit enabled, right margin ratio {}",
                rightMarginValue);
        } else {
            // Restore the user's normal player aspect choice when leaving the
            // dedicated docked layout.
            mpv.command("set", "video-align-x", "0");
            mpv.setAspect(MPVCore::VIDEO_ASPECT);

            brls::Logger::info(
                "RumbleNX: docked video fit disabled");
        }
    }

    void applyChatPreferences(const rumble::ChatPreferences& preferences) {
        if (playerShuttingDown ||
            activePlaybackSession.load() != playbackSessionId ||
            liveVideoId.empty() ||
            !chatOverlayPanel ||
            !chatDockedPanel)
            return;

        currentChatMode = preferences.mode;
        currentChatPreferences = preferences;

        chatOverlayPanel->apply(preferences);
        chatDockedPanel->apply(preferences);
        updateChatInteractionAction(preferences);

        const float width = brls::Application::contentWidth;
        const float height = brls::Application::contentHeight;

        if (portraitLayout) {
            this->setAxis(brls::Axis::COLUMN);
            this->setDimensions(width, height);
            portraitVideoHeight = std::min(width * 9.0f / 16.0f, height * 0.42f);
            const float composerHeight = std::min(485.0f, height * 0.42f);
            const float chatHeight = std::max(
                240.0f,
                height - portraitVideoHeight - composerHeight);
            view->setGrow(0.0f);
            view->setDimensions(width, portraitVideoHeight);
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            chatDockedPanel->setPortraitFrame(width, chatHeight);
            chatUi->activate(chatDockedPanel);
            if (portraitComposer) {
                portraitComposer->setFrameSize(width, composerHeight);
                portraitComposer->setVisibility(brls::Visibility::VISIBLE);
            }
            startChat();
            applyVideoLayout(preferences);
            this->invalidate();
            return;
        }

        this->setAxis(brls::Axis::ROW);
        this->setDimensions(width, height);
        if (portraitComposer)
            portraitComposer->setVisibility(brls::Visibility::GONE);
        view->setGrow(1.0f);
        applyVideoLayout(preferences);

        switch (preferences.mode) {
        case rumble::ChatMode::Off:
            chatUi->activate(nullptr);
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            view->setDimensions(width, height);
            stopChat();
            break;

        case rumble::ChatMode::RightPanel:
            // TV layout: the complete video is scaled into the remaining
            // width, and chat occupies its own column rather than covering it.
            chatOverlayPanel->setVisibility(brls::Visibility::GONE);
            chatDockedPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(
                std::max(
                    640.0f,
                    width -
                        static_cast<float>(
                            preferences.dockedWidth)),
                height);
            chatUi->activate(chatDockedPanel);
            startChat();
            break;

        case rumble::ChatMode::Overlay:
            chatDockedPanel->setVisibility(brls::Visibility::GONE);
            chatOverlayPanel->setVisibility(brls::Visibility::VISIBLE);
            view->setDimensions(width, height);
            chatUi->activate(chatOverlayPanel);
            startChat();
            break;
        }

        this->invalidate();
    }

private:
    VideoView* view = new VideoView();
    std::string url;
    std::string liveVideoId;
    std::string channelPath;
    std::vector<std::string> titles;
    std::unordered_map<std::string, std::string> subtitles;
    std::vector<rumble::SubtitleTrack> videoSubtitles;
    rumble::Video playerVideo;
    MPVEvent::Subscription eventSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    brls::VoidEvent::Subscription channelSubscribeID;
    bool channelEventSubscribed = false;
    brls::Event<rumble::ChatPreferences>::Subscription chatPreferenceSubscribeID;
    std::optional<brls::Event<rumblenx::portrait::DisplayOrientation>::Subscription>
        orientationSubscribeID;
    ChatPanel* chatOverlayPanel = nullptr;
    ChatPanel* chatDockedPanel = nullptr;
    PortraitChatComposer* portraitComposer = nullptr;
    std::shared_ptr<ChatUiState> chatUi;
    std::shared_ptr<LivePlaybackRecoveryState> playbackRecovery;
    std::unique_ptr<rumble::ChatClient> chatClient;
    rumble::ChatMode currentChatMode = rumble::ChatMode::Off;
    rumble::ChatPreferences currentChatPreferences;
    brls::ActionIdentifier chatSendAction = ACTION_NONE;
    brls::ActionIdentifier chatModeAction = ACTION_NONE;
    uint64_t playbackSessionId = 0;
    bool portraitLayout = false;
    float portraitVideoHeight = 405.0f;
    rumblenx::portrait::DisplayOrientation portraitOrientation =
        rumblenx::portrait::DisplayOrientation::Landscape;
    bool playerShuttingDown = false;
    bool streamLoaded = false;
    const std::chrono::steady_clock::time_point openedAt =
        std::chrono::steady_clock::now();
    // Shared with asynchronous callbacks (e.g. the Nintendo software keyboard)
    // so they can tell whether this player still exists before touching it.
    std::shared_ptr<std::atomic_bool> playerAlive =
        std::make_shared<std::atomic_bool>(true);
    bool navigatingToChannel = false;
    std::shared_ptr<std::atomic_bool> asyncCancelToken;
};

namespace {

std::atomic_bool resolvingVideo{false};

void showPlaybackError(const std::string& message) {
    auto* dialog = new brls::Dialog(message);
    dialog->addButton("hints/ok"_i18n, []() {});
    dialog->open();
}

/// "2026-11-02 17:00 UTC" from Rumble's ISO 8601 times.
std::string scheduleText(const std::string& iso) {
    if (iso.size() < 16 || iso[10] != 'T') return iso;
    const int64_t when = rumble::parse::parseIsoTime(iso);
    if (when <= 0) return iso;
    const std::time_t seconds = static_cast<std::time_t>(when);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M UTC", &utc);
    return buffer;
}

void openPlayer(const rumble::Video& listing, rumble::Playback playback) {
    const bool live = playback.isLive() && !playback.videoId.empty();
    // Every launch starts from a clean global MPV presentation, so a live
    // chat margin or recovery session never leaks into another video.
    MPVCore::LIVE_STREAM_RECOVERY = live;
    MPVCore::instance().stop();
    MPVCore::instance().command("set", "pause", "no");
    resetPlayerVideoLayout();

    PlayerOptions options;
    options.liveVideoId = live ? playback.videoId : std::string();
    options.channelName = playback.channelName;
    options.channelPath = playback.channelPath;
    options.channelAvatarUrl = playback.channelAvatarUrl;
    options.subtitles = playback.subtitles;
    options.uptimeSeconds = playback.uptimeSeconds;
    options.video = listing;
    if (!playback.videoId.empty()) options.video.id = playback.videoId;
    if (options.video.url.empty()) options.video.url = playback.pageUrl;
    options.video.live = live ? rumble::LiveStatus::Live : rumble::LiveStatus::None;

    // Like the Rumble site, the OSD shows the channel and the video title.
    const std::string title = playback.channelName.empty() || playback.title.empty()
        ? (playback.title.empty() ? playback.channelName : playback.title)
        : playback.channelName + " · " + playback.title;

    RemotePlayer* player = new RemotePlayer({remote::EntryType::VIDEO, title, ""}, std::move(options));
    brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);
    if (live) MPVCore::BOTTOM_BAR = false;
    player->setUrl(playback.selected.url, rumble::mpvExtra(live));
}

}  // namespace

void RemoteView::playVideo(const rumble::Video& video) {
    // One resolution at a time; repeated presses while it runs are ignored.
    if (resolvingVideo.exchange(true)) return;
    brls::Application::notify(video.isLive() ? "Opening live stream…" : "Opening video…");

    rumble::resolvePlaybackAsync(
        video,
        rumble::loadPreferredQuality(),
        [video](rumble::Playback playback) {
            resolvingVideo.store(false);
            if (playback.kind == rumble::PlaybackKind::Upcoming) {
                std::string message = "This live stream has not started yet.";
                if (!video.scheduledAt.empty()) message += "\n\nScheduled for " + scheduleText(video.scheduledAt) + ".";
                if (!playback.message.empty()) message += "\n\n" + playback.message;
                showPlaybackError(message);
                return;
            }
            openPlayer(video, std::move(playback));
        },
        [](const std::string& error) {
            resolvingVideo.store(false);
            showPlaybackError("Could not play this video:\n" + error);
        });
}
