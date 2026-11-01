#pragma once

#include "api/http.hpp"
#include "api/rumble_auth.hpp"
#include "api/rumble_types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rumble {

struct HomeData {
    Session account;
    VideoPage liveNow;
    VideoPage editorPicks;
    VideoPage trending;
    CategoryPage categories;
    std::string liveNowError;
    std::string editorPicksError;
    std::string trendingError;
    std::string categoriesError;

    bool hasErrors() const {
        return !liveNowError.empty() || !editorPicksError.empty() || !trendingError.empty() ||
               !categoriesError.empty();
    }
};

struct FollowingData {
    /// Live streams from followed channels, busiest first.
    std::vector<Video> live;
    /// Every followed channel, in follow order.
    std::vector<Channel> channels;
};

struct SearchResults {
    VideoPage videos;
    ChannelPage channels;
    std::string videosError;
    std::string channelsError;
};

struct ChannelPageData {
    Channel channel;
    /// Live and scheduled streams shown first on the channel page.
    std::vector<Video> live;
    /// Uploads and past broadcasts, newest first.
    VideoPage videos;
    bool following = false;
};

struct CategoryVideos {
    VideoPage page;
    /// True for live streams; false when nobody was live and the category's
    /// recent videos were returned instead.
    bool liveStreams = true;
};

using HomeCallback = std::function<void(HomeData)>;
using FollowingCallback = std::function<void(FollowingData)>;
using SearchCallback = std::function<void(SearchResults)>;
using ChannelPageCallback = std::function<void(ChannelPageData)>;
using VideoPageCallback = std::function<void(VideoPage)>;
using ChannelListCallback = std::function<void(ChannelPage)>;
using CategoryPageCallback = std::function<void(CategoryPage)>;
using CategoryVideosCallback = std::function<void(CategoryVideos)>;

/// Live now, editor picks, trending today and the busiest categories.
void loadHomeAsync(HomeCallback success, ErrorCallback failure, HTTP::Cancel cancel = nullptr);

/// Followed channels and which of them are live (one channel page each, a
/// few at a time).
void loadFollowingAsync(FollowingCallback success, ErrorCallback failure, HTTP::Cancel cancel = nullptr);

void loadLiveNowPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);
void loadEditorPicksPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);
void loadTrendingPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);
void loadCategoriesPageAsync(const std::string& cursor, CategoryPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);

/// First call with an empty cursor; later pages pass the returned cursor.
void loadCategoryVideosAsync(const std::string& slug, const std::string& cursor, CategoryVideosCallback success,
    ErrorCallback failure, HTTP::Cancel cancel = nullptr);

void searchAsync(const std::string& query, SearchCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);
void searchVideosPageAsync(const std::string& query, const std::string& cursor, VideoPageCallback success,
    ErrorCallback failure, HTTP::Cancel cancel = nullptr);
void searchChannelsPageAsync(const std::string& query, const std::string& cursor, ChannelListCallback success,
    ErrorCallback failure, HTTP::Cancel cancel = nullptr);

void loadChannelPageAsync(const std::string& channelPath, ChannelPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel = nullptr);
void loadChannelVideosPageAsync(const std::string& channelPath, const std::string& cursor,
    VideoPageCallback success, ErrorCallback failure, HTTP::Cancel cancel = nullptr);

}  // namespace rumble
