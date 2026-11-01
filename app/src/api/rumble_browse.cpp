#include "api/rumble_browse.hpp"

#include "api/rumble.hpp"
#include "api/rumble_follows.hpp"
#include "api/rumble_parse.hpp"
#include "api/rumble_playlist.hpp"
#include "utils/thread.hpp"

#include <borealis.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rumble {
namespace {

constexpr size_t CATEGORY_PAGE_SIZE = 40;
constexpr size_t MAX_FOLLOWED_CHECKS = 40;
constexpr int FOLLOWED_CHECK_WORKERS = 3;

int pageNumber(const std::string& cursor) {
    if (cursor.empty()) return 1;
    try {
        return std::max(1, std::stoi(cursor));
    } catch (...) {
        return 1;
    }
}

std::string withPage(const std::string& path, int page) {
    if (page <= 1) return path;
    return path + (path.find('?') == std::string::npos ? "?page=" : "&page=") + std::to_string(page);
}

/// Rumble marks a following page with <link rel=next>; channel search only
/// links it from its pagination bar.
bool linksPage(const std::string& html, int page) {
    const std::string number = std::to_string(page);
    for (const char* prefix : {"?page=", "&page=", "&amp;page="}) {
        const std::string needle = prefix + number;
        size_t pos = 0;
        while ((pos = html.find(needle, pos)) != std::string::npos) {
            const size_t end = pos + needle.size();
            if (end >= html.size() || !std::isdigit(static_cast<unsigned char>(html[end]))) return true;
            pos = end;
        }
    }
    return false;
}

std::string nextCursor(const std::string& html, int page, bool hasItems) {
    if (!hasItems) return {};
    return parse::hasNextPage(html) || linksPage(html, page + 1) ? std::to_string(page + 1) : std::string();
}

VideoPage videoPage(const std::string& path, const std::string& cursor, const HTTP::Cancel& cancel) {
    const int page = pageNumber(cursor);
    const std::string html = fetchPage(withPage(path, page), cancel);
    VideoPage result;
    result.items = parse::parseVideoListing(html);
    result.cursor = nextCursor(html, page, !result.items.empty());
    result.loaded = true;
    return result;
}

void fillChannel(std::vector<Video>& videos, const Channel& channel) {
    for (auto& video : videos) {
        if (video.channelPath.empty()) video.channelPath = channel.path;
        if (video.channelName.empty()) video.channelName = channel.name;
        if (video.channelAvatarUrl.empty() && video.channelPath == channel.path)
            video.channelAvatarUrl = channel.avatarUrl;
        if (video.channelPath == channel.path && channel.verified) video.channelVerified = true;
    }
}

std::string validChannelPath(const std::string& channelPath) {
    const std::string path = parse::channelPath("/" + channelPath);
    if (path.empty()) throw std::runtime_error("That is not a Rumble channel address.");
    return path;
}

// ------------------------------------------------------------- categories

struct CategoryCache {
    std::mutex mutex;
    std::vector<Category> items;
};

CategoryCache& categoryCache() {
    static CategoryCache cache;
    return cache;
}

CategoryPage categoriesPage(const std::string& cursor, const HTTP::Cancel& cancel) {
    size_t offset = 0;
    if (!cursor.empty()) {
        try {
            offset = static_cast<size_t>(std::max(0, std::stoi(cursor)));
        } catch (...) {
            offset = 0;
        }
    }

    auto& cache = categoryCache();
    std::vector<Category> all;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (offset > 0) all = cache.items;
    }
    // The first page always reloads (Home refresh); later pages come from
    // the list that page fetched, since /browse lists every category at once.
    if (all.empty()) {
        all = parse::parseCategories(fetchPage("/browse", cancel));
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.items = all;
    }

    CategoryPage page;
    page.loaded = true;
    if (offset < all.size()) {
        const size_t end = std::min(all.size(), offset + CATEGORY_PAGE_SIZE);
        page.items.assign(all.begin() + static_cast<std::ptrdiff_t>(offset),
            all.begin() + static_cast<std::ptrdiff_t>(end));
        if (end < all.size()) page.cursor = std::to_string(end);
    }
    return page;
}

// ------------------------------------------------------------- async plumbing

template <typename Result, typename Work, typename Callback>
void runAsync(Work work, Callback success, ErrorCallback failure, HTTP::Cancel cancel, std::string unknownError) {
    ThreadPool::instance().submit([work = std::move(work), success = std::move(success),
                                      failure = std::move(failure), cancel = std::move(cancel),
                                      unknownError = std::move(unknownError)](HTTP&) mutable {
        try {
            throwIfCancelled(cancel);
            Result result = work(cancel);
            throwIfCancelled(cancel);
            brls::sync(std::bind(std::move(success), std::move(result)));
        } catch (const std::exception& exception) {
            brls::sync(std::bind(std::move(failure), std::string(exception.what())));
        } catch (...) {
            brls::sync(std::bind(std::move(failure), unknownError));
        }
    });
}

std::string sectionError(const std::exception& exception) {
    const std::string message = exception.what();
    return message.empty() ? "Unknown request error" : message;
}

}  // namespace

// ================================================================= home

void loadHomeAsync(HomeCallback success, ErrorCallback failure, HTTP::Cancel cancel) {
    struct State {
        std::mutex mutex;
        HomeData data;
        std::atomic_int remaining{4};
        HomeCallback success;
        ErrorCallback failure;
        HTTP::Cancel cancel;
    };
    auto state = std::make_shared<State>();
    state->data.account = loadSession();
    state->success = std::move(success);
    state->failure = std::move(failure);
    state->cancel = std::move(cancel);

    auto finish = [state]() {
        if (state->remaining.fetch_sub(1) != 1) return;
        HomeData data;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            data = std::move(state->data);
        }
        if (state->cancel && state->cancel->load()) {
            brls::sync(std::bind(std::move(state->failure), std::string("Request cancelled")));
            return;
        }
        if (!data.liveNow.loaded && !data.editorPicks.loaded && !data.trending.loaded && !data.categories.loaded) {
            std::string error = data.liveNowError.empty() ? data.trendingError : data.liveNowError;
            if (error.empty()) error = "Rumble did not return any home rows. Check the connection and retry.";
            brls::sync(std::bind(std::move(state->failure), error));
            return;
        }
        brls::sync(std::bind(std::move(state->success), std::move(data)));
    };

    auto section = [state, finish](auto load, auto store, auto storeError) {
        ThreadPool::instance().submit([state, finish, load, store, storeError](HTTP&) {
            try {
                auto result = load(state->cancel);
                std::lock_guard<std::mutex> lock(state->mutex);
                store(state->data, std::move(result));
            } catch (const std::exception& exception) {
                std::lock_guard<std::mutex> lock(state->mutex);
                storeError(state->data, sectionError(exception));
            } catch (...) {
                std::lock_guard<std::mutex> lock(state->mutex);
                storeError(state->data, std::string("Unknown request error"));
            }
            finish();
        });
    };

    section([](const HTTP::Cancel& c) { return videoPage("/browse/live", "", c); },
        [](HomeData& d, VideoPage p) { d.liveNow = std::move(p); },
        [](HomeData& d, std::string e) { d.liveNowError = std::move(e); });
    section([](const HTTP::Cancel& c) { return videoPage("/editor-picks", "", c); },
        [](HomeData& d, VideoPage p) { d.editorPicks = std::move(p); },
        [](HomeData& d, std::string e) { d.editorPicksError = std::move(e); });
    section([](const HTTP::Cancel& c) { return videoPage("/videos?sort=views&date=today", "", c); },
        [](HomeData& d, VideoPage p) { d.trending = std::move(p); },
        [](HomeData& d, std::string e) { d.trendingError = std::move(e); });
    section([](const HTTP::Cancel& c) { return categoriesPage("", c); },
        [](HomeData& d, CategoryPage p) { d.categories = std::move(p); },
        [](HomeData& d, std::string e) { d.categoriesError = std::move(e); });
}

// ================================================================= following

void loadFollowingAsync(FollowingCallback success, ErrorCallback failure, HTTP::Cancel cancel) {
    struct State {
        std::mutex mutex;
        std::vector<Channel> channels;
        std::vector<Video> live;
        std::vector<Channel> updated;
        std::atomic_size_t next{0};
        std::atomic_int workers{0};
        size_t limit = 0;
        FollowingCallback success;
        ErrorCallback failure;
        HTTP::Cancel cancel;
    };
    auto state = std::make_shared<State>();
    state->channels = follows::load();
    state->limit = std::min(state->channels.size(), MAX_FOLLOWED_CHECKS);
    state->success = std::move(success);
    state->failure = std::move(failure);
    state->cancel = std::move(cancel);

    if (state->limit == 0) {
        FollowingData empty;
        empty.channels = state->channels;
        brls::sync(std::bind(std::move(state->success), std::move(empty)));
        return;
    }

    const int workers = static_cast<int>(std::min<size_t>(FOLLOWED_CHECK_WORKERS, state->limit));
    state->workers.store(workers);
    for (int worker = 0; worker < workers; ++worker) {
        ThreadPool::instance().submit([state](HTTP&) {
            for (size_t index = state->next.fetch_add(1); index < state->limit;
                 index = state->next.fetch_add(1)) {
                if (state->cancel && state->cancel->load()) break;
                const Channel followed = state->channels[index];
                try {
                    const std::string html = fetchPage("/" + followed.path, state->cancel);
                    Channel channel = parse::parseChannelHeader(html, followed.path);
                    std::vector<Video> videos = parse::parseVideoListing(html);
                    fillChannel(videos, channel);
                    std::lock_guard<std::mutex> lock(state->mutex);
                    for (auto& video : videos) {
                        if (video.isLive()) state->live.push_back(std::move(video));
                    }
                    state->updated.push_back(std::move(channel));
                } catch (const std::exception& error) {
                    brls::Logger::warning("RumbleNX: could not check {}: {}", followed.path, error.what());
                }
            }

            if (state->workers.fetch_sub(1) != 1) return;
            if (state->cancel && state->cancel->load()) {
                brls::sync(std::bind(std::move(state->failure), std::string("Request cancelled")));
                return;
            }

            follows::refreshDetails(state->updated);
            FollowingData data;
            data.channels = follows::load();
            data.live = std::move(state->live);
            std::stable_sort(data.live.begin(), data.live.end(),
                [](const Video& a, const Video& b) { return a.watching > b.watching; });
            brls::sync(std::bind(std::move(state->success), std::move(data)));
        });
    }
}

// ================================================================= pages

void loadLiveNowPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel) {
    runAsync<VideoPage>([cursor](const HTTP::Cancel& c) { return videoPage("/browse/live", cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown live streams request error");
}

void loadEditorPicksPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel) {
    runAsync<VideoPage>([cursor](const HTTP::Cancel& c) { return videoPage("/editor-picks", cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown editor picks request error");
}

void loadTrendingPageAsync(const std::string& cursor, VideoPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel) {
    runAsync<VideoPage>(
        [cursor](const HTTP::Cancel& c) { return videoPage("/videos?sort=views&date=today", cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown trending request error");
}

void loadCategoriesPageAsync(const std::string& cursor, CategoryPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel) {
    runAsync<CategoryPage>([cursor](const HTTP::Cancel& c) { return categoriesPage(cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown categories request error");
}

void loadCategoryVideosAsync(const std::string& slug, const std::string& cursor, CategoryVideosCallback success,
    ErrorCallback failure, HTTP::Cancel cancel) {
    runAsync<CategoryVideos>(
        [slug, cursor](const HTTP::Cancel& c) {
            if (slug.empty()) throw std::runtime_error("No category was selected.");
            const std::string base = "/category/" + percentEncode(slug);
            CategoryVideos result;
            if (cursor.rfind("videos:", 0) == 0) {
                result.liveStreams = false;
                result.page = videoPage(base + "/videos", cursor.substr(7), c);
                if (!result.page.cursor.empty()) result.page.cursor = "videos:" + result.page.cursor;
                return result;
            }

            const std::string livePage = cursor.rfind("live:", 0) == 0 ? cursor.substr(5) : std::string();
            result.page = videoPage(base, livePage, c);
            if (result.page.items.empty() && livePage.empty()) {
                // Nobody is live in this category; show its recent videos.
                result.liveStreams = false;
                result.page = videoPage(base + "/videos", "", c);
                if (!result.page.cursor.empty()) result.page.cursor = "videos:" + result.page.cursor;
                return result;
            }
            if (!result.page.cursor.empty()) result.page.cursor = "live:" + result.page.cursor;
            return result;
        },
        std::move(success), std::move(failure), std::move(cancel), "Unknown category request error");
}

// ================================================================= search

namespace {

VideoPage searchVideosPage(const std::string& query, const std::string& cursor, const HTTP::Cancel& cancel) {
    return videoPage("/search/video?q=" + percentEncode(query), cursor, cancel);
}

ChannelPage searchChannelsPage(const std::string& query, const std::string& cursor, const HTTP::Cancel& cancel) {
    const int page = pageNumber(cursor);
    const std::string html = fetchPage(withPage("/search/channel?q=" + percentEncode(query), page), cancel);
    ChannelPage result;
    result.items = parse::parseChannelSearch(html);
    result.cursor = nextCursor(html, page, !result.items.empty());
    result.loaded = true;
    return result;
}

}  // namespace

void searchAsync(const std::string& query, SearchCallback success, ErrorCallback failure, HTTP::Cancel cancel) {
    struct State {
        std::mutex mutex;
        SearchResults results;
        std::atomic_int remaining{2};
        SearchCallback success;
        ErrorCallback failure;
        HTTP::Cancel cancel;
    };
    auto state = std::make_shared<State>();
    state->success = std::move(success);
    state->failure = std::move(failure);
    state->cancel = std::move(cancel);

    auto finish = [state]() {
        if (state->remaining.fetch_sub(1) != 1) return;
        SearchResults results;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            results = std::move(state->results);
        }
        if (state->cancel && state->cancel->load()) {
            brls::sync(std::bind(std::move(state->failure), std::string("Request cancelled")));
        } else if (!results.videos.loaded && !results.channels.loaded) {
            brls::sync(std::bind(std::move(state->failure), results.videosError));
        } else {
            brls::sync(std::bind(std::move(state->success), std::move(results)));
        }
    };

    ThreadPool::instance().submit([state, finish, query](HTTP&) {
        try {
            VideoPage page = searchVideosPage(query, "", state->cancel);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->results.videos = std::move(page);
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->results.videosError = sectionError(exception);
        }
        finish();
    });
    ThreadPool::instance().submit([state, finish, query](HTTP&) {
        try {
            ChannelPage page = searchChannelsPage(query, "", state->cancel);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->results.channels = std::move(page);
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->results.channelsError = sectionError(exception);
        }
        finish();
    });
}

void searchVideosPageAsync(const std::string& query, const std::string& cursor, VideoPageCallback success,
    ErrorCallback failure, HTTP::Cancel cancel) {
    runAsync<VideoPage>([query, cursor](const HTTP::Cancel& c) { return searchVideosPage(query, cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown search request error");
}

void searchChannelsPageAsync(const std::string& query, const std::string& cursor, ChannelListCallback success,
    ErrorCallback failure, HTTP::Cancel cancel) {
    runAsync<ChannelPage>([query, cursor](const HTTP::Cancel& c) { return searchChannelsPage(query, cursor, c); },
        std::move(success), std::move(failure), std::move(cancel), "Unknown search request error");
}

// ================================================================= channels

void loadChannelPageAsync(const std::string& channelPath, ChannelPageCallback success, ErrorCallback failure,
    HTTP::Cancel cancel) {
    runAsync<ChannelPageData>(
        [channelPath](const HTTP::Cancel& c) {
            const std::string path = validChannelPath(channelPath);
            const std::string html = fetchPage("/" + path, c);

            ChannelPageData data;
            data.channel = parse::parseChannelHeader(html, path);
            std::vector<Video> videos = parse::parseVideoListing(html);
            fillChannel(videos, data.channel);
            data.videos.cursor = nextCursor(html, 1, !videos.empty());
            data.videos.loaded = true;
            for (auto& video : videos) {
                if (video.isLive() || video.isUpcoming()) data.live.push_back(std::move(video));
                else data.videos.items.push_back(std::move(video));
            }

            try {
                data.channel.description = parse::parseChannelDescription(fetchPage("/" + path + "/about", c));
            } catch (const std::exception& error) {
                throwIfCancelled(c);
                brls::Logger::info("RumbleNX: no description for {}: {}", path, error.what());
            }

            data.following = follows::isFollowing(path);
            if (data.following) follows::refreshDetails({data.channel});
            return data;
        },
        std::move(success), std::move(failure), std::move(cancel), "Unknown channel request error");
}

void loadChannelVideosPageAsync(const std::string& channelPath, const std::string& cursor,
    VideoPageCallback success, ErrorCallback failure, HTTP::Cancel cancel) {
    runAsync<VideoPage>(
        [channelPath, cursor](const HTTP::Cancel& c) {
            const std::string path = validChannelPath(channelPath);
            const int page = pageNumber(cursor);
            const std::string html = fetchPage(withPage("/" + path, page), c);
            VideoPage result;
            result.items = parse::parseVideoListing(html);
            fillChannel(result.items, parse::parseChannelHeader(html, path));
            result.items.erase(std::remove_if(result.items.begin(), result.items.end(),
                                   [](const Video& video) { return video.isLive() || video.isUpcoming(); }),
                result.items.end());
            result.cursor = nextCursor(html, page, !result.items.empty());
            result.loaded = true;
            return result;
        },
        std::move(success), std::move(failure), std::move(cancel), "Unknown channel request error");
}

}  // namespace rumble
