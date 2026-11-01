#pragma once

#include "api/rumble_types.hpp"
#include "view/recycling_grid.hpp"

#include <borealis.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/// Cards shared by Home and channel pages: videos (live, scheduled and
/// uploaded), channels and categories, plus their recycler data sources.
namespace rumblenx::cards {

/// 950 -> "950", 1440 -> "1.4K", 2500000 -> "2.5M".
std::string compactNumber(int64_t value);
/// 754 -> "12:34", 5293 -> "1:28:13".
std::string durationText(int64_t seconds);
/// "Channel · LIVE · 1.2K watching", "Channel · 12K views · 12:34", ...
std::string videoSubtitle(const rumble::Video& video, bool withChannel = true);
/// "LIVE · 1.2K watching" / "Scheduled stream" / "12K views".
std::string videoStatus(const rumble::Video& video);
std::string followersText(const rumble::Channel& channel);

using FocusTarget = std::function<brls::View*()>;

/// Circular picture with a placeholder, used for channel avatars.
class CircularAvatar : public brls::View {
public:
    CircularAvatar();
    ~CircularAvatar() override;

    void setUrl(const std::string& url);
    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style,
        brls::FrameContext* ctx) override;

    /// Draw a focus ring (for focusable avatars).
    bool focusRing = false;

private:
    brls::Image* loader = nullptr;
    std::string currentUrl;
};

class CardCell : public RecyclingGridItem {
public:
    brls::View* getNextFocus(brls::FocusDirection direction, brls::View* currentView) override;

    std::function<void()> onFocused;
    FocusTarget upTarget;
    FocusTarget downTarget;

protected:
    CardCell();
};

class VideoCell : public CardCell {
public:
    VideoCell();
    ~VideoCell() override;

    void prepareForReuse() override;
    void cacheForReuse() override;
    void configure(const rumble::Video& video, bool withChannel);

private:
    brls::Image* picture = nullptr;
    brls::Box* badge = nullptr;
    brls::Label* badgeLabel = nullptr;
    brls::Box* duration = nullptr;
    brls::Label* durationLabel = nullptr;
    brls::Label* title = nullptr;
    brls::Label* subtitle = nullptr;
};

class ChannelCell : public CardCell {
public:
    ChannelCell();

    void prepareForReuse() override;
    void cacheForReuse() override;
    void configure(const rumble::Channel& channel);

private:
    CircularAvatar* avatar = nullptr;
    brls::Label* name = nullptr;
    brls::Label* detail = nullptr;
};

class CategoryCell : public CardCell {
public:
    CategoryCell();
    ~CategoryCell() override;

    void prepareForReuse() override;
    void cacheForReuse() override;
    void configure(const rumble::Category& category);

private:
    brls::Image* picture = nullptr;
    brls::Label* title = nullptr;
};

std::string videoKey(const rumble::Video& video);
std::string channelKey(const rumble::Channel& channel);
std::string categoryKey(const rumble::Category& category);

/// Data source over a list of items shown by one cell type.
template <typename Item, typename Cell>
class CardDataSource : public RecyclingGridDataSource {
public:
    using Selection = std::function<void(const Item&)>;
    using Focus = std::function<void(const Item&)>;
    using Configure = std::function<void(Cell*, const Item&)>;
    using Key = std::function<std::string(const Item&)>;

    CardDataSource(std::string identifier, std::vector<Item> data, Configure configure, Key key,
        Selection selected, Focus focused, FocusTarget up, FocusTarget down)
        : identifier(std::move(identifier)),
          items(std::move(data)),
          configureCell(std::move(configure)),
          keyOf(std::move(key)),
          onSelected(std::move(selected)),
          onFocused(std::move(focused)),
          upTarget(std::move(up)),
          downTarget(std::move(down)) {}

    size_t getItemCount() override { return items.size(); }

    RecyclingGridItem* cellForRow(RecyclingView* recycler, size_t index) override {
        auto* cell = dynamic_cast<Cell*>(recycler->dequeueReusableCell(identifier));
        const Item& item = items.at(index);
        configureCell(cell, item);
        cell->upTarget = upTarget;
        cell->downTarget = downTarget;
        const Focus focused = onFocused;
        cell->onFocused = [item, focused]() {
            if (focused) focused(item);
        };
        return cell;
    }

    void onItemSelected(brls::Box*, size_t index) override {
        if (onSelected && index < items.size()) onSelected(items.at(index));
    }

    void clearData() override { items.clear(); }

    /// Adds items that are not shown yet; returns how many were added.
    size_t append(std::vector<Item> incoming) {
        size_t added = 0;
        for (auto& item : incoming) {
            if (find(keyOf(item)) < items.size()) continue;
            items.push_back(std::move(item));
            ++added;
        }
        return added;
    }

    size_t find(const std::string& key) const {
        for (size_t index = 0; index < items.size(); ++index) {
            if (keyOf(items[index]) == key) return index;
        }
        return items.size();
    }

    const std::vector<Item>& data() const { return items; }

private:
    std::string identifier;
    std::vector<Item> items;
    Configure configureCell;
    Key keyOf;
    Selection onSelected;
    Focus onFocused;
    FocusTarget upTarget;
    FocusTarget downTarget;
};

using VideoDataSource = CardDataSource<rumble::Video, VideoCell>;
using ChannelDataSource = CardDataSource<rumble::Channel, ChannelCell>;
using CategoryDataSource = CardDataSource<rumble::Category, CategoryCell>;

/// Focus target inside a recycler row, or nullptr when it has no cells.
brls::View* recyclerFocus(brls::View* recycler);

}  // namespace rumblenx::cards
