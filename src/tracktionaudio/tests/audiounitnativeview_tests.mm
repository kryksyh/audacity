/*
 * Audacity: A Digital Audio Editor
 */
#include <tracktion_engine/tracktion_engine.h>

#include <gtest/gtest.h>
#include <QTemporaryDir>

#import <Cocoa/Cocoa.h>
#import <AudioUnit/AUCocoaUIView.h>

#include "../internal/tracktionruntime.h"
#include "../internal/tracktionpluginloader.h"
#include "au3-audio-unit/AudioUnitUtils.h"

namespace au::tracktionaudio {
namespace {
NSView* createNativeView(AudioUnit unit)
{
    @autoreleasepool {
        PackedArray::Ptr<AudioUnitCocoaViewInfo> info;
        const auto status = AudioUnitUtils::GetVariableSizeProperty(unit, kAudioUnitProperty_CocoaUI, info);
        if (status != noErr || !info) {
            return nil;
        }
        NSBundle* bundle = [NSBundle bundleWithPath:[static_cast<NSURL*>(info->mCocoaAUViewBundleLocation) path]];
        Class factoryClass = [bundle classNamed:static_cast<NSString*>(info->mCocoaAUViewClass[0])];
        id<AUCocoaUIBase> factory = [[[factoryClass alloc] init] autorelease];
        return [[factory uiViewForAudioUnit:unit withSize:NSMakeSize(800, 600)] retain];
    }
}
}

TEST(AudioUnitNativeViewTests, InstalledEditorClosesBeforeProcessingAndDisposal)
{
    const auto identifier = qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER");
    if (identifier.isEmpty()) {
        GTEST_SKIP() << "Set AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER to test an installed Audio Unit.";
    }
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    TracktionRuntime runtime;
    runtime.init(directory.filePath("cache").toStdString(), directory.filePath("settings").toStdString());
    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = identifier.toStdString();
    for (int iteration = 0; iteration < 3; ++iteration) {
        SCOPED_TRACE(iteration);
        juce::String error;
        auto editor = loadTracktionPlugin(runtime.engine().getPluginManager().pluginFormatManager, description, error);
        ASSERT_TRUE(editor) << error.toStdString();
        auto* client = editor->getAudioUnitClient();
        ASSERT_TRUE(client);
        ASSERT_TRUE(client->prepareForEditor());
        NSView* view = createNativeView(client->getAudioUnitHandle());
        ASSERT_NE(view, nil);
        [view removeFromSuperview];
        [view release];
        client->editorClosed();
        auto processing = loadTracktionPlugin(runtime.engine().getPluginManager().pluginFormatManager, description, error);
        ASSERT_TRUE(processing) << error.toStdString();
        processing->setRateAndBufferSizeDetails(44100.0, 512);
        processing->prepareToPlay(44100.0, 512);
        juce::AudioBuffer<float> buffer(std::max(processing->getTotalNumInputChannels(), processing->getTotalNumOutputChannels()), 512);
        buffer.clear();
        juce::MidiBuffer midi;
        processing->processBlock(buffer, midi);
        editor.reset();
        juce::AudioUnitPluginFormatHeadless::waitForPendingDisposals();
        processing->releaseResources();
        processing.reset();
        juce::AudioUnitPluginFormatHeadless::waitForPendingDisposals();
    }
}

TEST(AudioUnitNativeViewTests, InstalledEditorClosesWithDirectCoreAudioHosting)
{
    const auto identifier = qEnvironmentVariable("AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER");
    if (identifier.isEmpty()) {
        GTEST_SKIP() << "Set AU_TRACKTION_TEST_AUDIO_UNIT_IDENTIFIER to test an installed Audio Unit.";
    }
    const auto fields = identifier.section(':', -1).split(',');
    ASSERT_EQ(fields.size(), 3);
    const auto code = [](const QString& value) {
        OSType result = 0;
        for (const auto character : value.toLatin1()) {
            result = (result << 8) | static_cast<unsigned char>(character);
        }
        return result;
    };
    AudioComponentDescription description {};
    description.componentType = code(fields[0]);
    description.componentSubType = code(fields[1]);
    description.componentManufacturer = code(fields[2]);
    const auto component = AudioComponentFindNext(nullptr, &description);
    ASSERT_NE(component, nullptr);
    AudioUnit unit = nullptr;
    ASSERT_EQ(AudioComponentInstanceNew(component, &unit), noErr);
    ASSERT_EQ(AudioUnitInitialize(unit), noErr);
    NSView* view = createNativeView(unit);
    ASSERT_NE(view, nil);
    [view removeFromSuperview];
    [view release];
    ASSERT_EQ(AudioUnitUninitialize(unit), noErr);
    ASSERT_EQ(AudioComponentInstanceDispose(unit), noErr);
}
} // namespace au::tracktionaudio
