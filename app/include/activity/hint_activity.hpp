//
// Created by fang on 2022/8/21.
//

#pragma once

#include <borealis.hpp>

/// Shown instead of Home in applet mode: how to launch RumbleNX through a
/// game so it gets the full memory it needs.
class HintActivity : public brls::Activity {
public:
    // Declare that the content of this activity is the given XML file
    CONTENT_FROM_XML_RES("activity/hint.xml");

    HintActivity();

    void onContentAvailable() override;

    ~HintActivity();

private:
    BRLS_BIND(brls::Box, steps, "hint/steps");
    BRLS_BIND(brls::Label, reason, "hint/reason");
    BRLS_BIND(brls::Label, forwarder, "hint/forwarder");
    BRLS_BIND(brls::Label, step1, "hint/step1");
    BRLS_BIND(brls::Label, step2, "hint/step2");
    BRLS_BIND(brls::Label, step3, "hint/step3");
};
