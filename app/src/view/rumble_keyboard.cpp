#include "view/rumble_keyboard.hpp"

#include "utils/haptics.hpp"

#include <utility>

namespace {

float keyGrow(const std::string& key) {
    if (key == "Space") return 3.8f;
    if (key == "Send") return 2.0f;
    if (key == "Emoji" || key == "Caps") return 1.4f;
    return 1.0f;
}

const NVGcolor EMOJI_GREEN = nvgRGB(72, 110, 33);

}  // namespace

RumbleKeyboard::RumbleKeyboard(Callbacks callbacks, const Options& options)
    : callbacks(std::move(callbacks)), options(options) {
    alphaLayout = {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
        {"a", "s", "d", "f", "g", "h", "j", "k", "l", "'"},
        {"Shift", "z", "x", "c", "v", "b", "n", "m", "Back"},
    };
    symbolLayout = {
        {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
        {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
        {"-", "_", "=", "+", "[", "]", "{", "}", "\\", "/"},
        {"ABC", ":", ";", "\"", "'", "<", ">", "?", "Back"},
    };
    if (options.sendKey) {
        alphaLayout.push_back({"#+=", "Emoji", "Space", ".", "Send"});
        symbolLayout.push_back({"ABC", "Emoji", "Space", ".", "Send"});
    } else {
        // Caps Lock sits bottom-left, below Shift, like a hardware keyboard.
        alphaLayout.push_back({"Caps", "#+=", "Emoji", "Space", "."});
        symbolLayout.push_back({"Caps", "ABC", "Emoji", "Space", "."});
    }

    this->setAxis(brls::Axis::COLUMN);
    this->setWidthPercentage(100.0f);
    buildRows();
    refreshKeys();
}

void RumbleKeyboard::resetShift() {
    if (!shifted) return;
    shifted = false;
    refreshKeys();
}

brls::View* RumbleKeyboard::getStartKey() {
    for (auto& slot : slots) {
        if (slot.row == 1 && slot.column == 0) return slot.button;
    }
    return this;
}

const std::vector<std::vector<std::string>>& RumbleKeyboard::currentLayout() const {
    return symbols ? symbolLayout : alphaLayout;
}

void RumbleKeyboard::buildRows() {
    for (size_t rowIndex = 0; rowIndex < alphaLayout.size(); ++rowIndex) {
        const auto& keys = alphaLayout[rowIndex];
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setWidthPercentage(100.0f);
        row->setHeight(options.keyHeight);
        row->setMarginBottom(rowIndex + 1 == alphaLayout.size() ? 0.0f : options.spacing);
        this->addView(row);

        for (size_t column = 0; column < keys.size(); ++column) {
            const std::string& label = keys[column];
            auto* key = new brls::Button();
            key->setHeight(options.keyHeight);
            key->setGrow(keyGrow(label));
            key->setMarginRight(column + 1 == keys.size() ? 0.0f : options.spacing);
            key->setStyle(label == "Send" ? &brls::BUTTONSTYLE_PRIMARY : &brls::BUTTONSTYLE_DEFAULT);
            if (label == "Emoji") key->setTextColor(nvgRGB(255, 255, 255));
            // A drop shadow would show through see-through keys as a dark blob.
            if (options.translucent) key->setShadowType(brls::ShadowType::NONE);
            key->setFontSize(options.fontSize);
            key->registerClickAction(
                [this, rowIndex, column](brls::View*) {
                    pressKey(rowIndex, column);
                    return true;
                });
            key->addGestureRecognizer(new brls::TapGestureRecognizer(key));
            row->addView(key);
            slots.push_back({key, rowIndex, column});
        }
    }
}

void RumbleKeyboard::refreshKeys() {
    const auto& layout = currentLayout();
    for (auto& slot : slots) {
        std::string label = layout[slot.row][slot.column];

        NVGcolor background = options.translucent
            ? nvgRGBA(40, 44, 56, 150)
            : brls::Application::getTheme()["brls/button/default_enabled_background"];
        if (label == "Emoji") {
            background = options.translucent ? nvgRGBA(72, 110, 33, 220) : EMOJI_GREEN;
        } else if (label == "Caps" && capsLock) {
            background = options.translucent ? nvgRGBA(105, 116, 140, 210) : nvgRGB(105, 116, 140);
        }
        if (label != "Send") slot.button->setBackgroundColor(background);

        if (label == "Caps" && capsLock) label = "CAPS";
        if (!symbols && upperCase() && label.size() == 1 &&
            label[0] >= 'a' && label[0] <= 'z')
            label[0] = static_cast<char>(label[0] - 'a' + 'A');
        slot.button->setText(label);
    }
}

void RumbleKeyboard::pressKey(size_t row, size_t column) {
    rumblenx::haptics::keyboardPulse();
    const std::string key = currentLayout()[row][column];

    if (key == "Shift") {
        shifted = !shifted;
        refreshKeys();
    } else if (key == "Caps") {
        capsLock = !capsLock;
        shifted = false;
        refreshKeys();
    } else if (key == "#+=" || key == "ABC") {
        symbols = !symbols;
        shifted = false;
        refreshKeys();
    } else if (key == "Back") {
        if (callbacks.backspace) callbacks.backspace();
    } else if (key == "Space") {
        if (callbacks.text) callbacks.text(" ");
    } else if (key == "Emoji") {
        if (callbacks.emoji) callbacks.emoji();
    } else if (key == "Send") {
        if (callbacks.send) callbacks.send();
    } else {
        std::string value = key;
        if (!symbols && upperCase() && value.size() == 1 &&
            value[0] >= 'a' && value[0] <= 'z') {
            value[0] = static_cast<char>(value[0] - 'a' + 'A');
            if (shifted) {
                shifted = false;
                refreshKeys();
            }
        }
        if (callbacks.text) callbacks.text(value);
    }
}
