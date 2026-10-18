/*
* Audacity: A Digital Audio Editor
*/
#include <gtest/gtest.h>

#include "../effectscommands.h"

using namespace au::effects;
using namespace muse::rcommand;

TEST(EffectsBase_EffectsCommandsTests, EffectIdWithUriDelimiters_SurvivesCommandStringRoundTrip)
{
    const EffectId id = EffectId::fromStdString("Effect_VST3_Vendor_Name_C:\\Plug-ins/what?.vst3;100% #done");

    const Command openCommand = makeEffectOpenCommand(id);
    const Command applyCommand = makeEffectApplyCommand(id);

    EXPECT_EQ(effectIdFromCommand(Command(openCommand.toString())), id);
    EXPECT_EQ(effectIdFromCommand(Command(applyCommand.toString())), id);
}

TEST(EffectsBase_EffectsCommandsTests, EffectId_IsASinglePathSegment)
{
    const EffectId id = EffectId::fromStdString("Effect_Nyquist_Audacity_ShelfFilter_/usr/share/audacity/ShelfFilter.ny");

    const std::vector<std::string> segments = makeEffectOpenCommand(id).pathSegments();

    ASSERT_EQ(segments.size(), 3u);
    EXPECT_EQ(segments[0], "effects");
    EXPECT_EQ(segments[1], "open");
    EXPECT_EQ(decodeEffectId(segments[2]), id);
}

TEST(EffectsBase_EffectsCommandsTests, EffectIdFromCommand_IsEmptyForNonEffectCommands)
{
    EXPECT_TRUE(effectIdFromCommand(EFFECTS_REPEAT_LAST_EFFECT_COMMAND).empty());
    EXPECT_TRUE(effectIdFromCommand(EFFECTS_PRESET_APPLY_COMMAND).empty());
}

TEST(EffectsBase_EffectsCommandsTests, MenuActionsKeepAudioUnitManufacturerCodesAndQuotedPluginPathsIntact)
{
    const std::vector<EffectId> ids {
        EffectId::fromStdString("Effect_AudioUnit_MeldaProduction_MFlanger_Meld/aumf/MAE&/MeldaProduction: MFlanger"),
        EffectId::fromQString(QString::fromUtf8("Effect_VST3_Mike's synth & 演奏/100%20 + #?.vst3"))
    };
    for (const auto& id : ids) {
        for (const auto& action : { EFFECT_OPEN_ACTION, REALTIME_EFFECT_ADD_ACTION, REALTIME_EFFECT_REPLACE_ACTION }) {
            const muse::actions::ActionQuery query(makeEffectAction(action, id));
            ASSERT_TRUE(query.isValid());
            EXPECT_EQ(query.params().size(), 2u); // No part of the ID became another query parameter.
            EXPECT_EQ(effectIdFromAction(query), id);
            EXPECT_EQ(effectIdFromCommand(Command(makeEffectOpenCommand(effectIdFromAction(query)).toString())), id);
        }
    }
}

TEST(EffectsBase_EffectsCommandsTests, ExistingMenuActionsAndLiteralPercentSequencesRemainUnchanged)
{
    const auto id = EffectId::fromStdString("Effect_VST3_Name_/Synth100%20.vst3");
    const auto oldAction = QString::fromStdString(EFFECT_OPEN_ACTION).arg(id).toStdString();
    EXPECT_EQ(makeEffectAction(EFFECT_OPEN_ACTION, id), oldAction);
    const muse::actions::ActionQuery query(oldAction);
    EXPECT_EQ(query.params().size(), 1u);
    EXPECT_EQ(effectIdFromAction(query), id);
}
