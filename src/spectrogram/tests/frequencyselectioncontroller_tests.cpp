/*
 * Audacity: A Digital Audio Editor
 */
#include <gtest/gtest.h>

#include "spectrogram/internal/frequencyselectioncontroller.h"
#include "spectrogram/internal/ifrequencyselectionrestorer.h"

namespace au::spectrogram {
namespace {
class SelectionRestorer : public IFrequencySelectionRestorer
{
public:
    void storeFrequencySelectionState(const FrequencySelection&) const override { ++stores; }
    FrequencySelection loadFrequencySelectionState() const override { return {}; }
    mutable int stores = 0;
};
}

TEST(FrequencySelectionControllerTests, ClearingAnEmptySelectionDoesNotPublishChanges)
{
    auto restorer = std::make_unique<SelectionRestorer>();
    const auto* stored = restorer.get();
    FrequencySelectionController controller(muse::modularity::globalCtx(), std::move(restorer));
    controller.init();
    muse::async::Asyncable receiver;
    int changes = 0;
    controller.frequencySelectionChanged().onReceive(&receiver, [&](bool) { ++changes; });
    controller.resetFrequencySelection();
    controller.resetFrequencySelection();
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(stored->stores, 0);
    EXPECT_EQ(controller.frequencySelection(), FrequencySelection {});
}
} // namespace au::spectrogram
