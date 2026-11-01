#pragma once

#include <borealis.hpp>

namespace rumblenx {

class RumbleLoadingView : public brls::View {
public:
    RumbleLoadingView();
    ~RumbleLoadingView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height, brls::Style style, brls::FrameContext* ctx) override;

    static brls::View* create();

private:
    brls::Image* logoImage = nullptr;
    brls::Image* shineImage = nullptr;
};

} // namespace rumblenx
