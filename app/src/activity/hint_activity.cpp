//
// Created by fang on 2022/8/21.
//

#include "activity/hint_activity.hpp"

#ifdef BUILTIN_NSP
#include <nspmini.hpp>
#endif

using namespace brls::literals;

HintActivity::HintActivity() {
    brls::Logger::debug("HintActivityActivity: create");
}

void HintActivity::onContentAvailable() {
    brls::Logger::debug("HintActivityActivity: onContentAvailable");

    step1->setText("1.  " + "main/hints/hint1"_i18n);
    step2->setText("2.  " + "main/hints/hint2"_i18n);
    step3->setText("3.  " + "main/hints/hint3"_i18n);

#ifdef BUILTIN_NSP
    auto* install = new brls::Label();
    install->setFontSize(24);
    install->setMarginTop(20);
    install->setFocusable(true);
    install->setText("main/hints/hint_nsp"_i18n);
    install->registerClickAction([](...) -> bool {
        auto dialog = new brls::Dialog("main/hints/hint_confirm"_i18n);
        dialog->addButton("hints/cancel"_i18n, []() {});
        dialog->addButton("hints/ok"_i18n, []() {
            brls::Application::blockInputs();

            mini::InstallSD("romfs:/forwarder.nsp");
            unsigned long long AppTitleID = mini::GetTitleID();
            appletRequestLaunchApplication(AppTitleID, NULL);
        });
        dialog->open();
        return true;
    });
    install->addGestureRecognizer(new brls::TapGestureRecognizer(install));
    steps->addView(install);
#endif
}

HintActivity::~HintActivity() {
    brls::Logger::debug("HintActivityActivity: delete");
}
