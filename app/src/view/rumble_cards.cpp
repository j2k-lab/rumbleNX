#include "view/rumble_cards.hpp"

#include "utils/image.hpp"
#include "utils/theme.hpp"
#include "view/h_recycling.hpp"

#include <fmt/format.h>

#include <algorithm>

namespace rumblenx::cards {

std::string compactNumber(int64_t value) {
    if (value >= 1000000000) return fmt::format("{:.1f}B", value / 1000000000.0);
    if (value >= 1000000) return fmt::format("{:.1f}M", value / 1000000.0);
    if (value >= 1000) return fmt::format("{:.1f}K", value / 1000.0);
    return std::to_string(std::max<int64_t>(0, value));
}

std::string durationText(int64_t seconds) {
    if (seconds <= 0) return {};
    const int64_t hours = seconds / 3600;
    const int64_t minutes = (seconds % 3600) / 60;
    const int64_t rest = seconds % 60;
    if (hours > 0) return fmt::format("{}:{:02}:{:02}", hours, minutes, rest);
    return fmt::format("{}:{:02}", minutes, rest);
}

std::string videoStatus(const rumble::Video& video) {
    if (video.isLive())
        return video.watching > 0 ? "LIVE · " + compactNumber(video.watching) + " watching" : std::string("LIVE");
    if (video.isUpcoming()) return "Scheduled stream";
    return video.views > 0 ? compactNumber(video.views) + (video.views == 1 ? " view" : " views") : std::string();
}

std::string videoSubtitle(const rumble::Video& video, bool withChannel) {
    std::string text = withChannel ? video.channelName : std::string();
    const std::string status = videoStatus(video);
    if (!status.empty()) {
        if (!text.empty()) text += " · ";
        text += status;
    }
    return text;
}

std::string followersText(const rumble::Channel& channel) {
    if (channel.followers <= 0) return channel.followersText;
    return compactNumber(channel.followers) + (channel.followers == 1 ? " follower" : " followers");
}

std::string videoKey(const rumble::Video& video) { return video.id.empty() ? video.url : video.id; }
std::string channelKey(const rumble::Channel& channel) { return channel.path; }
std::string categoryKey(const rumble::Category& category) { return category.slug; }

// ------------------------------------------------------------- avatar

CircularAvatar::CircularAvatar() {
    loader = new brls::Image();
    loader->setFreeTexture(false);
}

CircularAvatar::~CircularAvatar() {
    Image::cancel(loader);
    delete loader;
}

void CircularAvatar::setUrl(const std::string& url) {
    if (url == currentUrl) return;
    Image::cancel(loader);
    currentUrl = url;
    if (!currentUrl.empty()) Image::with(loader, currentUrl);
    this->invalidate();
}

void CircularAvatar::draw(
    NVGcontext* vg, float x, float y, float width, float height, brls::Style, brls::FrameContext*) {
    const auto& palette = rumblenx::theme::current();
    const float size = std::min(width, height);
    const float cx = x + width * 0.5f;
    const float cy = y + height * 0.5f;
    const float radius = size * 0.5f - 2.0f;

    nvgSave(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, radius);
    nvgFillColor(vg, palette.button);
    nvgFill(vg);

    if (loader->getTexture() > 0) {
        const NVGpaint paint = nvgImagePattern(
            vg, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 0, loader->getTexture(), 1.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, radius);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }

    const bool focused = focusRing && isFocused();
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, radius);
    nvgStrokeColor(vg, focused ? palette.accent : palette.outline);
    nvgStrokeWidth(vg, focused ? 4.0f : 2.0f);
    nvgStroke(vg);
    nvgRestore(vg);
}

// ------------------------------------------------------------- cells

CardCell::CardCell() {
    this->setAxis(brls::Axis::COLUMN);
    this->setFocusable(true);
    this->setCornerRadius(8);
    this->setBackgroundColor(rumblenx::theme::current().surface);
    this->setPadding(0, 0, 10, 0);
    this->getFocusEvent()->subscribe([this](brls::View*) {
        if (onFocused) onFocused();
    });
}

brls::View* CardCell::getNextFocus(brls::FocusDirection direction, brls::View* currentView) {
    if (direction == brls::FocusDirection::UP && upTarget) {
        if (auto* target = upTarget()) return target;
    }
    if (direction == brls::FocusDirection::DOWN && downTarget) {
        if (auto* target = downTarget()) return target;
    }
    return RecyclingGridItem::getNextFocus(direction, currentView);
}

namespace {

/// Small pill drawn over a thumbnail corner.
brls::Box* makeBadge(bool top, brls::Label** label) {
    auto* box = new brls::Box();
    box->setPositionType(brls::PositionType::ABSOLUTE);
    if (top) {
        box->setPositionTop(8);
        box->setPositionLeft(8);
    } else {
        box->setPositionBottom(8);
        box->setPositionRight(8);
    }
    box->setPadding(2, 6, 2, 6);
    box->setCornerRadius(4);
    box->setBackgroundColor(nvgRGBA(0, 0, 0, 190));
    box->setVisibility(brls::Visibility::GONE);

    *label = new brls::Label();
    (*label)->setFontSize(13);
    (*label)->setSingleLine(true);
    (*label)->setTextColor(nvgRGB(255, 255, 255));
    box->addView(*label);
    return box;
}

brls::Label* makeText(float size, NVGcolor color, float marginTop) {
    auto* label = new brls::Label();
    label->setFontSize(size);
    label->setSingleLine(true);
    label->setMarginTop(marginTop);
    label->setMarginLeft(10);
    label->setMarginRight(10);
    label->setTextColor(color);
    return label;
}

}  // namespace

VideoCell::VideoCell() {
    const auto& palette = rumblenx::theme::current();
    auto* thumb = new brls::Box();
    thumb->setHeight(142);
    thumb->setWidthPercentage(100);
    this->addView(thumb);

    picture = new brls::Image();
    picture->setHeightPercentage(100);
    picture->setWidthPercentage(100);
    picture->setScalingType(brls::ImageScalingType::FILL);
    picture->setImageFromRes("img/video-card-bg.png");
    thumb->addView(picture);

    badge = makeBadge(true, &badgeLabel);
    thumb->addView(badge);
    duration = makeBadge(false, &durationLabel);
    thumb->addView(duration);

    title = makeText(17, palette.textPrimary, 8);
    this->addView(title);
    subtitle = makeText(15, palette.textMuted, 3);
    this->addView(subtitle);
}

VideoCell::~VideoCell() { Image::cancel(picture); }

void VideoCell::prepareForReuse() {
    Image::cancel(picture);
    picture->setImageFromRes("img/video-card-bg.png");
    badge->setVisibility(brls::Visibility::GONE);
    duration->setVisibility(brls::Visibility::GONE);
    title->setText("");
    subtitle->setText("");
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void VideoCell::cacheForReuse() {
    Image::cancel(picture);
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void VideoCell::configure(const rumble::Video& video, bool withChannel) {
    title->setText(video.title);
    subtitle->setText(videoSubtitle(video, withChannel));
    if (!video.thumbnailUrl.empty()) Image::with(picture, video.thumbnailUrl);

    if (video.isLive() || video.isUpcoming()) {
        badgeLabel->setText(video.isLive() ? "LIVE" : "UPCOMING");
        badge->setBackgroundColor(video.isLive() ? nvgRGB(242, 49, 96) : nvgRGBA(0, 0, 0, 190));
        badge->setVisibility(brls::Visibility::VISIBLE);
    }
    const std::string length = durationText(video.durationSeconds);
    if (!video.isLive() && !length.empty()) {
        durationLabel->setText(length);
        duration->setVisibility(brls::Visibility::VISIBLE);
    }
}

ChannelCell::ChannelCell() {
    const auto& palette = rumblenx::theme::current();
    this->setAlignItems(brls::AlignItems::CENTER);
    this->setPadding(16, 10, 12, 10);

    avatar = new CircularAvatar();
    avatar->setWidth(118);
    avatar->setHeight(118);
    this->addView(avatar);

    name = makeText(17, palette.textPrimary, 12);
    name->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(name);
    detail = makeText(15, palette.textMuted, 3);
    detail->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    this->addView(detail);
}

void ChannelCell::prepareForReuse() {
    avatar->setUrl("");
    name->setText("");
    detail->setText("");
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void ChannelCell::cacheForReuse() {
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void ChannelCell::configure(const rumble::Channel& channel) {
    avatar->setUrl(channel.avatarUrl);
    name->setText(channel.name);
    const std::string followers = followersText(channel);
    detail->setText(followers.empty() ? "Channel" : followers);
}

CategoryCell::CategoryCell() {
    picture = new brls::Image();
    picture->setHeight(205);
    picture->setWidthPercentage(100);
    picture->setScalingType(brls::ImageScalingType::FILL);
    picture->setImageFromRes("img/category-card-bg.png");
    this->addView(picture);

    title = makeText(17, rumblenx::theme::current().textPrimary, 8);
    title->setMarginLeft(8);
    title->setMarginRight(8);
    this->addView(title);
}

CategoryCell::~CategoryCell() { Image::cancel(picture); }

void CategoryCell::prepareForReuse() {
    Image::cancel(picture);
    picture->setImageFromRes("img/category-card-bg.png");
    title->setText("");
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void CategoryCell::cacheForReuse() {
    Image::cancel(picture);
    onFocused = nullptr;
    upTarget = nullptr;
    downTarget = nullptr;
}

void CategoryCell::configure(const rumble::Category& category) {
    title->setText(category.name);
    if (!category.imageUrl.empty()) Image::with(picture, category.imageUrl);
}

brls::View* recyclerFocus(brls::View* view) {
    auto* recycler = dynamic_cast<HRecyclerFrame*>(view);
    if (!recycler || recycler->getVisibility() != brls::Visibility::VISIBLE) return nullptr;
    if (brls::View* focus = recycler->getDefaultFocus()) return focus;
    for (auto* child : recycler->getChildren()) {
        if (!child) continue;
        if (child->isFocusable() && child->getVisibility() == brls::Visibility::VISIBLE) return child;
        if (auto* box = dynamic_cast<brls::Box*>(child)) {
            for (auto* grandchild : box->getChildren()) {
                if (grandchild && grandchild->isFocusable() &&
                    grandchild->getVisibility() == brls::Visibility::VISIBLE)
                    return grandchild;
            }
        }
    }
    return nullptr;
}

}  // namespace rumblenx::cards
