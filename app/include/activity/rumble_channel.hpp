#pragma once

#include "api/http.hpp"
#include "api/rumble_browse.hpp"

#include <borealis.hpp>

#include <functional>
#include <string>

class HRecyclerFrame;
class ThemeHeader;

class ChannelActionButton : public brls::Box {
public:
    ChannelActionButton() { this->setFocusable(true); }

    std::function<brls::View*()> downTarget;
    std::function<brls::View*()> upTarget;

    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override {
        if (direction == brls::FocusDirection::DOWN && downTarget) {
            if (brls::View* target = downTarget()) return target;
        }
        if (direction == brls::FocusDirection::UP && upTarget) {
            if (brls::View* target = upTarget()) return target;
        }
        return brls::Box::getNextFocus(direction, currentView);
    }
};

/// A Rumble channel ("c/Name") or user page ("user/Name"): header, follow
/// button, live/scheduled streams and a paged list of videos.
class RumbleChannelPage : public brls::Box {
public:
    explicit RumbleChannelPage(std::string channelPath);
    ~RumbleChannelPage() override;

    brls::View* getDefaultFocus() override;
    void willAppear(bool resetState = false) override;
    void willDisappear(bool resetState = false) override;

private:
    void load();
    // Called as soon as the page is being left. Cancels in-flight requests and
    // makes any late async callback a no-op so it never touches (or gives
    // focus to) views of a page that is already being torn down.
    void markClosing();
    void showData(rumble::ChannelPageData data);
    void loadMoreVideos();
    void playLive();
    void updateFollowButton();
    void toggleFollow();
    brls::View* getActionFocus();
    brls::View* getFirstRowFocus();

    bool closing = false;
    std::string channelPath;
    rumble::ChannelPageData data;
    HTTP::Cancel requestCancel;
    std::string videosCursor;
    bool loadingMore = false;
    bool following = false;

    brls::ScrollingFrame* scroll = nullptr;
    brls::Box* content = nullptr;
    brls::Box* headerBox = nullptr;
    brls::Label* status = nullptr;
    ChannelActionButton* backButton = nullptr;
    brls::Image* bannerImage = nullptr;
    brls::Box* identity = nullptr;
    brls::View* avatar = nullptr;
    brls::Label* name = nullptr;
    brls::Label* liveState = nullptr;
    brls::Label* headline = nullptr;
    brls::Label* description = nullptr;
    brls::Label* metadata = nullptr;
    ChannelActionButton* playButton = nullptr;
    ChannelActionButton* followButton = nullptr;
    brls::Label* followButtonLabel = nullptr;
    ChannelActionButton* refreshButton = nullptr;
    brls::Box* liveSection = nullptr;
    brls::Box* videosSection = nullptr;
    ThemeHeader* liveHeader = nullptr;
    ThemeHeader* videosHeader = nullptr;
    brls::Label* videosEmpty = nullptr;
    HRecyclerFrame* liveRecycler = nullptr;
    HRecyclerFrame* videosRecycler = nullptr;
};
