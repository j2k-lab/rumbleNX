#include "activity/rumblenx_home.hpp"

#include "activity/rumble_channel.hpp"
#include "api/rumble_auth.hpp"
#include "api/rumble_browse.hpp"
#include "api/rumble_follows.hpp"
#include "tab/remote_view.hpp"
#include "utils/config.hpp"
#include "utils/image.hpp"
#include "utils/theme.hpp"
#include "utils/thread.hpp"
#include "utils/updater.hpp"
#include "view/h_recycling.hpp"
#include "view/rumble_cards.hpp"
#include "view/theme_header.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using rumblenx::cards::CategoryCell;
using rumblenx::cards::CategoryDataSource;
using rumblenx::cards::ChannelCell;
using rumblenx::cards::ChannelDataSource;
using rumblenx::cards::VideoCell;
using rumblenx::cards::VideoDataSource;

namespace {

/// Logo artwork size (resources/img/rumblenx_logo.png).
constexpr float LOGO_ASPECT = 840.0f / 200.0f;

class HeaderScrollingFrame : public brls::ScrollingFrame {
public:
    /// Focus anywhere inside this view (a page header and its buttons)
    /// counts as focus in the scroll frame.
    void setFocusProxy(brls::View* view) { focusProxy = view; }

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override {
        // ScrollingFrame normally reads the right stick only while it or one
        // of its descendants owns focus. Home and About keep their buttons in
        // a fixed header outside the scroll frame, so the right stick stopped
        // scrolling once one of them was focused. Bridge that one local focus
        // state during drawing so right-stick scrolling keeps working.
        bool proxyIsFocused = false;
        for (brls::View* view = brls::Application::getCurrentFocus(); focusProxy && view;
             view = view->getParent()) {
            if (view == focusProxy) {
                proxyIsFocused = true;
                break;
            }
        }
        const bool previousChildFocused = childFocused;
        const brls::ScrollingBehavior previousBehavior = behavior;
        if (proxyIsFocused) {
            childFocused = true;
            // NATURAL scrolling pulls focus that sits outside the frame back
            // into it, which would trap focus below the header. Bridge only
            // the right-stick scrolling.
            behavior = brls::ScrollingBehavior::CENTERED;
        }

        brls::ScrollingFrame::draw(vg, x, y, width, height, style, ctx);

        childFocused = previousChildFocused;
        behavior = previousBehavior;
    }

private:
    brls::View* focusProxy = nullptr;
};

class ActionButton : public brls::Box {
public:
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (auto* target = downTarget()) return target;
        }
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (auto* target = upTarget()) return target;
        }
        return brls::Box::getNextFocus(direction, currentView);
    }

    std::function<brls::View*()> downTarget;
    std::function<brls::View*()> upTarget;
};

class ProfileAvatar : public rumblenx::cards::CircularAvatar {
public:
    ProfileAvatar() {
        focusRing = true;
        this->setFocusable(true);
        this->setCornerRadius(38);
        this->setHighlightCornerRadius(38);
        this->setHideHighlightBackground(true);
        this->setHideHighlightBorder(true);
        this->setVisibility(brls::Visibility::GONE);
        this->addGestureRecognizer(new brls::TapGestureRecognizer(this));
    }

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (auto* target = downTarget()) return target;
        }
        return brls::View::getNextFocus(direction, currentView);
    }

    std::function<brls::View*()> downTarget;
};

ActionButton* makeButton(const std::string& text, brls::Label** outLabel = nullptr) {
    const auto& palette = rumblenx::theme::current();
    auto* button = new ActionButton();
    button->setFocusable(true);
    button->setAxis(brls::Axis::ROW);
    button->setPadding(12, 22, 12, 22);
    button->setCornerRadius(8);
    button->setBackgroundColor(palette.button);
    button->setHideHighlightBackground(palette.keepButtonFillOnFocus);
    button->setMarginRight(12);
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));

    auto* label = new brls::Label();
    label->setText(text);
    label->setFontSize(19);
    label->setTextColor(palette.buttonText);
    button->addView(label);
    if (outLabel) *outLabel = label;
    return button;
}

std::string trimmed(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string joinErrors(const rumble::HomeData& data) {
    std::vector<std::string> errors;
    if (!data.liveNowError.empty()) errors.push_back("live streams");
    if (!data.editorPicksError.empty()) errors.push_back("editor picks");
    if (!data.trendingError.empty()) errors.push_back("trending videos");
    if (!data.categoriesError.empty()) errors.push_back("categories");

    std::string result;
    for (size_t index = 0; index < errors.size(); ++index) {
        if (index) result += index + 1 == errors.size() ? " and " : ", ";
        result += errors[index];
    }
    return result;
}

void showMessage(const std::string& message) {
    auto* dialog = new brls::Dialog(message);
    dialog->addButton("OK", []() {});
    dialog->open();
}

/// Set by the quiet startup check; offered once Home is on top.
std::optional<rumblenx::updater::Release> pendingUpdate;

void offerUpdate(const rumblenx::updater::Release& release);

}  // namespace

/// One titled, horizontally scrolling row of video, channel or category cards.
class ContentRow : public brls::Box {
public:
    enum class Kind { Videos, Channels, Categories };
    using FocusCallback = std::function<void(const std::string&, const std::string&)>;
    using VideoFocusCallback = std::function<void(const rumble::Video&)>;

    ContentRow(std::string identifier, const std::string& text, Kind rowKind, FocusCallback focus)
        : id(std::move(identifier)), kind(rowKind), focusCallback(std::move(focus)) {
        const auto& palette = rumblenx::theme::current();
        this->setAxis(brls::Axis::COLUMN);
        this->setMarginBottom(18);

        header = new ThemeHeader();
        header->setTitle(text);
        header->setHeight(38);
        this->addView(header);

        recycler = new HRecyclerFrame();
        recycler->setHeight(kind == Kind::Categories ? 285 : 225);
        recycler->estimatedRowWidth = kind == Kind::Categories ? 175 : (kind == Kind::Channels ? 230 : 315);
        recycler->estimatedRowSpace = 18;
        recycler->setPadding(0, 4, 0, 4);
        if (kind == Kind::Videos) recycler->registerCell("Video", []() { return new VideoCell(); });
        else if (kind == Kind::Channels) recycler->registerCell("Channel", []() { return new ChannelCell(); });
        else recycler->registerCell("Category", []() { return new CategoryCell(); });
        this->addView(recycler);

        emptyBox = new brls::Box();
        emptyBox->setAxis(brls::Axis::ROW);
        emptyBox->setHeight(70);
        emptyBox->setPadding(14, 18, 14, 18);
        emptyBox->setCornerRadius(8);
        emptyBox->setBackgroundColor(palette.surfaceAlt);
        emptyLabel = new brls::Label();
        emptyLabel->setFontSize(17);
        emptyLabel->setTextColor(palette.textMuted);
        emptyBox->addView(emptyLabel);
        emptyBox->setVisibility(brls::Visibility::GONE);
        this->addView(emptyBox);

        this->setVisibility(brls::Visibility::GONE);
    }

    void setTitle(const std::string& text) { header->setTitle(text); }
    void onVideoFocused(VideoFocusCallback callback) { videoFocusCallback = std::move(callback); }

    void setVideos(std::vector<rumble::Video> items, std::function<void(const rumble::Video&)> selected,
        const std::string& emptyText, std::function<brls::View*()> upTarget = {}, bool withChannel = true) {
        count = items.size();
        recycler->setDataSource(new VideoDataSource("Video", std::move(items),
            [withChannel](VideoCell* cell, const rumble::Video& video) { cell->configure(video, withChannel); },
            rumblenx::cards::videoKey, std::move(selected),
            [this](const rumble::Video& video) {
                if (focusCallback) focusCallback(id, rumblenx::cards::videoKey(video));
                if (videoFocusCallback) videoFocusCallback(video);
            },
            std::move(upTarget), nullptr));
        showResult(emptyText);
    }

    void setChannels(std::vector<rumble::Channel> items, std::function<void(const rumble::Channel&)> selected,
        const std::string& emptyText, std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        recycler->setDataSource(new ChannelDataSource("Channel", std::move(items),
            [](ChannelCell* cell, const rumble::Channel& channel) { cell->configure(channel); },
            rumblenx::cards::channelKey, std::move(selected),
            [this](const rumble::Channel& channel) {
                if (focusCallback) focusCallback(id, rumblenx::cards::channelKey(channel));
            },
            std::move(upTarget), nullptr));
        showResult(emptyText);
    }

    void setCategories(std::vector<rumble::Category> items, std::function<void(const rumble::Category&)> selected,
        const std::string& emptyText, std::function<brls::View*()> upTarget = {}) {
        count = items.size();
        recycler->setDataSource(new CategoryDataSource("Category", std::move(items),
            [](CategoryCell* cell, const rumble::Category& category) { cell->configure(category); },
            rumblenx::cards::categoryKey, std::move(selected),
            [this](const rumble::Category& category) {
                if (focusCallback) focusCallback(id, rumblenx::cards::categoryKey(category));
            },
            std::move(upTarget), nullptr));
        showResult(emptyText);
    }

    template <typename Source, typename Item>
    size_t append(std::vector<Item> items) {
        auto* source = dynamic_cast<Source*>(recycler->getDataSource());
        if (!source) return 0;
        const size_t added = source->append(std::move(items));
        count += added;
        if (added) recycler->notifyDataChanged();
        updateSubtitle();
        return added;
    }

    size_t appendVideos(std::vector<rumble::Video> items) { return append<VideoDataSource>(std::move(items)); }
    size_t appendChannels(std::vector<rumble::Channel> items) { return append<ChannelDataSource>(std::move(items)); }
    size_t appendCategories(std::vector<rumble::Category> items) {
        return append<CategoryDataSource>(std::move(items));
    }

    void setHasMore(bool value) {
        hasMore = value;
        updateSubtitle();
    }

    void onNextPage(bool enabled, const std::function<void()>& callback) {
        recycler->onNextPage(enabled ? callback : std::function<void()>{});
    }

    void clear() {
        this->setVisibility(brls::Visibility::GONE);
        recycler->onNextPage(std::function<void()>{});
        recycler->clearData();
        count = 0;
        hasMore = false;
    }

    brls::View* defaultFocus() {
        if (this->getVisibility() != brls::Visibility::VISIBLE || count == 0) return nullptr;
        return rumblenx::cards::recyclerFocus(recycler);
    }

    bool focusKey(const std::string& key) {
        size_t index = count;
        if (auto* videos = dynamic_cast<VideoDataSource*>(recycler->getDataSource())) index = videos->find(key);
        else if (auto* channels = dynamic_cast<ChannelDataSource*>(recycler->getDataSource()))
            index = channels->find(key);
        else if (auto* categories = dynamic_cast<CategoryDataSource*>(recycler->getDataSource()))
            index = categories->find(key);
        if (index >= count || this->getVisibility() != brls::Visibility::VISIBLE) return false;
        recycler->selectRowAt(index, false);
        if (auto* focus = recycler->getDefaultFocus()) {
            brls::Application::giveFocus(focus);
            return true;
        }
        return false;
    }

private:
    void showResult(const std::string& emptyText) {
        const bool hasItems = count > 0;
        this->setVisibility(brls::Visibility::VISIBLE);
        recycler->setVisibility(hasItems ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
        emptyLabel->setText(emptyText);
        emptyBox->setVisibility(hasItems ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
        updateSubtitle();
    }

    void updateSubtitle() { header->setSubtitle(std::to_string(count) + (hasMore ? "+" : "")); }

    std::string id;
    Kind kind;
    FocusCallback focusCallback;
    VideoFocusCallback videoFocusCallback;
    ThemeHeader* header = nullptr;
    HRecyclerFrame* recycler = nullptr;
    brls::Box* emptyBox = nullptr;
    brls::Label* emptyLabel = nullptr;
    size_t count = 0;
    bool hasMore = false;
};

RumbleNXHome::RumbleNXHome() {
    instance = this;
    const auto& palette = rumblenx::theme::current();
    this->setAxis(brls::Axis::COLUMN);
    this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    this->setBackgroundColor(palette.background);

    auto* top = new brls::Box();
    top->setAxis(brls::Axis::COLUMN);
    top->setWidthPercentage(100);
    top->setBackgroundColor(palette.header);
    top->setPadding(24, 48, 12, 48);
    rumblenx::theme::decorateHeader(top);
    this->addView(top);

    auto* headingRow = new brls::Box();
    headingRow->setAxis(brls::Axis::ROW);
    headingRow->setAlignItems(brls::AlignItems::FLEX_START);
    headingRow->setWidthPercentage(100);
    top->addView(headingRow);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    headingRow->addView(headingText);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    headingText->addView(titleRow);

    auto* titleLogo = new brls::Image();
    titleLogo->setImageFromRes("img/rumblenx_logo.png");
    titleLogo->setScalingType(brls::ImageScalingType::FIT);
    titleLogo->setHeight(44);
    titleLogo->setWidth(44.0f * LOGO_ASPECT);
    titleRow->addView(titleLogo);

    auto* title = new brls::Label();
    title->setText("· Rumble for Nintendo Switch");
    title->setFontSize(30);
    title->setTextColor(palette.headerAccent);
    title->setMarginLeft(14);
    // Keep the title on one line even while the parent is momentarily
    // narrow during screen transitions.
    title->setSingleLine(true);
    title->setShrink(0.0f);
    titleRow->addView(title);

    accountLabel = new brls::Label();
    accountLabel->setFontSize(18);
    accountLabel->setTextColor(palette.headerTextSecondary);
    accountLabel->setMarginTop(5);
    headingText->addView(accountLabel);

    profileAvatar = new ProfileAvatar();
    profileAvatar->setWidth(104);
    profileAvatar->setHeight(104);
    profileAvatar->setMarginLeft(24);
    profileAvatar->registerClickAction([this](brls::View*) {
        const auto session = rumble::loadSession();
        if (!session.signedIn() || session.username.empty() || session.username.find('@') != std::string::npos)
            return false;
        openChannel("user/" + session.username);
        return true;
    });
    headingRow->addView(profileAvatar);

    statusLabel = new brls::Label();
    statusLabel->setFontSize(17);
    statusLabel->setTextColor(palette.headerTextMuted);
    statusLabel->setMarginTop(6);
    statusLabel->setMarginBottom(14);
    top->addView(statusLabel);

    auto* actions = new brls::Box();
    actions->setAxis(brls::Axis::ROW);
    top->addView(actions);

    loginButton = makeButton("Sign in", &loginButtonLabel);
    loginButton->registerClickAction([this](brls::View*) {
        if (rumble::loadSession().signedIn()) signOut();
        else beginSignIn();
        return true;
    });
    actions->addView(loginButton);

    searchButton = makeButton("Search");
    searchButton->registerClickAction([this](brls::View*) {
        search();
        return true;
    });
    actions->addView(searchButton);

    refreshButton = makeButton("Refresh");
    refreshButton->registerClickAction([this](brls::View*) {
        refreshHome();
        return true;
    });
    actions->addView(refreshButton);

    importButton = makeButton("Import follows");
    importButton->registerClickAction([this](brls::View*) {
        importFollows();
        return true;
    });
    actions->addView(importButton);

    aboutButton = makeButton("About");
    aboutButton->registerClickAction([this](brls::View*) {
        showAbout();
        return true;
    });
    actions->addView(aboutButton);

    themeButton = makeButton("Theme");
    themeButton->registerClickAction([this](brls::View*) {
        showThemePicker();
        return true;
    });
    actions->addView(themeButton);

    auto* avatar = static_cast<ProfileAvatar*>(profileAvatar);
    avatar->downTarget = [this]() -> brls::View* { return themeButton; };
    auto avatarUp = [this]() -> brls::View* {
        return profileAvatar && profileAvatar->getVisibility() == brls::Visibility::VISIBLE ? profileAvatar : nullptr;
    };
    auto contentDown = [this]() -> brls::View* { return firstContentFocus(); };
    for (auto* button : {loginButton, searchButton, refreshButton, importButton, aboutButton, themeButton}) {
        auto* action = static_cast<ActionButton*>(button);
        action->upTarget = avatarUp;
        action->downTarget = contentDown;
    }

    errorBox = new brls::Box();
    errorBox->setAxis(brls::Axis::ROW);
    errorBox->setAlignItems(brls::AlignItems::CENTER);
    errorBox->setPadding(12, 16, 12, 16);
    errorBox->setMarginTop(12);
    errorBox->setCornerRadius(8);
    errorBox->setBackgroundColor(palette.errorBackground);
    errorBox->setVisibility(brls::Visibility::GONE);
    errorLabel = new brls::Label();
    errorLabel->setGrow(1.0f);
    errorLabel->setFontSize(16);
    errorLabel->setTextColor(palette.errorText);
    errorBox->addView(errorLabel);
    retryButton = makeButton("Retry");
    retryButton->setMarginRight(0);
    retryButton->registerClickAction([this](brls::View*) {
        if (retryAction) retryAction();
        return true;
    });
    errorBox->addView(retryButton);
    top->addView(errorBox);

    auto* scroll = new HeaderScrollingFrame();
    scroll->setFocusProxy(top);
    homeScroll = scroll;
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusable(false);
    scroll->setGrow(1.0f);
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(4, 48, 26, 48);
    scroll->setContentView(content);
    this->addView(scroll);

    heroBox = new brls::Box();
    heroBox->setAxis(brls::Axis::ROW);
    heroBox->setHeight(278);
    heroBox->setCornerRadius(12);
    heroBox->setBackgroundColor(palette.surfaceAlt);
    heroBox->setMarginBottom(20);
    heroBox->setVisibility(brls::Visibility::GONE);

    auto* heroInfo = new brls::Box();
    heroInfo->setAxis(brls::Axis::COLUMN);
    heroInfo->setWidthPercentage(44);
    heroInfo->setHeightPercentage(100);
    heroInfo->setPadding(24, 26, 24, 26);
    heroInfo->setBackgroundColor(palette.heroOverlay);
    heroBox->addView(heroInfo);

    auto heroLabel = [heroInfo](float size, NVGcolor color, float marginBottom, bool singleLine) {
        auto* label = new brls::Label();
        label->setFontSize(size);
        label->setTextColor(color);
        label->setMarginBottom(marginBottom);
        label->setSingleLine(singleLine);
        heroInfo->addView(label);
        return label;
    };
    heroLiveLabel = heroLabel(16, palette.accent, 8, true);
    heroChannelLabel = heroLabel(27, palette.textPrimary, 8, true);
    heroTitleLabel = heroLabel(20, palette.textPrimary, 12, false);
    heroMetaLabel = heroLabel(17, palette.textMuted, 0, true);

    heroPreview = new brls::Image();
    heroPreview->setGrow(1.0f);
    heroPreview->setHeightPercentage(100);
    heroPreview->setScalingType(brls::ImageScalingType::FILL);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->addView(heroPreview);
    content->addView(heroBox);

    auto remember = [this](const std::string& row, const std::string& key) { rememberFocus(row, key); };
    followingLiveRow = new ContentRow("following-live", "Following · live now", ContentRow::Kind::Videos, remember);
    liveNowRow = new ContentRow("live-now", "Live now", ContentRow::Kind::Videos, remember);
    editorPicksRow = new ContentRow("editor-picks", "Editor picks", ContentRow::Kind::Videos, remember);
    trendingRow = new ContentRow("trending", "Trending today", ContentRow::Kind::Videos, remember);
    categoryVideosRow = new ContentRow("category", "Category", ContentRow::Kind::Videos, remember);
    categoriesRow = new ContentRow("categories", "Categories", ContentRow::Kind::Categories, remember);
    searchVideosRow = new ContentRow("search-videos", "Videos", ContentRow::Kind::Videos, remember);
    searchChannelsRow = new ContentRow("search-channels", "Channels", ContentRow::Kind::Channels, remember);
    followingRow = new ContentRow("following", "Following", ContentRow::Kind::Channels, remember);

    // The hero belongs to the first row. Keep the page pinned to its top
    // while moving across that row so the hero is never cropped; other rows
    // centre themselves as usual.
    auto updateTopRowVideo = [this](const rumble::Video& video) {
        updateHero(video);
        if (homeScroll) {
            homeScroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);
            homeScroll->setContentOffsetY(0.0f, false);
        }
        brls::sync([this]() {
            if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
        });
    };
    auto updateFocusedVideo = [this](const rumble::Video& video) {
        if (homeScroll) homeScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        updateHero(video);
    };
    followingLiveRow->onVideoFocused(updateTopRowVideo);
    liveNowRow->onVideoFocused(updateFocusedVideo);
    editorPicksRow->onVideoFocused(updateFocusedVideo);
    trendingRow->onVideoFocused(updateFocusedVideo);
    categoryVideosRow->onVideoFocused(updateFocusedVideo);
    searchVideosRow->onVideoFocused(updateFocusedVideo);

    content->addView(searchVideosRow);
    content->addView(searchChannelsRow);
    content->addView(categoryVideosRow);
    content->addView(followingLiveRow);
    content->addView(liveNowRow);
    content->addView(editorPicksRow);
    content->addView(trendingRow);
    content->addView(categoriesRow);
    content->addView(followingRow);

    this->registerAction("Search", brls::BUTTON_Y, [this](brls::View*) {
        search();
        return true;
    });
    this->registerAction("Refresh", brls::BUTTON_X, [this](brls::View*) {
        refreshHome();
        return true;
    });

    updateAccount();
    refreshHome();
    brls::sync([this]() {
        if (isActiveActivity()) brls::Application::giveFocus(refreshButton);
    });

    static bool startupUpdateCheckDone = false;
    if (!startupUpdateCheckDone && rumblenx::updater::checkOnStartup() &&
        !AppVersion::getReleaseRepository().empty()) {
        startupUpdateCheckDone = true;
        // After Home has loaded, and quietly: failures are only logged.
        brls::delay(STARTUP_UPDATE_DELAY_MS, []() {
            rumblenx::updater::checkAsync(
                [](std::optional<rumblenx::updater::Release> release) {
                    if (!release) return;
                    pendingUpdate = std::move(release);
                    if (auto* home = RumbleNXHome::getInstance()) home->offerPendingUpdate();
                },
                [](const std::string&) {});
        });
    }
}

void RumbleNXHome::offerPendingUpdate() {
    // Never on top of playback or another screen; willAppear retries.
    if (!pendingUpdate || !isActiveActivity()) return;
    const rumblenx::updater::Release release = std::move(*pendingUpdate);
    pendingUpdate.reset();
    offerUpdate(release);
}

RumbleNXHome::~RumbleNXHome() {
    if (instance == this) instance = nullptr;
    if (activeRequest) activeRequest->store(true);
    if (followingRequest) followingRequest->store(true);
    if (heroPreview) Image::cancel(heroPreview);
}

brls::View* RumbleNXHome::firstContentFocus() {
    for (auto* row : {searchVideosRow, searchChannelsRow, categoryVideosRow, followingLiveRow, liveNowRow,
             editorPicksRow, trendingRow, categoriesRow, followingRow}) {
        if (auto* focus = row->defaultFocus()) return focus;
    }
    return nullptr;
}

void RumbleNXHome::updateHero(const rumble::Video& video) {
    if (!heroBox || !heroPreview) return;
    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    if (!video.thumbnailUrl.empty()) Image::with(heroPreview, video.thumbnailUrl);

    const std::string status = rumblenx::cards::videoStatus(video);
    heroLiveLabel->setText(status.empty() ? "VIDEO" : status);
    heroChannelLabel->setText(video.channelName.empty() ? "Rumble" : video.channelName);
    heroTitleLabel->setText(video.title);

    std::string meta = rumblenx::cards::durationText(video.durationSeconds);
    if (meta.empty() && video.isLive()) meta = "Press A to watch live";
    heroMetaLabel->setText(meta.empty() ? "Press A to watch" : "Length " + meta);
    heroBox->setVisibility(brls::Visibility::VISIBLE);
}

void RumbleNXHome::clearHero() {
    if (!heroBox || !heroPreview) return;
    Image::cancel(heroPreview);
    heroPreview->setImageFromRes("img/video-card-bg.png");
    heroBox->setVisibility(brls::Visibility::GONE);
}

void RumbleNXHome::willAppear(bool resetState) {
    brls::Box::willAppear(resetState);
    if (!busy && statusLabel) statusLabel->setText("");
    // A channel was followed or unfollowed on its page.
    if (rumble::follows::consumeChanged()) refreshFollowing();
    // A theme chosen while another screen was on top is applied on return.
    if (themeRebuildPending) brls::delay(THEME_REBUILD_DELAY_MS, []() { RumbleNXHome::rebuildForTheme(); });
    // An update found while another screen was on top.
    if (pendingUpdate) brls::sync([]() {
        if (auto* home = RumbleNXHome::getInstance()) home->offerPendingUpdate();
    });
}

// ================================================================= theme picker

namespace {

// Full-screen dimmed backdrop. Translucent, so Borealis keeps drawing Home
// underneath and the picker reads as a pop-up rather than a new page.
class ThemePickerOverlay : public brls::Box {
public:
    bool isTranslucent() override { return true; }
};

// A miniature Home drawn in a preset's own colours, so each card previews the
// theme it applies rather than the one currently active.
brls::Box* makeThemePreview(const rumblenx::theme::Palette& preset) {
    auto* preview = new brls::Box();
    preview->setAxis(brls::Axis::COLUMN);
    preview->setWidthPercentage(100);
    preview->setHeight(150);
    preview->setCornerRadius(8);
    preview->setBackgroundColor(preset.background);

    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setHeight(40);
    bar->setPadding(0, 14, 0, 14);
    bar->setCornerRadius(8);
    bar->setBackgroundColor(preset.header);
    rumblenx::theme::decorateHeader(bar, preset);
    preview->addView(bar);

    auto* logo = new brls::Label();
    logo->setText("RumbleNX");
    logo->setFontSize(17);
    logo->setTextColor(preset.headerAccent);
    bar->addView(logo);

    auto* cards = new brls::Box();
    cards->setAxis(brls::Axis::ROW);
    cards->setPadding(14, 14, 14, 14);
    cards->setGrow(1.0f);
    preview->addView(cards);

    for (int index = 0; index < 2; ++index) {
        auto* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setCornerRadius(6);
        card->setPadding(10, 10, 10, 10);
        card->setBackgroundColor(preset.surface);
        if (index == 0) card->setMarginRight(10);
        cards->addView(card);

        auto* thumb = new brls::Box();
        thumb->setHeight(36);
        thumb->setCornerRadius(4);
        thumb->setBackgroundColor(preset.surfaceAlt);
        card->addView(thumb);

        auto* name = new brls::Label();
        name->setText(index == 0 ? "Live stream" : "Category");
        name->setFontSize(14);
        name->setMarginTop(6);
        name->setTextColor(preset.textPrimary);
        card->addView(name);

        auto* meta = new brls::Label();
        meta->setText(index == 0 ? "LIVE · 1.2K watching" : "Gaming");
        meta->setFontSize(12);
        meta->setTextColor(index == 0 ? preset.accent : preset.textMuted);
        card->addView(meta);
    }
    return preview;
}

void chooseTheme(rumblenx::theme::Preset preset) {
    brls::Application::popActivity();
    if (preset == rumblenx::theme::current().preset) return;
    rumblenx::theme::set(preset);
    RumbleNXHome::scheduleThemeRebuild();
}

}  // namespace

void RumbleNXHome::scheduleThemeRebuild() {
    // The picker restores focus to this Home as it closes, so rebuild only
    // after it is gone; deleting Home earlier would leave focus pointing at a
    // destroyed view.
    themeRebuildPending = true;
    brls::delay(THEME_REBUILD_DELAY_MS, []() { RumbleNXHome::rebuildForTheme(); });
}

void RumbleNXHome::showThemePicker() {
    const auto& palette = rumblenx::theme::current();

    auto* page = new ThemePickerOverlay();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    page->setJustifyContent(brls::JustifyContent::CENTER);
    page->setAlignItems(brls::AlignItems::CENTER);
    page->setBackgroundColor(nvgRGBA(0, 0, 0, 160));

    auto* panel = new brls::Box();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(std::min(1040.0f, brls::Application::contentWidth - 96.0f));
    panel->setCornerRadius(16);
    panel->setBorderThickness(2.0f);
    panel->setBorderColor(palette.outline);
    panel->setBackgroundColor(palette.background);
    page->addView(panel);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(20, 28, 16, 28);
    heading->setWidthPercentage(100);
    heading->setCornerRadius(16);
    heading->setBackgroundColor(palette.header);
    rumblenx::theme::decorateHeader(heading);
    panel->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText("Theme");
    title->setFontSize(34);
    title->setTextColor(palette.headerAccent);
    headingText->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText("Choose how RumbleNX looks. Video and the chat overlay stay dark.");
    subtitle->setFontSize(17);
    subtitle->setTextColor(palette.headerTextMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) { return brls::Application::popActivity(); });
    heading->addView(backButton);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setPadding(24, 28, 28, 28);
    panel->addView(row);

    brls::View* currentCard = nullptr;
    const auto& all = rumblenx::theme::presets();
    for (size_t index = 0; index < all.size(); ++index) {
        const auto& preset = all[index];
        const bool active = preset.preset == palette.preset;

        auto* card = new ActionButton();
        card->setFocusable(true);
        card->setAxis(brls::Axis::COLUMN);
        card->setGrow(1.0f);
        card->setPadding(14, 14, 16, 14);
        card->setCornerRadius(12);
        card->setBackgroundColor(palette.surface);
        if (index + 1 < all.size()) card->setMarginRight(24);
        card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
        card->upTarget = [backButton]() -> brls::View* { return backButton; };
        const rumblenx::theme::Preset choice = preset.preset;
        card->registerClickAction([choice](brls::View*) {
            chooseTheme(choice);
            return true;
        });
        row->addView(card);

        card->addView(makeThemePreview(preset));

        auto* name = new brls::Label();
        name->setText(preset.name);
        name->setFontSize(20);
        name->setMarginTop(12);
        name->setTextColor(palette.textPrimary);
        card->addView(name);

        auto* state = new brls::Label();
        state->setText(active ? "Current theme" : "Press A to apply");
        state->setFontSize(15);
        state->setMarginTop(4);
        state->setTextColor(active ? palette.accent : palette.textMuted);
        card->addView(state);

        if (active) currentCard = card;
    }

    backButton->downTarget = [currentCard]() -> brls::View* { return currentCard; };
    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) { return brls::Application::popActivity(); });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::View* initialFocus = currentCard ? currentCard : backButton;
    brls::sync([initialFocus]() { brls::Application::giveFocus(initialFocus); });
}

void RumbleNXHome::rebuildForTheme() {
    RumbleNXHome* old = instance;
    if (!old || !themeRebuildPending) return;
    brls::Activity* activity = old->getParentActivity();
    const auto& stack = brls::Application::getActivitiesStack();
    // Only swap while Home is the visible screen; otherwise willAppear retries.
    if (!activity || stack.empty() || stack.back() != activity) return;
    // Wait for in-flight browsing requests so none completes into a deleted Home.
    if (old->busy) {
        brls::delay(THEME_REBUILD_DELAY_MS, []() { RumbleNXHome::rebuildForTheme(); });
        return;
    }

    themeRebuildPending = false;
    auto* fresh = new RumbleNXHome();
    // Move focus into the new Home before setContentView deletes the old one.
    brls::Application::giveFocus(fresh->refreshButton);
    activity->setContentView(fresh);
}

// ================================================================= state

void RumbleNXHome::setBusy(bool value, const std::string& message) {
    busy = value;
    if (!message.empty()) statusLabel->setText(message);
}

void RumbleNXHome::updateAccount() {
    const auto session = rumble::loadSession();
    if (session.signedIn()) {
        accountLabel->setText("Signed in to Rumble as " + session.username);
        loginButtonLabel->setText("Sign out");
        importButton->setVisibility(brls::Visibility::VISIBLE);
        profileAvatar->setFocusable(true);
        updateProfileImage(session.avatarUrl);
    } else {
        accountLabel->setText("Browsing as a guest · sign in to send chat messages");
        loginButtonLabel->setText("Sign in");
        importButton->setVisibility(brls::Visibility::GONE);
        profileAvatar->setFocusable(false);
        updateProfileImage("");
    }
}

void RumbleNXHome::updateProfileImage(const std::string& url) {
    if (!profileAvatar) return;
    auto* avatar = static_cast<ProfileAvatar*>(profileAvatar);
    avatar->setUrl(url);
    // Hidden while the focus could otherwise land on it with no account.
    avatar->setVisibility(url.empty() ? brls::Visibility::GONE : brls::Visibility::VISIBLE);
}

uint64_t RumbleNXHome::beginBrowsingRequest(const std::string& message) {
    // A browsing refresh/search may replace an older browsing operation.
    // Sign-in is not replaceable; it completes its own lifecycle.
    if (busy && !activeRequest) return 0;
    if (activeRequest) activeRequest->store(true);

    activeRequest = std::make_shared<std::atomic_bool>(false);
    const uint64_t id = ++requestGeneration;
    setBusy(true, message);
    this->ptrLock();
    return id;
}

bool RumbleNXHome::finishBrowsingRequest(uint64_t requestId) {
    this->ptrUnlock();
    if (requestId != requestGeneration) return false;
    activeRequest.reset();
    busy = false;
    return true;
}

void RumbleNXHome::cancelBrowsingRequest() {
    if (!activeRequest) return;
    activeRequest->store(true);
    activeRequest.reset();
    ++requestGeneration;
    busy = false;
}

void RumbleNXHome::showRequestError(const std::string& message, std::function<void()> retry) {
    errorLabel->setText(message);
    retryAction = std::move(retry);
    retryButton->setVisibility(retryAction ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    errorBox->setVisibility(brls::Visibility::VISIBLE);
}

void RumbleNXHome::clearRequestError() {
    retryAction = nullptr;
    errorBox->setVisibility(brls::Visibility::GONE);
}

void RumbleNXHome::rememberFocus(const std::string& row, const std::string& key) {
    lastFocusedRow = row;
    lastFocusedKey = key;
}

bool RumbleNXHome::isActiveActivity() {
    const auto& stack = brls::Application::getActivitiesStack();
    return !stack.empty() && (stack.back() == this->getParentActivity() || stack.back()->getContentView() == this);
}

void RumbleNXHome::restoreContentFocus() {
    if (!isActiveActivity()) return;
    const std::pair<const char*, ContentRow*> rows[] = {
        {"following-live", followingLiveRow}, {"live-now", liveNowRow}, {"editor-picks", editorPicksRow},
        {"trending", trendingRow}, {"category", categoryVideosRow}, {"categories", categoriesRow},
        {"search-videos", searchVideosRow}, {"search-channels", searchChannelsRow}, {"following", followingRow},
    };
    for (const auto& [name, row] : rows) {
        if (lastFocusedRow == name && row->focusKey(lastFocusedKey)) return;
    }
    focusFirstContentRow();
}

void RumbleNXHome::focusFirstContentRow() {
    if (!isActiveActivity()) return;
    if (auto* focus = firstContentFocus()) {
        brls::Application::giveFocus(focus);
        return;
    }
    brls::Application::giveFocus(refreshButton);
}

// ================================================================= sign-in

void RumbleNXHome::beginSignIn() {
    if (busy) return;
    clearRequestError();

    struct SignInState {
        std::string username;
        std::string password;
        rumble::SecondFactor challenge;
        bool working = false;
        brls::Label* usernameValue = nullptr;
        brls::Label* passwordValue = nullptr;
        brls::Label* statusText = nullptr;
        brls::Box* codeRow = nullptr;
        brls::Box* emailButton = nullptr;
        brls::Box* phoneButton = nullptr;
        brls::Box* codeButton = nullptr;
        brls::Box* signInButton = nullptr;
    };
    auto state = std::make_shared<SignInState>();
    state->username = rumble::loadSession().username;

    const auto& palette = rumblenx::theme::current();
    // With full-screen artwork the heading sits on the pattern and the card
    // takes the page colour so it stands out.
    const bool artwork = !palette.pageImage.empty();
    const NVGcolor cardColor = artwork ? palette.background : palette.surfaceAlt;

    auto* page = new brls::Box();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    page->setBackgroundColor(palette.header);
    rumblenx::theme::decoratePage(page);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(20, 48, 14, 48);
    heading->setWidthPercentage(100);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* title = new brls::Label();
    title->setText("Sign in to Rumble");
    title->setFontSize(32);
    title->setTextColor(palette.headerTextPrimary);
    headingText->addView(title);

    auto* subtitle = new brls::Label();
    subtitle->setText("Signing in lets you send live chat messages and import the channels you follow");
    subtitle->setFontSize(16);
    subtitle->setTextColor(palette.headerTextMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([state](brls::View*) {
        if (state->working) return true;
        return brls::Application::popActivity();
    });
    heading->addView(backButton);

    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setGrow(1.0f);
    card->setMargins(10, 48, 28, 48);
    card->setPadding(28, 40, 28, 40);
    card->setCornerRadius(18);
    card->setBackgroundColor(cardColor);
    page->addView(card);

    auto addText = [card](const std::string& text, float size, NVGcolor color, float marginTop) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(size);
        label->setTextColor(color);
        label->setMarginTop(marginTop);
        card->addView(label);
        return label;
    };

    auto addField = [card, &palette](const std::string& caption, brls::Label** value) {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setMarginTop(14);
        card->addView(row);

        auto* name = new brls::Label();
        name->setText(caption);
        name->setFontSize(19);
        name->setWidth(250);
        name->setTextColor(palette.textSecondary);
        row->addView(name);

        auto* button = makeButton("Not set", value);
        button->setWidth(560);
        row->addView(button);
        return button;
    };

    addText("Use your Rumble username (or e-mail address) and password.", 18, palette.textPrimary, 0);
    auto* usernameButton = addField("Username or e-mail", &state->usernameValue);
    auto* passwordButton = addField("Password", &state->passwordValue);
    if (!state->username.empty()) state->usernameValue->setText(state->username);

    auto* actionRow = new brls::Box();
    actionRow->setAxis(brls::Axis::ROW);
    actionRow->setMarginTop(22);
    card->addView(actionRow);
    state->signInButton = makeButton("Sign in");
    actionRow->addView(state->signInButton);

    // Second factor, shown only when Rumble asks for it.
    state->codeRow = new brls::Box();
    state->codeRow->setAxis(brls::Axis::ROW);
    state->codeRow->setMarginTop(18);
    state->codeRow->setVisibility(brls::Visibility::GONE);
    card->addView(state->codeRow);
    state->emailButton = makeButton("E-mail me a code");
    state->phoneButton = makeButton("Text me a code");
    state->codeButton = makeButton("Enter code");
    state->codeRow->addView(state->emailButton);
    state->codeRow->addView(state->phoneButton);
    state->codeRow->addView(state->codeButton);

    state->statusText = addText("", 17, palette.accent, 18);
    addText(
        "Your password is sent only to rumble.com, as Rumble's salted hash, and is never saved. "
        "RumbleNX keeps just the session on the SD card (sdmc:/config/RumbleNX/session.json) until "
        "you sign out.",
        15, palette.textMuted, 18);

    auto setStatus = [state](const std::string& text) {
        if (state->statusText) state->statusText->setText(text);
    };

    auto finish = [this, state](const rumble::Session& session) {
        state->working = false;
        this->ptrUnlock();
        updateAccount();
        brls::Application::notify("RumbleNX: signed in as " + session.username);
        brls::Application::popActivity();
        refreshHome();
    };

    auto fail = [this, state, setStatus](const std::string& error) {
        state->working = false;
        this->ptrUnlock();
        setStatus(error);
    };

    usernameButton->registerClickAction([state](brls::View*) {
        if (state->working) return true;
        brls::Application::getImeManager()->openForText(
            [state](std::string value) {
                value = trimmed(value);
                if (value.empty()) return;
                state->username = value;
                state->usernameValue->setText(value);
            },
            "Rumble username", "Your Rumble username or e-mail address", 100, state->username);
        return true;
    });

    passwordButton->registerClickAction([state](brls::View*) {
        if (state->working) return true;
        brls::Application::getImeManager()->openForPassword(
            [state](std::string value) {
                if (value.empty()) return;
                state->password = value;
                state->passwordValue->setText(std::string(std::min<size_t>(value.size(), 16), '*'));
            },
            "Rumble password", "Your Rumble password", 128, "");
        return true;
    });

    state->signInButton->registerClickAction([this, state, setStatus, finish, fail](brls::View*) {
        if (state->working) return true;
        if (state->username.empty() || state->password.empty()) {
            setStatus("Enter your username and password first.");
            return true;
        }
        state->working = true;
        setStatus("Signing in…");
        this->ptrLock();
        rumble::signInAsync(
            state->username, state->password,
            [this, state, setStatus, finish](rumble::SignInResult result) {
                // The password is no longer needed once Rumble has answered.
                state->password.clear();
                state->passwordValue->setText("Not set");
                if (!result.needsSecondFactor) {
                    finish(result.session);
                    return;
                }
                state->working = false;
                this->ptrUnlock();
                state->challenge = result.challenge;
                state->codeRow->setVisibility(brls::Visibility::VISIBLE);
                state->emailButton->setVisibility(
                    result.challenge.email ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                state->phoneButton->setVisibility(
                    result.challenge.phone ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
                setStatus(result.challenge.authenticator
                        ? "Two-factor sign-in: choose Enter code and type the code from your authenticator app."
                        : "Two-factor sign-in: ask Rumble to send you a code, then choose Enter code.");
                brls::Application::giveFocus(state->codeButton);
            },
            [state, fail](const std::string& error) {
                state->password.clear();
                state->passwordValue->setText("Not set");
                fail(error);
            });
        return true;
    });

    auto requestCode = [this, state, setStatus, fail](bool email) {
        if (state->working) return;
        state->working = true;
        setStatus(email ? "Asking Rumble to e-mail a code…" : "Asking Rumble to text a code…");
        this->ptrLock();
        rumble::requestSecondFactorCodeAsync(
            state->challenge, email,
            [this, state, setStatus](const std::string& sentTo) {
                state->working = false;
                this->ptrUnlock();
                setStatus(sentTo.empty() ? "Code sent. Choose Enter code." : "Code sent to " + sentTo + ". Choose Enter code.");
                brls::Application::giveFocus(state->codeButton);
            },
            fail);
    };
    state->emailButton->registerClickAction([requestCode](brls::View*) {
        requestCode(true);
        return true;
    });
    state->phoneButton->registerClickAction([requestCode](brls::View*) {
        requestCode(false);
        return true;
    });

    state->codeButton->registerClickAction([this, state, setStatus, finish, fail](brls::View*) {
        if (state->working) return true;
        brls::Application::getImeManager()->openForText(
            [this, state, setStatus, finish, fail](std::string code) {
                code = trimmed(code);
                if (code.empty() || state->working) return;
                state->working = true;
                setStatus("Checking the code…");
                this->ptrLock();
                rumble::verifySecondFactorAsync(state->challenge, code, finish, fail);
            },
            "Sign-in code", "The one-time code from Rumble", 12, "");
        return true;
    });

    page->registerAction("Back", brls::BUTTON_B, [state](brls::View*) {
        if (state->working) return true;
        return brls::Application::popActivity();
    });

    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([usernameButton, passwordButton, state]() {
        brls::Application::giveFocus(state->username.empty() ? usernameButton : passwordButton);
    });
}

void RumbleNXHome::signOut() {
    if (busy && !activeRequest) return;
    cancelBrowsingRequest();
    clearRequestError();
    setBusy(true, "Signing out of Rumble…");
    this->ptrLock();
    rumble::signOutAsync([this]() {
        updateAccount();
        setBusy(false, "Signed out. Browsing Rumble as a guest.");
        brls::Application::notify("RumbleNX: signed out of Rumble");
        this->ptrUnlock();
    });
}

void RumbleNXHome::importFollows() {
    if (busy) return;
    setBusy(true, "Importing the channels you follow on Rumble…");
    this->ptrLock();
    rumble::follows::importFromAccountAsync(
        [this](int added, int found) {
            busy = false;
            this->ptrUnlock();
            const std::string message = added > 0
                ? fmt::format("Added {} of {} channels from your Rumble account.", added, found)
                : fmt::format("All {} channels from your Rumble account were already followed.", found);
            setBusy(false, message);
            showMessage(message);
            rumble::follows::consumeChanged();
            refreshFollowing();
        },
        [this](const std::string& error) {
            busy = false;
            this->ptrUnlock();
            setBusy(false, "Import failed.");
            showMessage("Could not import follows:\n" + error);
        });
}

// ================================================================= home data

void RumbleNXHome::refreshHome() {
    const uint64_t requestId = beginBrowsingRequest("Loading Rumble…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    // Home and Following load side by side and appear together, so the
    // followed rows never show ahead of the rest of Home.
    struct Pending {
        std::optional<rumble::HomeData> home;
        std::string homeError;
        bool homeDone = false;
        std::optional<rumble::FollowingData> following;
        bool followingDone = false;
    };
    auto pending = std::make_shared<Pending>();

    if (followingRequest) followingRequest->store(true);
    followingRequest = std::make_shared<std::atomic_bool>(false);
    const HTTP::Cancel followingToken = followingRequest;

    auto finish = [this, requestId, pending]() {
        if (!pending->homeDone || !pending->followingDone) return;
        if (!finishBrowsingRequest(requestId)) return;
        if (pending->home) {
            showHomeData(std::move(*pending->home));
        } else {
            setBusy(false, "Home error: " + pending->homeError);
            showRequestError("Could not load Rumble: " + pending->homeError, [this]() { refreshHome(); });
            brls::Application::notify("RumbleNX: " + pending->homeError);
        }
        // After Home, so a followed live stream takes the hero.
        if (pending->following) {
            followingLiveRow->setTitle("Following · live now");
            showFollowing(std::move(*pending->following));
        }
        if (pending->home) restoreContentFocus();
    };

    rumble::loadHomeAsync(
        [pending, finish](rumble::HomeData data) {
            pending->home = std::move(data);
            pending->homeDone = true;
            finish();
        },
        [pending, finish](const std::string& error) {
            pending->homeError = error;
            pending->homeDone = true;
            finish();
        },
        requestCancel);

    rumble::loadFollowingAsync(
        [pending, finish, followingToken](rumble::FollowingData data) {
            // A newer Following refresh (an import, a follow) owns the rows.
            if (!followingToken->load()) pending->following = std::move(data);
            pending->followingDone = true;
            finish();
        },
        [pending, finish, followingToken](const std::string&) {
            // The live check failed; the followed channels still show.
            if (!followingToken->load()) {
                rumble::FollowingData data;
                data.channels = rumble::follows::load();
                pending->following = std::move(data);
            }
            pending->followingDone = true;
            finish();
        },
        followingToken);
}

void RumbleNXHome::refreshFollowing() {
    if (followingRequest) followingRequest->store(true);
    followingRequest = std::make_shared<std::atomic_bool>(false);
    const HTTP::Cancel token = followingRequest;

    // Followed channels show at once; their live streams follow once each
    // channel page has been checked.
    rumble::FollowingData immediate;
    immediate.channels = rumble::follows::load();
    showFollowing(std::move(immediate));
    if (followingRow->getVisibility() == brls::Visibility::VISIBLE) {
        followingLiveRow->setTitle("Following · checking who is live…");
    }

    this->ptrLock();
    rumble::loadFollowingAsync(
        [this, token](rumble::FollowingData data) {
            if (!token->load()) {
                followingLiveRow->setTitle("Following · live now");
                showFollowing(std::move(data));
            }
            this->ptrUnlock();
        },
        [this, token](const std::string&) {
            if (!token->load()) followingLiveRow->setTitle("Following · live now");
            this->ptrUnlock();
        },
        token);
}

void RumbleNXHome::showFollowing(rumble::FollowingData data) {
    auto play = [this](const rumble::Video& video) { playVideo(video); };
    auto open = [this](const rumble::Channel& channel) { openChannel(channel.path); };
    auto toTop = [this]() -> brls::View* {
        if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
        return refreshButton;
    };

    if (data.channels.empty()) {
        followingRow->setChannels({}, open,
            "Follow channels from their pages (or import them after signing in) to see them here.");
        followingLiveRow->clear();
        return;
    }
    followingRow->setChannels(std::move(data.channels), open, "");
    if (data.live.empty()) {
        followingLiveRow->clear();
    } else {
        const rumble::Video first = data.live.front();
        followingLiveRow->setVideos(std::move(data.live), play, "", toTop);
        updateHero(first);
    }
}

void RumbleNXHome::showHomeData(rumble::HomeData data) {
    updateAccount();
    auto play = [this](const rumble::Video& video) { playVideo(video); };

    if (!data.liveNow.items.empty()) updateHero(data.liveNow.items.front());
    else if (!data.editorPicks.items.empty()) updateHero(data.editorPicks.items.front());
    else clearHero();

    liveNowCursor = data.liveNow.cursor;
    editorPicksCursor = data.editorPicks.cursor;
    trendingCursor = data.trending.cursor;
    categoriesCursor = data.categories.cursor;

    auto toTop = [this]() -> brls::View* {
        if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
        return refreshButton;
    };

    liveNowRow->setVideos(std::move(data.liveNow.items), play,
        data.liveNowError.empty() ? "Nobody is live on Rumble right now."
                                  : "Live streams could not be loaded. Use Retry to try again.",
        toTop);
    editorPicksRow->setVideos(std::move(data.editorPicks.items), play,
        data.editorPicksError.empty() ? "Rumble returned no editor picks."
                                      : "Editor picks could not be loaded. Use Retry to try again.");
    trendingRow->setVideos(std::move(data.trending.items), play,
        data.trendingError.empty() ? "Rumble returned no trending videos."
                                   : "Trending videos could not be loaded. Use Retry to try again.");
    categoriesRow->setCategories(std::move(data.categories.items),
        [this](const rumble::Category& category) { openCategory(category); },
        data.categoriesError.empty() ? "Rumble returned no categories."
                                     : "Categories could not be loaded. Use Retry to try again.");

    liveNowRow->setHasMore(!liveNowCursor.empty());
    editorPicksRow->setHasMore(!editorPicksCursor.empty());
    trendingRow->setHasMore(!trendingCursor.empty());
    categoriesRow->setHasMore(!categoriesCursor.empty());
    liveNowRow->onNextPage(!liveNowCursor.empty(), [this]() { loadMoreLiveNow(); });
    editorPicksRow->onNextPage(!editorPicksCursor.empty(), [this]() { loadMoreEditorPicks(); });
    trendingRow->onNextPage(!trendingCursor.empty(), [this]() { loadMoreTrending(); });
    categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });

    categoryVideosRow->clear();
    searchVideosRow->clear();
    searchChannelsRow->clear();
    categoryVideosCursor.clear();
    searchVideosCursor.clear();
    searchChannelsCursor.clear();
    currentCategorySlug.clear();
    currentCategoryName.clear();
    currentSearchQuery.clear();

    if (data.hasErrors()) {
        const std::string sections = joinErrors(data);
        setBusy(false, "Home updated, but " + sections + " could not be loaded.");
        showRequestError("Some rows failed to load: " + sections + ".", [this]() { refreshHome(); });
    } else {
        clearRequestError();
        setBusy(false, "Home updated.");
    }
}

// ================================================================= about

namespace {

/// Download, check and install progress for a self-update.
class UpdatePage : public brls::Box {
public:
    explicit UpdatePage(rumblenx::updater::Release value) : release(std::move(value)) {
        const auto& palette = rumblenx::theme::current();
        this->setAxis(brls::Axis::COLUMN);
        this->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
        this->setJustifyContent(brls::JustifyContent::CENTER);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setBackgroundColor(palette.background);

        auto* title = new brls::Label();
        title->setText("Updating RumbleNX");
        title->setFontSize(34);
        title->setTextColor(palette.textPrimary);
        this->addView(title);

        auto* versions = new brls::Label();
        versions->setText(AppVersion::getVersion() + "  →  " + release.version);
        versions->setFontSize(20);
        versions->setTextColor(palette.textSecondary);
        versions->setMarginTop(8);
        versions->setMarginBottom(34);
        this->addView(versions);

        auto* track = new brls::Box();
        track->setWidth(TRACK_WIDTH);
        track->setHeight(14);
        track->setCornerRadius(7);
        track->setBackgroundColor(palette.surfaceAlt);
        this->addView(track);

        fill = new brls::Box();
        fill->setWidth(0);
        fill->setHeight(14);
        fill->setCornerRadius(7);
        fill->setBackgroundColor(palette.accent);
        track->addView(fill);

        status = new brls::Label();
        status->setFontSize(19);
        status->setTextColor(palette.textSecondary);
        status->setMarginTop(18);
        status->setMarginBottom(34);
        this->addView(status);

        button = makeButton("Cancel", &buttonLabel);
        button->setMarginRight(0);
        button->registerClickAction([this](brls::View*) {
            onButton();
            return true;
        });
        this->addView(button);

        this->registerAction("Cancel", brls::BUTTON_B, [this](brls::View*) {
            onBack();
            return true;
        });
        start();
    }

    brls::View* getDefaultFocus() override { return button; }

private:
    enum class State { Downloading, Installed, Failed };
    static constexpr float TRACK_WIDTH = 620.0f;

    void start() {
        state = State::Downloading;
        cancel = std::make_shared<std::atomic_bool>(false);
        status->setText("Starting the download…");
        buttonLabel->setText("Cancel");
        fill->setWidth(0);
        this->ptrLock();
        rumblenx::updater::installAsync(
            release,
            [this](int64_t done, int64_t total) {
                if (total <= 0) return;
                const float fraction = std::clamp(static_cast<float>(done) / static_cast<float>(total), 0.0f, 1.0f);
                fill->setWidth(TRACK_WIDTH * fraction);
                status->setText(fmt::format("Downloading… {:.1f} of {:.1f} MB", done / 1048576.0, total / 1048576.0));
            },
            [this]() {
                this->ptrUnlock();
                state = State::Installed;
                fill->setWidth(TRACK_WIDTH);
                status->setText("RumbleNX " + release.version + " is installed.");
                buttonLabel->setText("Restart now");
            },
            [this](const std::string& error) {
                this->ptrUnlock();
                state = State::Failed;
                status->setText(error + "\nRumbleNX " + AppVersion::getVersion() + " is unchanged.");
                buttonLabel->setText("Back");
            },
            cancel);
    }

    void onButton() {
        switch (state) {
            case State::Downloading:
                cancel->store(true);
                status->setText("Cancelling…");
                break;
            case State::Installed:
                if (!rumblenx::updater::restart())
                    showMessage("Close RumbleNX and open it again from the Homebrew Menu to use version " +
                                release.version + ".");
                break;
            case State::Failed:
                brls::Application::popActivity();
                break;
        }
    }

    void onBack() {
        // The page stays until the download stops, so callbacks never
        // outlive it.
        if (state == State::Downloading) {
            onButton();
        } else {
            brls::Application::popActivity();
        }
    }

    rumblenx::updater::Release release;
    State state = State::Downloading;
    HTTP::Cancel cancel;
    brls::Box* fill = nullptr;
    brls::Label* status = nullptr;
    brls::Label* buttonLabel = nullptr;
    ActionButton* button = nullptr;
};

void startUpdate(const rumblenx::updater::Release& release) {
    brls::Application::pushActivity(new brls::Activity(new UpdatePage(release)));
}

std::string releasesPage() { return "github.com/" + AppVersion::getReleaseRepository() + "/releases"; }

/// Asks whether to install a newer release, or says where to get it when
/// this copy can't update itself.
void offerUpdate(const rumblenx::updater::Release& release) {
    const std::string available =
        "RumbleNX " + release.version + " is available. You have " + AppVersion::getVersion() + ".";
    if (!release.installable() || !rumblenx::updater::canInstall()) {
        showMessage(available + "\n\nDownload it from " + releasesPage());
        return;
    }
    auto* dialog =
        new brls::Dialog(available + "\n\nDownload and install it now? It takes a minute, then RumbleNX restarts.");
    dialog->addButton("Later", []() {});
    dialog->addButton("Update", [release]() { brls::sync([release]() { startUpdate(release); }); });
    dialog->open();
}

/// About → Check for updates.
void checkForUpdates() {
    static std::atomic_bool checking{false};
    if (AppVersion::getReleaseRepository().empty() || checking.exchange(true)) return;
    brls::Application::notify("Checking for updates…");
    rumblenx::updater::checkAsync(
        [](std::optional<rumblenx::updater::Release> release) {
            checking.store(false);
            if (release) offerUpdate(*release);
            else showMessage("You're on the latest version (" + AppVersion::getVersion() + ").");
        },
        [](const std::string& error) {
            checking.store(false);
            showMessage(error);
        });
}

}  // namespace

void RumbleNXHome::showAbout() {
    const auto& palette = rumblenx::theme::current();
    auto* page = new brls::Box();
    page->setAxis(brls::Axis::COLUMN);
    page->setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
    page->setBackgroundColor(palette.background);

    auto* heading = new brls::Box();
    heading->setAxis(brls::Axis::ROW);
    heading->setAlignItems(brls::AlignItems::CENTER);
    heading->setPadding(24, 48, 18, 48);
    heading->setWidthPercentage(100);
    heading->setBackgroundColor(palette.header);
    rumblenx::theme::decorateHeader(heading);
    page->addView(heading);

    auto* headingText = new brls::Box();
    headingText->setAxis(brls::Axis::COLUMN);
    headingText->setGrow(1.0f);
    heading->addView(headingText);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    headingText->addView(titleRow);

    auto* title = new brls::Label();
    title->setText("About");
    title->setFontSize(34);
    title->setTextColor(palette.headerAccent);
    titleRow->addView(title);

    auto* titleLogo = new brls::Image();
    titleLogo->setImageFromRes("img/rumblenx_logo.png");
    titleLogo->setScalingType(brls::ImageScalingType::FIT);
    titleLogo->setHeight(44);
    titleLogo->setWidth(44.0f * LOGO_ASPECT);
    titleLogo->setMarginLeft(14);
    titleRow->addView(titleLogo);

    auto* subtitle = new brls::Label();
    subtitle->setText("Version " + AppVersion::getVersion() + " · Rumble for Nintendo Switch");
    subtitle->setFontSize(17);
    subtitle->setTextColor(palette.headerTextMuted);
    subtitle->setMarginTop(4);
    headingText->addView(subtitle);

    if (!AppVersion::getReleaseRepository().empty()) {
        auto* updateButton = makeButton("Check for updates");
        updateButton->registerClickAction([](brls::View*) {
            checkForUpdates();
            return true;
        });
        heading->addView(updateButton);

        brls::Label* startupLabel = nullptr;
        auto startupText = []() {
            return std::string("Check at startup: ") + (rumblenx::updater::checkOnStartup() ? "On" : "Off");
        };
        auto* startupButton = makeButton(startupText(), &startupLabel);
        startupButton->registerClickAction([startupLabel, startupText](brls::View*) {
            rumblenx::updater::setCheckOnStartup(!rumblenx::updater::checkOnStartup());
            startupLabel->setText(startupText());
            return true;
        });
        heading->addView(startupButton);
    }

    auto* backButton = makeButton("Back");
    backButton->setMarginRight(0);
    backButton->registerClickAction([](brls::View*) { return brls::Application::popActivity(); });
    heading->addView(backButton);

    auto* scroll = new HeaderScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setFocusable(false);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    scroll->setFocusProxy(heading);
    page->addView(scroll);

    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setPadding(6, 48, 42, 48);
    scroll->setContentView(content);

    auto addSection = [content, &palette](const std::string& text) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(25);
        label->setTextColor(palette.textPrimary);
        label->setMarginTop(18);
        label->setMarginBottom(8);
        content->addView(label);
    };
    auto addBody = [content, &palette](const std::string& text, bool faint = false) {
        auto* label = new brls::Label();
        label->setText(text);
        label->setFontSize(18);
        label->setTextColor(faint ? palette.textFaint : palette.textSecondary);
        label->setMarginBottom(8);
        content->addView(label);
    };

    auto* creditCard = new brls::Box();
    creditCard->setAxis(brls::Axis::COLUMN);
    creditCard->setPadding(18, 22, 18, 22);
    creditCard->setCornerRadius(10);
    creditCard->setBackgroundColor(palette.surfaceAlt);
    creditCard->setMarginBottom(8);
    content->addView(creditCard);

    auto* name = new brls::Label();
    name->setText("RumbleNX");
    name->setFontSize(24);
    name->setTextColor(palette.accent);
    creditCard->addView(name);

    auto* projectDescription = new brls::Label();
    projectDescription->setText(
        "A controller-first, native Rumble client for Nintendo Switch homebrew, built on kickNX by j2k.");
    projectDescription->setFontSize(18);
    projectDescription->setTextColor(palette.textSecondary);
    projectDescription->setMarginTop(7);
    creditCard->addView(projectDescription);

    addSection("Features");
    addBody(
        "• Live now, editor picks, trending today and Rumble's categories on Home\n"
        "• Search for videos and channels\n"
        "• Channel pages with live and scheduled streams and every video, page by page\n"
        "• Follow channels on the Switch, with a live-now row; signed-in accounts can import their follows\n"
        "• Live streams (with DVR), past broadcasts and uploads, with quality selection and subtitles\n"
        "• Software, Hardware and Hybrid video decoding\n"
        "• Real-time Rumble live chat with rants, docked or overlaid in four corners\n"
        "• Send chat messages from the on-screen keyboard with an emoji picker (signed in)\n"
        "• Portrait mode from the Joy-Con motion sensors, with chat and keyboard on one screen\n"
        "• OLED, Dark Grey and Rumble colour themes");

    addSection("Notes");
    addBody(
        "• RumbleNX reads Rumble's public web pages and its embed and chat services. Rumble can change "
        "them at any time, which can break features until RumbleNX is updated.\n"
        "• Following is kept on the SD card. Rumble's own follow button is not available to third-party "
        "apps; use Import follows after signing in to copy your account's list.\n"
        "• Chat timestamps are shown in UTC.");

    addSection("Credits");
    addBody(
        "RumbleNX is a Rumble port of kickNX by j2k (with Gemini and Claude), itself built on twiNX and "
        "Switchfin.\n\n"
        "Built with mpv, FFmpeg, Borealis, libcurl, nlohmann/json, Noto Emoji, libnx and devkitPro. "
        "Thanks to the yt-dlp and cocorum projects for documenting Rumble's public services, and to the "
        "Nintendo Switch homebrew community.");

    addSection("Project note");
    addBody(
        "RumbleNX is an independent homebrew project. It is not affiliated with, endorsed by or sponsored "
        "by Rumble Inc. or Nintendo.",
        true);

    page->registerAction("Back", brls::BUTTON_B, [](brls::View*) { return brls::Application::popActivity(); });
    brls::Application::pushActivity(new brls::Activity(page));
    brls::sync([backButton]() { brls::Application::giveFocus(backButton); });
}

// ================================================================= search & categories

void RumbleNXHome::search() {
    if (busy && !activeRequest) return;
    brls::Application::getImeManager()->openForText(
        [this](std::string query) {
            query = trimmed(query);
            if (!query.empty()) searchFor(query);
        },
        "Search Rumble", "Search for videos and channels", 100, currentSearchQuery);
}

void RumbleNXHome::searchFor(const std::string& query) {
    const uint64_t requestId = beginBrowsingRequest("Searching Rumble for “" + query + "”…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    rumble::searchAsync(
        query,
        [this, requestId, query](rumble::SearchResults results) {
            if (!finishBrowsingRequest(requestId)) return;
            currentSearchQuery = query;
            searchVideosCursor = results.videos.cursor;
            searchChannelsCursor = results.channels.cursor;
            auto toTop = [this]() -> brls::View* {
                if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
                return searchButton;
            };

            searchVideosRow->setTitle("Videos · " + query);
            searchVideosRow->setVideos(std::move(results.videos.items),
                [this](const rumble::Video& video) { playVideo(video); },
                results.videosError.empty() ? "No videos matched “" + query + "”."
                                            : "Video results could not be loaded: " + results.videosError,
                toTop);
            searchChannelsRow->setTitle("Channels · " + query);
            searchChannelsRow->setChannels(std::move(results.channels.items),
                [this](const rumble::Channel& channel) { openChannel(channel.path); },
                results.channelsError.empty() ? "No channels matched “" + query + "”."
                                              : "Channel results could not be loaded: " + results.channelsError);
            searchVideosRow->setHasMore(!searchVideosCursor.empty());
            searchChannelsRow->setHasMore(!searchChannelsCursor.empty());
            searchVideosRow->onNextPage(!searchVideosCursor.empty(), [this]() { loadMoreSearchVideos(); });
            searchChannelsRow->onNextPage(!searchChannelsCursor.empty(), [this]() { loadMoreSearchChannels(); });
            clearRequestError();
            setBusy(false, "Search complete.");
            if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
            if (isActiveActivity()) {
                if (auto* focus = firstContentFocus()) brls::Application::giveFocus(focus);
            }
        },
        [this, requestId, query](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Search error: " + error);
            showRequestError("Search failed: " + error, [this, query]() { searchFor(query); });
            brls::Application::notify("RumbleNX: " + error);
        },
        requestCancel);
}

void RumbleNXHome::openCategory(const rumble::Category& category) {
    const uint64_t requestId = beginBrowsingRequest("Loading " + category.name + "…");
    if (!requestId) return;
    clearRequestError();
    const HTTP::Cancel requestCancel = activeRequest;

    rumble::loadCategoryVideosAsync(
        category.slug, "",
        [this, requestId, category](rumble::CategoryVideos result) {
            if (!finishBrowsingRequest(requestId)) return;
            currentCategorySlug = category.slug;
            currentCategoryName = category.name;
            categoryVideosCursor = result.page.cursor;
            if (!result.page.items.empty()) updateHero(result.page.items.front());
            categoryVideosRow->setTitle((result.liveStreams ? "Live in " : "Recent videos in ") + category.name);
            categoryVideosRow->setVideos(std::move(result.page.items),
                [this](const rumble::Video& video) { playVideo(video); },
                "Nothing has been posted in " + category.name + " yet.",
                [this]() -> brls::View* {
                    if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
                    return refreshButton;
                });
            categoryVideosRow->setHasMore(!categoryVideosCursor.empty());
            categoryVideosRow->onNextPage(!categoryVideosCursor.empty(), [this]() { loadMoreCategoryVideos(); });
            clearRequestError();
            setBusy(false, category.name + " loaded.");
            if (homeScroll) homeScroll->setContentOffsetY(0.0f, false);
            if (isActiveActivity()) {
                if (auto* focus = categoryVideosRow->defaultFocus()) brls::Application::giveFocus(focus);
            }
        },
        [this, requestId, category](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            setBusy(false, "Category error: " + error);
            showRequestError("Could not load " + category.name + ": " + error,
                [this, category]() { openCategory(category); });
            brls::Application::notify("RumbleNX: " + error);
        },
        requestCancel);
}

// ================================================================= pagination

void RumbleNXHome::loadMoreLiveNow() {
    if (liveNowCursor.empty() || busy) return;
    const std::string cursor = liveNowCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more live streams…");
    if (!requestId) return;
    rumble::loadLiveNowPageAsync(
        cursor,
        [this, requestId](rumble::VideoPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = liveNowRow->appendVideos(std::move(page.items));
            liveNowCursor = page.cursor;
            liveNowRow->setHasMore(!liveNowCursor.empty());
            liveNowRow->onNextPage(!liveNowCursor.empty(), [this]() { loadMoreLiveNow(); });
            clearRequestError();
            setBusy(false, added ? "More live streams loaded." : "No more live streams.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more live streams: " + error, [this]() { loadMoreLiveNow(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreEditorPicks() {
    if (editorPicksCursor.empty() || busy) return;
    const std::string cursor = editorPicksCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more editor picks…");
    if (!requestId) return;
    rumble::loadEditorPicksPageAsync(
        cursor,
        [this, requestId](rumble::VideoPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = editorPicksRow->appendVideos(std::move(page.items));
            editorPicksCursor = page.cursor;
            editorPicksRow->setHasMore(!editorPicksCursor.empty());
            editorPicksRow->onNextPage(!editorPicksCursor.empty(), [this]() { loadMoreEditorPicks(); });
            clearRequestError();
            setBusy(false, added ? "More editor picks loaded." : "No more editor picks.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more editor picks: " + error, [this]() { loadMoreEditorPicks(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreTrending() {
    if (trendingCursor.empty() || busy) return;
    const std::string cursor = trendingCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more trending videos…");
    if (!requestId) return;
    rumble::loadTrendingPageAsync(
        cursor,
        [this, requestId](rumble::VideoPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = trendingRow->appendVideos(std::move(page.items));
            trendingCursor = page.cursor;
            trendingRow->setHasMore(!trendingCursor.empty());
            trendingRow->onNextPage(!trendingCursor.empty(), [this]() { loadMoreTrending(); });
            clearRequestError();
            setBusy(false, added ? "More trending videos loaded." : "No more trending videos.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more trending videos: " + error, [this]() { loadMoreTrending(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreCategories() {
    if (categoriesCursor.empty() || busy) return;
    const std::string cursor = categoriesCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more categories…");
    if (!requestId) return;
    rumble::loadCategoriesPageAsync(
        cursor,
        [this, requestId](rumble::CategoryPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoriesRow->appendCategories(std::move(page.items));
            categoriesCursor = page.cursor;
            categoriesRow->setHasMore(!categoriesCursor.empty());
            categoriesRow->onNextPage(!categoriesCursor.empty(), [this]() { loadMoreCategories(); });
            clearRequestError();
            setBusy(false, added ? "More categories loaded." : "No more categories.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more categories: " + error, [this]() { loadMoreCategories(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreCategoryVideos() {
    if (categoryVideosCursor.empty() || currentCategorySlug.empty() || busy) return;
    const std::string cursor = categoryVideosCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more of " + currentCategoryName + "…");
    if (!requestId) return;
    rumble::loadCategoryVideosAsync(
        currentCategorySlug, cursor,
        [this, requestId](rumble::CategoryVideos result) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = categoryVideosRow->appendVideos(std::move(result.page.items));
            categoryVideosCursor = result.page.cursor;
            categoryVideosRow->setHasMore(!categoryVideosCursor.empty());
            categoryVideosRow->onNextPage(!categoryVideosCursor.empty(), [this]() { loadMoreCategoryVideos(); });
            clearRequestError();
            setBusy(false, added ? "More videos loaded." : "No more videos.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more videos: " + error, [this]() { loadMoreCategoryVideos(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreSearchVideos() {
    if (searchVideosCursor.empty() || currentSearchQuery.empty() || busy) return;
    const std::string cursor = searchVideosCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more results…");
    if (!requestId) return;
    rumble::searchVideosPageAsync(
        currentSearchQuery, cursor,
        [this, requestId](rumble::VideoPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = searchVideosRow->appendVideos(std::move(page.items));
            searchVideosCursor = page.cursor;
            searchVideosRow->setHasMore(!searchVideosCursor.empty());
            searchVideosRow->onNextPage(!searchVideosCursor.empty(), [this]() { loadMoreSearchVideos(); });
            clearRequestError();
            setBusy(false, added ? "More results loaded." : "No more results.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more results: " + error, [this]() { loadMoreSearchVideos(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

void RumbleNXHome::loadMoreSearchChannels() {
    if (searchChannelsCursor.empty() || currentSearchQuery.empty() || busy) return;
    const std::string cursor = searchChannelsCursor;
    const uint64_t requestId = beginBrowsingRequest("Loading more channels…");
    if (!requestId) return;
    rumble::searchChannelsPageAsync(
        currentSearchQuery, cursor,
        [this, requestId](rumble::ChannelPage page) {
            if (!finishBrowsingRequest(requestId)) return;
            const size_t added = searchChannelsRow->appendChannels(std::move(page.items));
            searchChannelsCursor = page.cursor;
            searchChannelsRow->setHasMore(!searchChannelsCursor.empty());
            searchChannelsRow->onNextPage(!searchChannelsCursor.empty(), [this]() { loadMoreSearchChannels(); });
            clearRequestError();
            setBusy(false, added ? "More channels loaded." : "No more channels.");
        },
        [this, requestId](const std::string& error) {
            if (!finishBrowsingRequest(requestId)) return;
            showRequestError("Could not load more channels: " + error, [this]() { loadMoreSearchChannels(); });
            setBusy(false, "Paging error: " + error);
        },
        activeRequest);
}

// ================================================================= navigation

void RumbleNXHome::openChannel(const std::string& channelPath) {
    if (channelPath.empty()) {
        statusLabel->setText("This result has no Rumble channel.");
        return;
    }
    brls::Application::pushActivity(new brls::Activity(new RumbleChannelPage(channelPath)));
}

void RumbleNXHome::playVideo(const rumble::Video& video) {
    if (busy && !activeRequest) return;
    RemoteView::playVideo(video);
}
