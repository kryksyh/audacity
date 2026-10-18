/*
* Audacity: A Digital Audio Editor
*/
#include <QApplication>
#include <gmock/gmock.h>
#include "global/runtime.h"
#include "testing/environment.h"

int main(int argc, char** argv)
{
    QCoreApplication::setOrganizationName("MuseScore");
    QCoreApplication::setOrganizationDomain("musescore.org");
    // Match Audacity: native instrument editors may contain Qt widgets.
    QApplication application(argc, argv);
    qputenv("QML_DISABLE_DISK_CACHE", "true");
    muse::runtime::mainThreadId();
    muse::runtime::setThreadName("main");
    muse::testing::Environment::setup();
    testing::InitGoogleMock(&argc, argv);
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    const int result = RUN_ALL_TESTS();
    muse::testing::Environment::deinit();
    return result;
}
