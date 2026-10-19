/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <memory>
#include <QObject>
#include <QTimer>
#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "audio/iaudioengine.h"
#include "context/iglobalcontext.h"
#include "trackedit/iprojecthistory.h"
#include "trackedit/itracksviewrequestsservice.h"

class PlayableTrack;
namespace au::trackedit {
class TrackFreezeModel : public QObject, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    Q_PROPERTY(qlonglong trackId READ trackId WRITE setTrackId NOTIFY dataChanged)
    Q_PROPERTY(bool frozen READ frozen NOTIFY dataChanged)
    Q_PROPERTY(bool canFreeze READ canFreeze NOTIFY dataChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY dataChanged)
    Q_PROPERTY(double progress READ progress NOTIFY dataChanged)
    Q_PROPERTY(QString error READ error NOTIFY dataChanged)

    muse::GlobalInject<audio::IAudioEngine> audioEngine;

    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<IProjectHistory> history { this };
    muse::ContextInject<ITracksViewRequestsService> viewRequests { this };

public:
    explicit TrackFreezeModel(QObject* parent = nullptr);
    TrackFreezeModel(const muse::modularity::ContextPtr& ctx, QObject* parent = nullptr);
    ~TrackFreezeModel() override;
    Q_INVOKABLE void init();
    qlonglong trackId() const { return m_trackId; }
    void setTrackId(qlonglong id);
    bool frozen() const;
    bool canFreeze() const;
    bool busy() const;
    double progress() const;
    QString error() const { return m_error; }
    Q_INVOKABLE void freeze(double tailSeconds = 2.0);
    Q_INVOKABLE void unfreeze();
    Q_INVOKABLE void cancel();

signals:
    void dataChanged();

private:
    PlayableTrack* track() const;
    bool prepareEdit();
    void finish();
    struct Job;
    std::unique_ptr<Job> m_job;
    QTimer m_timer;
    qlonglong m_trackId = -1;
    bool m_initialised = false;
    QString m_error;
};
}
