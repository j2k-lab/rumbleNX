#include "activity/rumble_channel.hpp"

#include "api/rumble_auth.hpp"
#include "api/rumble_follows.hpp"
#include "tab/remote_view.hpp"
#include "utils/image.hpp"
#include "utils/theme.hpp"
#include "view/h_recycling.hpp"
#include "view/rumble_cards.hpp"
#include "view/theme_header.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>

using rumblenx::cards::VideoCell;
using rumblenx::cards::VideoDataSource;

namespace {

ChannelActionButton* makeButton(const std::string& text, brls::Label** output = nullptr) {
    const auto& palette = rumblenx::theme::current();
    auto* button = new ChannelActionButton();
    button->setAxis(brls::Axis::ROW);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(palette.button);
    button->setHideHighlightBackground(palette.keepButtonFillOnFocus);
    button->setMarginLeft(8);
    button->setMarginRight(8);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(18);
    label->setTextColor(palette.buttonText);
    button->addView(label);

    if (output) *output = label;
    return button;
}

HRecyclerFrame* makeVideoRow() {
    auto* row = new HRecyclerFrame();
    row->setHeight(225);
    row->estimatedRowWidth = 315;
    row->estimatedRowSpace = 18;
    row->setPadding(0, 4, 0, 4);
    row->registerCell("Video", []() { return new VideoCell(); });
    return row;
}

brls::Box* makeSection(const std::string& title, ThemeHeader** outHeader) {
    auto* section = new brls::Box();
    section->setAxis(brls::Axis::COLUMN);
    section->setMarginTop(20);
    auto* header = new ThemeHeader();
    header->setTitle(title);
    header->setHeight(38);
    section->addView(header);
    *outHeader = header;
    return section;
}

}  // namespace

RumbleChannelPage::RumbleChannelPage(std::string channelPath)
    : channelPath(std::move(channelPath)), requestCancel(std::make_shared<std::atomic_bool>(false)) {
    const auto& palette = rumblenx::theme::current();
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(palette.background);

    scroll = new brls::ScrollingFrame();
    scroll->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusable(false);
    scroll->setBackgroundColor(palette.background);
    this->addView(scroll);

    content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setWidthPercentage(100);
    content->setBackgroundColor(palette.background);
    scroll->setContentView(content);

    headerBox = new brls::Box();
    headerBox->setAxis(brls::Axis::COLUMN);
    headerBox->setWidthPercentage(100);
    headerBox->setAlignItems(brls::AlignItems::CENTER);
    headerBox->setBackgroundColor(palette.header);
    headerBox->setPadding(14, 42, 24, 42);
    // This header is about as tall as the screen, so it takes the
    // full-screen artwork rather than the short header strip.
    rumblenx::theme::decoratePage(headerBox);
    content->addView(headerBox);

    auto* topBar = new brls::Box();
    topBar->setAxis(brls::Axis::ROW);
    topBar->setWidthPercentage(100);
    topBar->setAlignItems(brls::AlignItems::CENTER);
    topBar->setMarginBottom(12);
    headerBox->addView(topBar);

    backButton = new ChannelActionButton();
    backButton->setAxis(brls::Axis::ROW);
    backButton->setPadding(8, 18, 8, 18);
    backButton->setCornerRadius(8);
    backButton->setBackgroundColor(palette.button);
    backButton->setHideHighlightBackground(palette.keepButtonFillOnFocus);
    backButton->addGestureRecognizer(new brls::TapGestureRecognizer(backButton));
    backButton->registerClickAction([this](brls::View*) {
        markClosing();
        return brls::Application::popActivity();
    });
    backButton->downTarget = [this]() { return getActionFocus(); };
    auto* backLabel = new brls::Label();
    backLabel->setText("<  Back");
    backLabel->setFontSize(16);
    backLabel->setTextColor(palette.buttonText);
    backButton->addView(backLabel);
    topBar->addView(backButton);

    bannerImage = new brls::Image();
    bannerImage->setHeight(215);
    bannerImage->setWidthPercentage(100);
    bannerImage->setCornerRadius(8);
    bannerImage->setScalingType(brls::ImageScalingType::FILL);
    bannerImage->setImageFromRes("img/video-card-bg.png");
    bannerImage->setVisibility(brls::Visibility::GONE);
    headerBox->addView(bannerImage);

    identity = new brls::Box();
    identity->setAxis(brls::Axis::COLUMN);
    identity->setAlignItems(brls::AlignItems::CENTER);
    identity->setJustifyContent(brls::JustifyContent::CENTER);
    identity->setWidthPercentage(100);
    identity->setMarginTop(28);
    headerBox->addView(identity);

    avatar = new rumblenx::cards::CircularAvatar();
    avatar->setWidth(116);
    avatar->setHeight(116);
    avatar->setMarginBottom(12);
    identity->addView(avatar);

    auto addLabel = [](brls::Box* parent, float size, NVGcolor color, float marginTop) {
        auto* label = new brls::Label();
        label->setFontSize(size);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        label->setTextColor(color);
        label->setMarginTop(marginTop);
        parent->addView(label);
        return label;
    };

    name = addLabel(identity, 32, palette.headerTextPrimary, 0);
    name->setText(this->channelPath.substr(this->channelPath.find('/') + 1));
    liveState = addLabel(identity, 17, palette.headerTextMuted, 6);
    liveState->setText("Loading channel…");
    status = addLabel(identity, 15, palette.headerTextMuted, 4);

    headline = addLabel(headerBox, 22, palette.headerTextPrimary, 14);
    headline->setWidthPercentage(90);
    metadata = addLabel(headerBox, 16, palette.headerTextMuted, 8);
    description = addLabel(headerBox, 16, palette.headerTextSecondary, 12);
    description->setWidthPercentage(85);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    actions->setJustifyContent(brls::JustifyContent::CENTER);
    actions->setAlignItems(brls::AlignItems::CENTER);
    actions->setMarginTop(18);
    headerBox->addView(actions);

    playButton = makeButton("Watch live");
    playButton->setVisibility(brls::Visibility::GONE);
    playButton->registerClickAction([this](brls::View*) {
        playLive();
        return true;
    });
    actions->addView(playButton);

    following = rumble::follows::isFollowing(this->channelPath);
    followButton = makeButton("Follow", &followButtonLabel);
    followButton->registerClickAction([this](brls::View*) {
        toggleFollow();
        return true;
    });
    actions->addView(followButton);
    updateFollowButton();

    refreshButton = makeButton("Refresh");
    refreshButton->registerClickAction([this](brls::View*) {
        load();
        return true;
    });
    actions->addView(refreshButton);

    auto resetScrollTop = [this]() {
        if (scroll) scroll->setContentOffsetY(0.0f, false);
    };
    for (auto* button : {playButton, followButton, refreshButton}) {
        button->upTarget = [this]() -> brls::View* { return backButton; };
        button->downTarget = [this]() -> brls::View* { return getFirstRowFocus(); };
        button->getFocusEvent()->subscribe([resetScrollTop](brls::View*) { resetScrollTop(); });
    }

    auto* body = new brls::Box();
    body->setAxis(brls::Axis::COLUMN);
    body->setWidthPercentage(100);
    body->setBackgroundColor(palette.background);
    body->setPadding(8, 42, 36, 42);
    content->addView(body);

    liveSection = makeSection("Live & upcoming", &liveHeader);
    liveRecycler = makeVideoRow();
    liveSection->addView(liveRecycler);
    liveSection->setVisibility(brls::Visibility::GONE);
    body->addView(liveSection);

    videosSection = makeSection("Videos", &videosHeader);
    videosRecycler = makeVideoRow();
    videosSection->addView(videosRecycler);
    videosEmpty = new brls::Label();
    videosEmpty->setFontSize(16);
    videosEmpty->setTextColor(palette.textMuted);
    videosEmpty->setMarginTop(14);
    videosEmpty->setMarginBottom(14);
    videosEmpty->setVisibility(brls::Visibility::GONE);
    videosSection->addView(videosEmpty);
    videosSection->setVisibility(brls::Visibility::GONE);
    body->addView(videosSection);

    this->registerAction("Back", brls::BUTTON_B, [this](brls::View*) {
        markClosing();
        return brls::Application::popActivity();
    });

    load();
}

RumbleChannelPage::~RumbleChannelPage() {
    closing = true;
    if (requestCancel) requestCancel->store(true);
    Image::cancel(bannerImage);
}

void RumbleChannelPage::markClosing() {
    closing = true;
    if (requestCancel) requestCancel->store(true);
}

void RumbleChannelPage::willDisappear(bool resetState) {
    // resetState is true only when the activity is being popped for good.
    if (resetState) markClosing();
    brls::Box::willDisappear(resetState);
}

void RumbleChannelPage::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    // Following may have changed elsewhere (e.g. from Home's import).
    const bool nowFollowing = rumble::follows::isFollowing(channelPath);
    if (nowFollowing != following) {
        following = nowFollowing;
        updateFollowButton();
    }
}

brls::View* RumbleChannelPage::getActionFocus() {
    if (playButton && playButton->getVisibility() == brls::Visibility::VISIBLE) return playButton;
    return followButton;
}

brls::View* RumbleChannelPage::getFirstRowFocus() {
    if (liveSection->getVisibility() == brls::Visibility::VISIBLE) {
        if (auto* focus = rumblenx::cards::recyclerFocus(liveRecycler)) return focus;
    }
    if (videosSection->getVisibility() == brls::Visibility::VISIBLE)
        return rumblenx::cards::recyclerFocus(videosRecycler);
    return nullptr;
}

brls::View* RumbleChannelPage::getDefaultFocus() { return getActionFocus(); }

void RumbleChannelPage::load() {
    if (requestCancel) requestCancel->store(true);
    requestCancel = std::make_shared<std::atomic_bool>(false);
    loadingMore = false;

    status->setText("Loading channel…");
    this->ptrLock();

    // Each request owns its cancel token. A superseded (refresh) or abandoned
    // (page left) request must never write to the UI, so its callbacks only
    // release the pointer lock.
    auto token = requestCancel;
    rumble::loadChannelPageAsync(
        channelPath,
        [this, token](rumble::ChannelPageData loaded) {
            if (!closing && !token->load()) showData(std::move(loaded));
            this->ptrUnlock();
        },
        [this, token](const std::string& error) {
            if (!closing && !token->load()) {
                status->setText("Channel error: " + error);
                liveState->setText("");
                brls::Application::notify("RumbleNX: " + error);
            }
            this->ptrUnlock();
        },
        token);
}

void RumbleChannelPage::showData(rumble::ChannelPageData loaded) {
    // The rows own recyclable cells. Move focus to a permanent control before
    // replacing them so Borealis never retains a detached view.
    brls::Application::giveFocus(refreshButton);

    data = std::move(loaded);
    const auto& palette = rumblenx::theme::current();
    const rumble::Channel& channel = data.channel;

    name->setText(channel.name.empty() ? channelPath : channel.name);
    static_cast<rumblenx::cards::CircularAvatar*>(avatar)->setUrl(channel.avatarUrl);

    if (!channel.bannerUrl.empty()) {
        bannerImage->setVisibility(brls::Visibility::VISIBLE);
        Image::with(bannerImage, channel.bannerUrl);
        identity->setMarginTop(16);
    } else {
        Image::cancel(bannerImage);
        bannerImage->setVisibility(brls::Visibility::GONE);
        identity->setMarginTop(28);
    }

    const auto liveVideo = std::find_if(
        data.live.begin(), data.live.end(), [](const rumble::Video& video) { return video.isLive(); });
    if (liveVideo != data.live.end()) {
        liveState->setText(rumblenx::cards::videoStatus(*liveVideo));
        liveState->setTextColor(palette.headerAccent);
        headline->setText(liveVideo->title);
        playButton->setVisibility(brls::Visibility::VISIBLE);
    } else {
        liveState->setText(data.live.empty() ? "Not live right now" : "Stream scheduled");
        liveState->setTextColor(palette.headerTextMuted);
        headline->setText(data.live.empty() ? std::string() : data.live.front().title);
        playButton->setVisibility(brls::Visibility::GONE);
    }

    std::string meta = rumblenx::cards::followersText(channel);
    if (channel.verified) meta += meta.empty() ? "Verified channel" : "  ·  Verified";
    metadata->setText(meta);
    description->setText(channel.description.empty() ? "This channel has no description." : channel.description);
    status->setText("");

    following = data.following;
    updateFollowButton();

    auto playVideo = [](const rumble::Video& video) { RemoteView::playVideo(video); };
    auto actionUp = [this]() -> brls::View* {
        if (scroll) scroll->setContentOffsetY(0.0f, false);
        return getActionFocus();
    };
    auto configureVideo = [](VideoCell* cell, const rumble::Video& video) { cell->configure(video, false); };

    const bool hasLive = !data.live.empty();
    liveSection->setVisibility(hasLive ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    liveRecycler->setDataSource(new VideoDataSource("Video", data.live, configureVideo,
        rumblenx::cards::videoKey, playVideo, nullptr, actionUp,
        [this]() -> brls::View* { return rumblenx::cards::recyclerFocus(videosRecycler); }));

    videosCursor = data.videos.cursor;
    const bool hasVideos = !data.videos.items.empty();
    videosSection->setVisibility(brls::Visibility::VISIBLE);
    videosRecycler->setVisibility(hasVideos ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    videosEmpty->setText("This channel has no videos yet.");
    videosEmpty->setVisibility(hasVideos ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
    videosHeader->setSubtitle(std::to_string(data.videos.items.size()) + (videosCursor.empty() ? "" : "+"));
    videosRecycler->setDataSource(new VideoDataSource("Video", data.videos.items, configureVideo,
        rumblenx::cards::videoKey, playVideo, nullptr,
        [this, actionUp]() -> brls::View* {
            if (liveSection->getVisibility() == brls::Visibility::VISIBLE) {
                if (auto* focus = rumblenx::cards::recyclerFocus(liveRecycler)) return focus;
            }
            return actionUp();
        },
        nullptr));
    videosRecycler->onNextPage(videosCursor.empty() ? std::function<void()>{} : [this]() { loadMoreVideos(); });

    content->invalidate();
    scroll->setContentOffsetY(0, false);
    brls::Application::giveFocus(getActionFocus());
}

void RumbleChannelPage::loadMoreVideos() {
    if (loadingMore || videosCursor.empty()) return;
    loadingMore = true;
    auto token = requestCancel;
    this->ptrLock();
    rumble::loadChannelVideosPageAsync(
        channelPath, videosCursor,
        [this, token](rumble::VideoPage page) {
            if (!closing && !token->load()) {
                loadingMore = false;
                auto* source = dynamic_cast<VideoDataSource*>(videosRecycler->getDataSource());
                if (source && source->append(std::move(page.items)) > 0) videosRecycler->notifyDataChanged();
                videosCursor = page.cursor;
                if (source)
                    videosHeader->setSubtitle(
                        std::to_string(source->getItemCount()) + (videosCursor.empty() ? "" : "+"));
                videosRecycler->onNextPage(
                    videosCursor.empty() ? std::function<void()>{} : [this]() { loadMoreVideos(); });
            }
            this->ptrUnlock();
        },
        [this, token](const std::string& error) {
            if (!closing && !token->load()) {
                loadingMore = false;
                brls::Application::notify("RumbleNX: could not load more videos: " + error);
            }
            this->ptrUnlock();
        },
        token);
}

void RumbleChannelPage::playLive() {
    const auto liveVideo = std::find_if(
        data.live.begin(), data.live.end(), [](const rumble::Video& video) { return video.isLive(); });
    if (liveVideo == data.live.end()) {
        brls::Application::notify("This channel is not live right now");
        return;
    }
    RemoteView::playVideo(*liveVideo);
}

void RumbleChannelPage::updateFollowButton() {
    if (!followButton || !followButtonLabel) return;
    const auto& palette = rumblenx::theme::current();
    followButtonLabel->setText(following ? "Following" : "Follow");
    followButton->setBackgroundColor(following ? palette.buttonMuted : palette.button);
}

void RumbleChannelPage::toggleFollow() {
    rumble::Channel channel = data.channel;
    if (channel.path.empty()) channel.path = channelPath;
    if (channel.name.empty()) channel.name = channelPath.substr(channelPath.find('/') + 1);

    const bool saved = following ? rumble::follows::unfollow(channel.path) : rumble::follows::follow(channel);
    if (!saved) {
        brls::Application::notify("RumbleNX: could not save your follows to the SD card");
        return;
    }
    following = !following;
    updateFollowButton();
    brls::Application::notify(following ? "Following " + channel.name : "Unfollowed " + channel.name);
}
