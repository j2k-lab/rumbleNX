//
// Created by fang on 2023/3/4.
//

#pragma once

#include "api/rumble_types.hpp"

#include <borealis.hpp>
#include <string>

class ButtonClose;

/// What the player is showing, so settings can reload it.
struct PlayerContext {
    rumble::Video video;
    bool live = false;
};

class PlayerSetting : public brls::Box {
public:
    explicit PlayerSetting(PlayerContext context = {});
    ~PlayerSetting() override;

    bool isTranslucent() override { return true; }

    View* getDefaultFocus() override { return this->settings->getDefaultFocus(); }

    inline static int selectedSubtitle = 0;
    inline static int selectedAudio = 0;

    enum class Equalizer {
        BRIGHTNESS,
        CONTRAST,
        SATURATION,
        HUE,
        GAMMA,
    };

private:
    BRLS_BIND(brls::ScrollingFrame, settings, "player/settings");
    BRLS_BIND(brls::Box, cancel, "player/cancel");

    BRLS_BIND(brls::SelectorCell, subtitleTrack, "setting/track/subtitle");
    BRLS_BIND(brls::SelectorCell, audioTrack, "setting/track/audio");
    BRLS_BIND(brls::SelectorCell, btnRumbleQuality, "setting/rumble/quality");
    BRLS_BIND(brls::SelectorCell, btnRumbleDecoder, "setting/rumble/decoder");
    BRLS_BIND(brls::SelectorCell, btnPortraitOrientation, "setting/portrait/orientation");
    BRLS_BIND(brls::SelectorCell, btnKeyboardHaptic, "setting/portrait/keyboard_haptic");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatMode, "setting/rumble/chat_mode");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatParticipation, "setting/rumble/chat_participation");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatComposer, "setting/rumble/chat_composer");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatOverlaySize, "setting/rumble/chat_overlay_size");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatOverlayPosition, "setting/rumble/chat_overlay_position");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatDockedWidth, "setting/rumble/chat_docked_width");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatOverlayWidth, "setting/rumble/chat_overlay_width");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatFont, "setting/rumble/chat_font");
    BRLS_BIND(brls::SelectorCell, btnRumbleChatOpacity, "setting/rumble/chat_opacity");
    BRLS_BIND(brls::BooleanCell, btnRumbleChatTimestamps, "setting/rumble/chat_timestamps");
    BRLS_BIND(brls::SliderCell, btnSubsync, "setting/video/subsync");
    // equalizer setting
    BRLS_BIND(brls::RadioCell, btnEqualizerReset, "setting/equalizer/reset");
    BRLS_BIND(brls::SliderCell, btnEqualizerBrightness, "setting/equalizer/brightness");
    BRLS_BIND(brls::SliderCell, btnEqualizerContrast, "setting/equalizer/contrast");
    BRLS_BIND(brls::SliderCell, btnEqualizerSaturation, "setting/equalizer/saturation");
    BRLS_BIND(brls::SliderCell, btnEqualizerGamma, "setting/equalizer/gamma");
    BRLS_BIND(brls::SliderCell, btnEqualizerHue, "setting/equalizer/hue");

    void setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue);

    void registerHideBackground(brls::View* view);
};
