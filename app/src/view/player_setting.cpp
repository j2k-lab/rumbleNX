#include "api/rumble.hpp"
#include "api/rumble_chat.hpp"
#include "utils/config.hpp"
#include "utils/event.hpp"
#include "utils/haptics.hpp"
#include "utils/orientation.hpp"
#include "utils/thread.hpp"
#include "view/button_close.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

using namespace brls::literals;

namespace {

const std::vector<std::string>& qualityLabels() {
    static const std::vector<std::string> labels = {
        "Auto (recommended)", "1080p", "720p", "480p", "360p",
    };
    return labels;
}

const std::vector<std::string>& qualityValues() {
    static const std::vector<std::string> values = {"auto", "1080p", "720p", "480p", "360p"};
    return values;
}

int qualityIndex(const std::string& quality) {
    const auto& values = qualityValues();
    const auto found = std::find(values.begin(), values.end(), quality);
    return found == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), found));
}

const std::vector<std::string>& decoderLabels() {
    static const std::vector<std::string> labels = {"Software", "Hardware", "Hybrid (recommended)"};
    return labels;
}

const std::vector<std::string>& portraitOrientationLabels() {
    static const std::vector<std::string> labels = {
        "Auto (Joy-Con sensor)",
        "Landscape (recommended)",
        "Portrait clockwise",
        "Portrait counter-clockwise",
    };
    return labels;
}

const std::vector<std::string>& keyboardHapticLabels() {
    static const std::vector<std::string> labels = {"Off", "Light", "Medium", "Strong", "Maximum"};
    return labels;
}

const std::vector<std::string>& chatModeLabels() {
    static const std::vector<std::string> labels = {"Off", "Right panel", "Overlay (recommended)"};
    return labels;
}

const std::vector<std::string>& chatParticipationLabels() {
    static const std::vector<std::string> labels = {"Read only", "Interactive"};
    return labels;
}

const std::vector<std::string>& chatComposerLabels() {
    static const std::vector<std::string> labels = {"Nintendo keyboard", "RumbleNX keyboard (recommended)"};
    return labels;
}

const std::vector<std::string>& chatOverlaySizeLabels() {
    static const std::vector<std::string> labels = {"Full height", "Compact"};
    return labels;
}

const std::vector<std::string>& chatOverlayPositionLabels() {
    static const std::vector<std::string> labels = {"Top right", "Top left", "Bottom right", "Bottom left"};
    return labels;
}

const std::vector<std::string>& chatDockedWidthLabels() {
    static const std::vector<std::string> labels = {"Compact", "TV style", "Wide"};
    return labels;
}

const std::vector<int>& chatDockedWidthValues() {
    static const std::vector<int> values = {300, 330, 380};
    return values;
}

const std::vector<std::string>& chatOverlayWidthLabels() {
    static const std::vector<std::string> labels = {"Compact", "Standard", "Wide"};
    return labels;
}

const std::vector<int>& chatOverlayWidthValues() {
    static const std::vector<int> values = {300, 360, 430};
    return values;
}

const std::vector<std::string>& chatFontLabels() {
    static const std::vector<std::string> labels = {"Small", "Medium", "Large"};
    return labels;
}

const std::vector<int>& chatFontValues() {
    static const std::vector<int> values = {13, 16, 19};
    return values;
}

const std::vector<std::string>& chatOpacityLabels() {
    static const std::vector<std::string> labels = {"40% (recommended)", "60%", "80%", "95%"};
    return labels;
}

const std::vector<int>& chatOpacityValues() {
    static const std::vector<int> values = {40, 60, 80, 95};
    return values;
}

int closestIndex(const std::vector<int>& values, int wanted) {
    int best = 0;
    int distance = std::abs(values.front() - wanted);
    for (size_t index = 1; index < values.size(); ++index) {
        const int candidate = std::abs(values[index] - wanted);
        if (candidate < distance) {
            distance = candidate;
            best = static_cast<int>(index);
        }
    }
    return best;
}

std::atomic_uint64_t reloadGeneration{0};

/// Resolves the current video again with the saved preferences and reloads
/// it: live streams at the live edge, other videos where they were.
void reloadCurrentVideo(const PlayerContext& context, const std::string& successNote) {
    if (context.video.id.empty() && context.video.url.empty()) return;
    const uint64_t generation = reloadGeneration.fetch_add(1) + 1;
    const double position = context.live ? 0.0 : MPVCore::instance().getDouble("time-pos");

    // Selectors fire on every step; only the last choice made within a
    // moment triggers a reload.
    brls::delay(650, [context, generation, position, successNote]() {
        if (generation != reloadGeneration.load()) return;
        rumble::resolvePlaybackAsync(
            context.video, rumble::loadPreferredQuality(),
            [context, generation, position, successNote](rumble::Playback playback) {
                if (generation != reloadGeneration.load()) return;
                // The stream went live or ended underneath us; leave it.
                if (playback.isLive() != context.live) return;
                MPVCore::BOTTOM_BAR = false;
                std::string extra = rumble::mpvExtra(context.live);
                if (!context.live && position > 1.0) extra = fmt::format("start={:.1f}", position);
                MPVCore::instance().setUrl(playback.selected.url, extra);
                if (!successNote.empty()) brls::Application::notify(successNote + playback.selected.name);
            },
            [generation](const std::string& error) {
                if (generation != reloadGeneration.load()) return;
                brls::Application::notify("RumbleNX: could not reload the video: " + error);
            });
    });
}

void saveChat(const char* what, const std::function<void(rumble::ChatPreferences&)>& change) {
    auto preferences = rumble::loadChatPreferences();
    change(preferences);
    if (!rumble::saveChatPreferences(preferences))
        brls::Application::notify(std::string("RumbleNX: could not save the ") + what);
}

}  // namespace

PlayerSetting::PlayerSetting(PlayerContext context) {
    this->inflateFromXMLRes("xml/view/player_setting.xml");
    brls::Logger::debug("PlayerSetting: create");
    this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    this->subtitleTrack->detail->setVisibility(brls::Visibility::GONE);

    btnRumbleQuality->init("Video quality", qualityLabels(), qualityIndex(rumble::loadPreferredQuality()),
        [context](int selected) {
            if (selected < 0 || static_cast<size_t>(selected) >= qualityValues().size()) return;
            if (!rumble::savePreferredQuality(qualityValues().at(selected))) {
                brls::Application::notify("RumbleNX: could not save the quality preference");
                return;
            }
            reloadCurrentVideo(context, "RumbleNX: switched to ");
        });

    auto& orientationController = rumblenx::portrait::OrientationController::instance();
    btnPortraitOrientation->init("Display orientation (experimental)", portraitOrientationLabels(),
        static_cast<int>(orientationController.mode()), [](int selected) {
            const int bounded = std::clamp(selected, 0, 3);
            auto& controller = rumblenx::portrait::OrientationController::instance();
            if (!controller.setMode(static_cast<rumblenx::portrait::OrientationMode>(bounded))) {
                brls::Application::notify("RumbleNX: could not save the orientation preference");
                return;
            }
            if (bounded == 0) brls::Application::notify("RumbleNX: automatic Joy-Con orientation enabled");
        });

    btnKeyboardHaptic->init("Touch keyboard vibration intensity", keyboardHapticLabels(),
        std::clamp(static_cast<int>(std::lround(rumblenx::haptics::keyboardIntensity() * 4.0f)), 0, 4),
        [](int selected) {
            const float intensity = static_cast<float>(std::clamp(selected, 0, 4)) / 4.0f;
            if (!rumblenx::haptics::setKeyboardIntensity(intensity))
                brls::Application::notify("RumbleNX: could not save the vibration preference");
            rumblenx::haptics::keyboardPulse();
        });

    btnRumbleDecoder->init("Video decoder", decoderLabels(), static_cast<int>(rumble::loadDecoderMode()),
        [context](int selected) {
            if (selected < static_cast<int>(rumble::DecoderMode::Software) ||
                selected > static_cast<int>(rumble::DecoderMode::Hybrid))
                return;
            const auto mode = static_cast<rumble::DecoderMode>(selected);
            if (!rumble::saveDecoderMode(mode)) {
                brls::Application::notify("RumbleNX: could not save the decoder preference");
                return;
            }

            auto& mpv = MPVCore::instance();
            if (mode == rumble::DecoderMode::Software) {
                mpv.command("set", "hwdec", "no");
            } else {
                mpv.command("set", "vd-lavc-dr", "no");
                mpv.command("set", "hwdec", MPVCore::PLAYER_HWDEC_METHOD.c_str());
            }
            brls::Application::notify(mode == rumble::DecoderMode::Software
                    ? "RumbleNX: switching to software decoding..."
                    : mode == rumble::DecoderMode::Hybrid ? "RumbleNX: switching to Hybrid decoding..."
                                                          : "RumbleNX: switching to hardware decoding...");
            // Reload so the new decoder starts on a clean stream.
            reloadCurrentVideo(context, "");
        });

    if (!context.live) {
        btnRumbleChatMode->setVisibility(brls::Visibility::GONE);
        btnRumbleChatParticipation->setVisibility(brls::Visibility::GONE);
        btnRumbleChatComposer->setVisibility(brls::Visibility::GONE);
        btnRumbleChatOverlaySize->setVisibility(brls::Visibility::GONE);
        btnRumbleChatOverlayPosition->setVisibility(brls::Visibility::GONE);
        btnRumbleChatDockedWidth->setVisibility(brls::Visibility::GONE);
        btnRumbleChatOverlayWidth->setVisibility(brls::Visibility::GONE);
        btnRumbleChatFont->setVisibility(brls::Visibility::GONE);
        btnRumbleChatOpacity->setVisibility(brls::Visibility::GONE);
        btnRumbleChatTimestamps->setVisibility(brls::Visibility::GONE);
    } else {
        const rumble::ChatPreferences chat = rumble::loadChatPreferences();

        btnRumbleChatMode->init("Live chat", chatModeLabels(), static_cast<int>(chat.mode), [](int selected) {
            saveChat("chat layout", [selected](rumble::ChatPreferences& p) {
                p.mode = static_cast<rumble::ChatMode>(std::clamp(selected, 0, 2));
            });
        });

        btnRumbleChatParticipation->init("Chat participation", chatParticipationLabels(),
            static_cast<int>(chat.participation), [](int selected) {
                saveChat("chat participation", [selected](rumble::ChatPreferences& p) {
                    p.participation =
                        selected == 1 ? rumble::ChatParticipation::Interactive : rumble::ChatParticipation::ReadOnly;
                });
                if (selected == 1 && !rumble::canSendChat())
                    brls::Application::notify("Sign in to Rumble on the Home screen to send messages");
            });

        btnRumbleChatComposer->init("Message composer", chatComposerLabels(), static_cast<int>(chat.composerMode),
            [](int selected) {
                saveChat("composer preference", [selected](rumble::ChatPreferences& p) {
                    p.composerMode = selected == 0 ? rumble::ChatComposerMode::NintendoKeyboard
                                                   : rumble::ChatComposerMode::RumbleNXComposer;
                });
            });

        btnRumbleChatOverlaySize->init(
            "Overlay chat size", chatOverlaySizeLabels(), static_cast<int>(chat.overlaySize), [](int selected) {
                saveChat("overlay chat size", [selected](rumble::ChatPreferences& p) {
                    p.overlaySize =
                        selected == 1 ? rumble::ChatOverlaySize::Compact : rumble::ChatOverlaySize::FullHeight;
                });
            });

        btnRumbleChatOverlayPosition->init("Overlay chat position", chatOverlayPositionLabels(),
            static_cast<int>(chat.overlayPosition), [](int selected) {
                saveChat("overlay chat position", [selected](rumble::ChatPreferences& p) {
                    p.overlayPosition = static_cast<rumble::ChatOverlayPosition>(std::clamp(selected, 0, 3));
                });
            });

        btnRumbleChatDockedWidth->init("Right chat panel width", chatDockedWidthLabels(),
            closestIndex(chatDockedWidthValues(), chat.dockedWidth), [](int selected) {
                if (selected < 0 || static_cast<size_t>(selected) >= chatDockedWidthValues().size()) return;
                saveChat("docked chat width",
                    [selected](rumble::ChatPreferences& p) { p.dockedWidth = chatDockedWidthValues().at(selected); });
            });

        btnRumbleChatOverlayWidth->init("Overlay chat width", chatOverlayWidthLabels(),
            closestIndex(chatOverlayWidthValues(), chat.overlayWidth), [](int selected) {
                if (selected < 0 || static_cast<size_t>(selected) >= chatOverlayWidthValues().size()) return;
                saveChat("overlay chat width", [selected](rumble::ChatPreferences& p) {
                    p.overlayWidth = chatOverlayWidthValues().at(selected);
                });
            });

        btnRumbleChatFont->init(
            "Chat text size", chatFontLabels(), closestIndex(chatFontValues(), chat.fontSize), [](int selected) {
                if (selected < 0 || static_cast<size_t>(selected) >= chatFontValues().size()) return;
                saveChat("chat text size",
                    [selected](rumble::ChatPreferences& p) { p.fontSize = chatFontValues().at(selected); });
            });

        btnRumbleChatOpacity->init(
            "Chat background", chatOpacityLabels(), closestIndex(chatOpacityValues(), chat.opacity), [](int selected) {
                if (selected < 0 || static_cast<size_t>(selected) >= chatOpacityValues().size()) return;
                saveChat("chat background",
                    [selected](rumble::ChatPreferences& p) { p.opacity = chatOpacityValues().at(selected); });
            });

        btnRumbleChatTimestamps->init("Chat timestamps (UTC)", chat.timestamps, [](bool enabled) {
            saveChat("timestamp preference", [enabled](rumble::ChatPreferences& p) { p.timestamps = enabled; });
        });
    }

    this->registerAction("hints/cancel"_i18n, brls::BUTTON_B, [](brls::View* view) {
        brls::Application::popActivity();
        return true;
    });

    this->cancel->registerClickAction([](...) {
        brls::Application::popActivity();
        return true;
    });
    this->cancel->addGestureRecognizer(new brls::TapGestureRecognizer(this->cancel));

    auto& mpv = MPVCore::instance();

    std::vector<std::string> audioTrack;
    std::vector<std::string> subTrack = {"main/player/none"_i18n};
    std::vector<int64_t> subTrackIds = {0};
    int currentSubIndex = 0;

    int64_t count = mpv.getInt("track-list/count");
    for (int64_t n = 0; n < count; n++) {
        std::string type = mpv.getString(fmt::format("track-list/{}/type", n));
        std::string title = mpv.getString(fmt::format("track-list/{}/title", n));
        if (title.empty()) title = mpv.getString(fmt::format("track-list/{}/lang", n));
        if (title.empty()) title = fmt::format("{} track {}", type, n);
        if (type == "sub") {
            subTrack.push_back(title);
            int64_t trackId = mpv.getInt(fmt::format("track-list/{}/id", n));
            subTrackIds.push_back(trackId);
            if (mpv.getString(fmt::format("track-list/{}/selected", n)) == "yes") {
                currentSubIndex = static_cast<int>(subTrack.size() - 1);
            }
        } else if (type == "audio") {
            audioTrack.push_back(title);
        }
    }

    // Subtitle selection
    if (subTrack.size() > 1) {
        this->subtitleTrack->setVisibility(brls::Visibility::VISIBLE);
        this->subtitleTrack->init("main/player/subtitle"_i18n, subTrack, currentSubIndex, [&mpv, subTrackIds](int selected) {
            selectedSubtitle = selected;
            if (selected <= 0 || static_cast<size_t>(selected) >= subTrackIds.size() || subTrackIds[selected] <= 0) {
                mpv.command("set", "sid", "no");
            } else {
                mpv.setInt("sid", subTrackIds[selected]);
            }
        });
    } else {
        std::vector<std::string> defaultSub = {"main/player/none"_i18n};
        this->subtitleTrack->setVisibility(brls::Visibility::VISIBLE);
        this->subtitleTrack->init("main/player/subtitle"_i18n, defaultSub, 0, [&mpv](int selected) {
            mpv.command("set", "sid", "no");
        });
    }
    // Audio track selection
    if (audioTrack.size() > 1) {
        int64_t value = mpv.getInt("aid", 1) - 1;
        this->audioTrack->init("main/player/audio"_i18n, audioTrack, value, [&mpv](int selected) {
            selectedAudio = selected + 1;
            mpv.setInt("aid", selectedAudio);
        });
        this->audioTrack->detail->setVisibility(brls::Visibility::GONE);
    } else {
        this->audioTrack->setVisibility(brls::Visibility::GONE);
    }

    auto& conf = AppConfig::instance();

    /// Subsync
    double subDelay = mpv.getDouble("sub-delay");
    btnSubsync->title->setMarginRight(0);
    btnSubsync->slider->setMarginRight(0);
    btnSubsync->slider->setPointerSize(20);
    btnSubsync->setDetailText(fmt::format("{:.1f}", subDelay));
    btnSubsync->init("main/setting/playback/subsync"_i18n, (subDelay + 10) * 0.05f, [this](float value) {
        float data = value * 20 - 10.f;
        MPVCore::instance().setDouble("sub-delay", data);
        btnSubsync->setDetailText(fmt::format("{:.1f}", data));
    });

    btnEqualizerReset->registerClickAction([this](View* view) {
        btnEqualizerBrightness->slider->setProgress(0.5f);
        btnEqualizerContrast->slider->setProgress(0.5f);
        btnEqualizerSaturation->slider->setProgress(0.5f);
        btnEqualizerGamma->slider->setProgress(0.5f);
        btnEqualizerHue->slider->setProgress(0.5f);
        return true;
    });
    registerHideBackground(btnEqualizerReset);
    setupEqualizer(btnEqualizerBrightness, "main/setting/equalizer/brightness"_i18n, Equalizer::BRIGHTNESS,
        mpv.getDouble("brightness"));
    setupEqualizer(
        btnEqualizerContrast, "main/setting/equalizer/contrast"_i18n, Equalizer::CONTRAST, mpv.getDouble("contrast"));
    setupEqualizer(btnEqualizerSaturation, "main/setting/equalizer/saturation"_i18n, Equalizer::SATURATION,
        mpv.getDouble("saturation"));
    setupEqualizer(btnEqualizerGamma, "main/setting/equalizer/gamma"_i18n, Equalizer::GAMMA, mpv.getDouble("gamma"));
    setupEqualizer(btnEqualizerHue, "main/setting/equalizer/hue"_i18n, Equalizer::HUE, mpv.getDouble("hue"));
}

PlayerSetting::~PlayerSetting() { brls::Logger::debug("PlayerSetting: delete"); }

void PlayerSetting::setupEqualizer(brls::SliderCell* cell, const std::string& title, Equalizer item, double initValue) {
    if (initValue < -100)
        initValue = -100;
    else if (initValue > 100)
        initValue = 100;

    cell->detail->setWidth(50);
    cell->title->setWidth(116);
    cell->title->setMarginRight(0);
    cell->slider->setStep(0.05f);
    cell->slider->setMarginRight(0);
    cell->slider->setPointerSize(20);
    cell->setDetailText(fmt::format("{:.0f}", initValue));
    cell->init(title, (initValue + 100) * 0.005f, [cell, item](float value) {
        auto& mpv = MPVCore::instance();
        int data = (int)(value * 200 - 100);
        cell->setDetailText(std::to_string(data));
        switch (item) {
        case Equalizer::BRIGHTNESS:
            mpv.setInt("brightness", data);
            break;
        case Equalizer::CONTRAST:
            mpv.setInt("contrast", data);
            break;
        case Equalizer::SATURATION:
            mpv.setInt("saturation", data);
            break;
        case Equalizer::GAMMA:
            mpv.setInt("gamma", data);
            break;
        case Equalizer::HUE:
            mpv.setInt("hue", data);
            break;
        default:;
        }
    });
    registerHideBackground(cell->getDefaultFocus());
}

void PlayerSetting::registerHideBackground(brls::View* view) {
    view->getFocusEvent()->subscribe([this](...) { this->setBackgroundColor(nvgRGBAf(0.0f, 0.0f, 0.0f, 0.0f)); });
    view->getFocusLostEvent()->subscribe(
        [this](...) { this->setBackgroundColor(brls::Application::getTheme().getColor("brls/backdrop")); });
}
