#include "utils/theme.hpp"
#include "utils/config.hpp"

#include <algorithm>

namespace rumblenx::theme {

namespace {

// OLED: pure black pages, near-black bars and Rumble's green accent.
Palette makeOled() {
    Palette p;
    p.preset = Preset::Oled;
    p.id = "oled";
    p.name = "OLED (default)";
    p.background = nvgRGB(0, 0, 0);
    p.header = nvgRGB(14, 18, 24);
    p.surface = nvgRGB(30, 35, 44);
    p.surfaceAlt = nvgRGB(23, 28, 36);
    p.heroOverlay = nvgRGBA(16, 20, 27, 245);
    p.textPrimary = nvgRGB(245, 246, 250);
    p.textSecondary = nvgRGB(190, 199, 212);
    p.textMuted = nvgRGB(170, 179, 193);
    p.textFaint = nvgRGB(150, 160, 176);
    p.accent = nvgRGB(133, 199, 66);
    p.button = nvgRGB(72, 110, 33);
    p.buttonText = nvgRGB(250, 250, 252);
    p.buttonMuted = nvgRGB(50, 58, 70);
    p.errorBackground = nvgRGB(75, 35, 42);
    p.errorText = nvgRGB(245, 220, 224);
    p.live = nvgRGB(242, 49, 96);
    p.outline = nvgRGBA(255, 255, 255, 90);
    p.divider = nvgRGBA(255, 255, 255, 18);
    p.focusHighlight1 = nvgRGB(133, 199, 66);
    p.focusHighlight2 = nvgRGB(174, 218, 129);
    p.focusBackground = nvgRGB(31, 36, 40);
    p.headerAccent = p.accent;
    p.headerTextPrimary = p.textPrimary;
    p.headerTextSecondary = p.textSecondary;
    p.headerTextMuted = p.textMuted;
    return p;
}

// Lifted charcoal for LCD screens, where pure black looks muddy.
Palette makeDarkGrey() {
    Palette p = makeOled();
    p.preset = Preset::DarkGrey;
    p.id = "dark_grey";
    p.name = "Dark Grey";
    p.background = nvgRGB(30, 32, 38);
    p.header = nvgRGB(40, 43, 51);
    p.surface = nvgRGB(54, 58, 69);
    p.surfaceAlt = nvgRGB(46, 49, 59);
    p.heroOverlay = nvgRGBA(40, 43, 51, 245);
    p.buttonMuted = nvgRGB(72, 78, 94);
    p.errorBackground = nvgRGB(92, 44, 52);
    p.focusBackground = nvgRGB(66, 71, 84);
    return p;
}

// Rumble's dark navy (#061726) pages and blue-grey cards with its green
// accent, under a faint play-mark pattern on headers and the sign-in page.
Palette makeRumble() {
    Palette p = makeOled();
    p.preset = Preset::Rumble;
    p.id = "rumble";
    p.name = "Rumble";
    p.background = nvgRGB(6, 23, 38);
    p.header = nvgRGB(11, 33, 53);
    p.surface = nvgRGB(22, 44, 64);
    p.surfaceAlt = nvgRGB(15, 35, 54);
    p.heroOverlay = nvgRGBA(9, 28, 46, 240);
    p.textPrimary = nvgRGB(255, 255, 255);
    p.textSecondary = nvgRGB(214, 224, 234);
    p.textMuted = nvgRGB(160, 180, 200);
    p.textFaint = nvgRGB(130, 152, 174);
    p.buttonMuted = nvgRGB(73, 90, 106);
    p.errorBackground = nvgRGB(92, 30, 44);
    p.errorText = nvgRGB(255, 214, 222);
    p.focusBackground = nvgRGB(30, 56, 80);
    p.headerImage = "img/rumble_theme_header.png";
    p.headerAccent = p.accent;
    p.headerTextPrimary = p.textPrimary;
    p.headerTextSecondary = p.textSecondary;
    p.headerTextMuted = p.textMuted;
    p.pageImage = "img/rumble_theme_background.png";
    return p;
}

size_t activeIndex = 0;

void applyBorealisColors(const Palette& p) {
    for (auto* theme : {&brls::Theme::getLightTheme(), &brls::Theme::getDarkTheme()}) {
        theme->addColor("brls/highlight/color1", p.focusHighlight1);
        theme->addColor("brls/highlight/color2", p.focusHighlight2);
        theme->addColor("brls/accent", p.focusHighlight1);
        theme->addColor("brls/highlight/background", p.focusBackground);
        theme->addColor("brls/click_pulse",
            nvgRGBA(static_cast<unsigned char>(p.focusHighlight1.r * 255),
                static_cast<unsigned char>(p.focusHighlight1.g * 255),
                static_cast<unsigned char>(p.focusHighlight1.b * 255), 45));
    }
}

void activate(size_t index) {
    activeIndex = index;
    applyBorealisColors(presets()[index]);
}

}  // namespace

const std::vector<Palette>& presets() {
    static const std::vector<Palette> all = {makeOled(), makeDarkGrey(), makeRumble()};
    return all;
}

const Palette& current() { return presets()[activeIndex]; }

void load() {
    const std::string saved = AppConfig::instance().getItem(AppConfig::APP_COLOR_THEME, std::string("oled"));
    const auto& all = presets();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Palette& p) { return p.id == saved; });
    activate(it == all.end() ? 0 : static_cast<size_t>(std::distance(all.begin(), it)));
}

void set(Preset preset) {
    const auto& all = presets();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Palette& p) { return p.preset == preset; });
    if (it == all.end()) return;
    activate(static_cast<size_t>(std::distance(all.begin(), it)));
    AppConfig::instance().setItem(AppConfig::APP_COLOR_THEME, it->id);
}

namespace {

// Paints artwork over the whole of its parent's frame, scaled to cover it and
// centred, with the parent's rounded corners. A plain brls::Image laid out as
// an absolute child of a padded header was sized and placed from its own
// frame and ended up in the header's top-right corner, so this ignores its
// own layout and draws from the parent's frame instead.
class Backdrop : public brls::View {
public:
    explicit Backdrop(const std::string& image) {
        loader = new brls::Image();
        loader->setImageFromRes(image);
        this->setPositionType(brls::PositionType::ABSOLUTE);
        this->setPositionTop(0);
        this->setPositionLeft(0);
        this->setWidthPercentage(100);
        this->setHeightPercentage(100);
        this->setFocusable(false);
    }

    ~Backdrop() override { delete loader; }

    void draw(NVGcontext* vg, float, float, float, float, brls::Style, brls::FrameContext*) override {
        brls::View* parent = this->getParent();
        const int texture = loader->getTexture();
        const float imageWidth = loader->getOriginalImageWidth();
        const float imageHeight = loader->getOriginalImageHeight();
        if (!parent || texture <= 0 || imageWidth <= 0 || imageHeight <= 0) return;

        const brls::Rect frame = parent->getFrame();
        const float scale = std::max(frame.getWidth() / imageWidth, frame.getHeight() / imageHeight);
        const float drawWidth = imageWidth * scale;
        const float drawHeight = imageHeight * scale;
        const float drawX = frame.getMinX() + (frame.getWidth() - drawWidth) / 2;
        const float drawY = frame.getMinY() + (frame.getHeight() - drawHeight) / 2;

        const NVGpaint paint = nvgImagePattern(vg, drawX, drawY, drawWidth, drawHeight, 0, texture, this->getAlpha());
        nvgBeginPath(vg);
        nvgRoundedRect(vg, frame.getMinX(), frame.getMinY(), frame.getWidth(), frame.getHeight(),
            parent->getCornerRadius());
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

private:
    brls::Image* loader = nullptr;
};

void addBackdrop(brls::Box* box, const std::string& image) {
    if (!box || image.empty()) return;
    box->addView(new Backdrop(image), 0);
}

}  // namespace

void decorateHeader(brls::Box* header, const Palette& palette) {
    addBackdrop(header, palette.headerImage);
}

void decoratePage(brls::Box* page, const Palette& palette) {
    addBackdrop(page, palette.pageImage);
}

}  // namespace rumblenx::theme
