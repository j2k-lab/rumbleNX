#pragma once

#include "api/http.hpp"
#include "api/rumble_types.hpp"

#include <borealis.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace rumble {
struct HomeData;
struct FollowingData;
struct SearchResults;
}

class ContentRow;

/// Home: sign-in, search, followed channels, live now, editor picks,
/// trending today and categories.
class RumbleNXHome : public brls::Box {
public:
    RumbleNXHome();
    ~RumbleNXHome() override;

    static RumbleNXHome* getInstance() { return instance; }

    void willAppear(bool resetState = false) override;

    /// Rebuilds Home shortly, once any picker on top of it has closed.
    static void scheduleThemeRebuild();

private:
    void setBusy(bool value, const std::string& message = "");
    void updateAccount();
    void updateProfileImage(const std::string& url);
    void beginSignIn();
    void signOut();
    void importFollows();
    void refreshHome();
    void refreshFollowing();
    void search();
    void showAbout();
    void showThemePicker();
    /// Replaces Home with a freshly built one so the new theme takes effect.
    static void rebuildForTheme();
    void searchFor(const std::string& query);
    void openCategory(const rumble::Category& category);
    void openChannel(const std::string& channelPath);
    void playVideo(const rumble::Video& video);
    void updateHero(const rumble::Video& video);
    void clearHero();
    void showHomeData(rumble::HomeData data);
    void showFollowing(rumble::FollowingData data);
    /// Offers an update found by the startup check once Home is on top.
    void offerPendingUpdate();

    uint64_t beginBrowsingRequest(const std::string& message);
    bool finishBrowsingRequest(uint64_t requestId);
    void cancelBrowsingRequest();
    void showRequestError(const std::string& message, std::function<void()> retry);
    void clearRequestError();

    void rememberFocus(const std::string& row, const std::string& key);
    void restoreContentFocus();
    void focusFirstContentRow();
    bool isActiveActivity();
    brls::View* firstContentFocus();

    void loadMoreLiveNow();
    void loadMoreEditorPicks();
    void loadMoreTrending();
    void loadMoreCategories();
    void loadMoreCategoryVideos();
    void loadMoreSearchVideos();
    void loadMoreSearchChannels();

    brls::Label* accountLabel = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Label* loginButtonLabel = nullptr;
    brls::View* profileAvatar = nullptr;
    brls::Box* loginButton = nullptr;
    brls::Box* searchButton = nullptr;
    brls::Box* refreshButton = nullptr;
    brls::Box* importButton = nullptr;
    brls::Box* aboutButton = nullptr;
    brls::Box* themeButton = nullptr;

    brls::Box* errorBox = nullptr;
    brls::Label* errorLabel = nullptr;
    brls::Box* retryButton = nullptr;

    brls::ScrollingFrame* homeScroll = nullptr;

    brls::Box* heroBox = nullptr;
    brls::Image* heroPreview = nullptr;
    brls::Label* heroLiveLabel = nullptr;
    brls::Label* heroChannelLabel = nullptr;
    brls::Label* heroTitleLabel = nullptr;
    brls::Label* heroMetaLabel = nullptr;
    std::function<void()> retryAction;

    ContentRow* followingLiveRow = nullptr;
    ContentRow* liveNowRow = nullptr;
    ContentRow* editorPicksRow = nullptr;
    ContentRow* trendingRow = nullptr;
    ContentRow* categoryVideosRow = nullptr;
    ContentRow* categoriesRow = nullptr;
    ContentRow* searchVideosRow = nullptr;
    ContentRow* searchChannelsRow = nullptr;
    ContentRow* followingRow = nullptr;

    HTTP::Cancel activeRequest;
    HTTP::Cancel followingRequest;
    uint64_t requestGeneration = 0;

    std::string liveNowCursor;
    std::string editorPicksCursor;
    std::string trendingCursor;
    std::string categoriesCursor;
    std::string categoryVideosCursor;
    std::string searchVideosCursor;
    std::string searchChannelsCursor;
    std::string currentSearchQuery;
    std::string currentCategorySlug;
    std::string currentCategoryName;

    std::string lastFocusedRow;
    std::string lastFocusedKey;

    bool busy = false;

    inline static RumbleNXHome* instance = nullptr;
    inline static bool themeRebuildPending = false;
    static constexpr long THEME_REBUILD_DELAY_MS = 500;
    static constexpr long STARTUP_UPDATE_DELAY_MS = 4000;
};
