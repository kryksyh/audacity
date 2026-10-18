/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QJSValue>

#include "api/apiobject.h"
#include "context/iglobalcontext.h"
#include "modularity/ioc.h"
#include "playback/iplaybackcontroller.h"
#include "record/irecordcontroller.h"
#include "audio/iaudioengine.h"
#include "trackedit/dom/clip.h"
#include "trackedit/iselectioncontroller.h"
#include "trackedit/iprojecthistory.h"
#include "trackedit/itrackeditproject.h"

class AudacityProject;

namespace au::trackedit::api {
class ProjectApi : public muse::api::ApiObject
{
    Q_OBJECT
    Q_PROPERTY(QObject * selection READ selection)
    Q_PROPERTY(double defaultSampleRate READ defaultSampleRate)

    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<ISelectionController> selectionController = { this };
    muse::ContextInject<IProjectHistory> projectHistory = { this };
    muse::ContextInject<record::IRecordController> recordController = { this };
    muse::ContextInject<playback::IPlaybackController> playbackController = { this };
    muse::GlobalInject<audio::IAudioEngine> audioEngine;

public:
    explicit ProjectApi(muse::api::IApiEngine* e);

    QObject* selection();
    double defaultSampleRate() const;

    // Tracks
    Q_INVOKABLE int trackCount() const;
    Q_INVOKABLE QJSValue trackInfo(int trackIndex) const;
    // Stored waveform samples, without inserting effects or routing. Bounded
    // to one minute so scripts cannot accidentally scan an entire long project.
    // Includes a hash of every retrieved float sample for exact PCM comparisons.
    Q_INVOKABLE QJSValue waveformStatistics(int trackIndex, double start, double end) const;

    // Clips
    Q_INVOKABLE int clipCount(int trackIndex) const;
    Q_INVOKABLE QJSValue clipsOnTrack(int trackIndex) const;

    // Project
    Q_INVOKABLE double totalTime() const;
    Q_INVOKABLE QJSValue fileInfo() const;
    // Read only: do not consume the player's single-reader callback queue or
    // update its clock. Transport changes still go through playback actions.
    Q_INVOKABLE QJSValue playbackInfo() const;
    Q_INVOKABLE QJSValue musicalContext() const;
    Q_INVOKABLE QObject* beginEdit(const QString& name, QObject* session = nullptr);

private:
    AudacityProject* project() const;
    ITrackeditProjectPtr trackeditProject() const;
    Clips sortedClips(int trackIndex) const;
};
}
