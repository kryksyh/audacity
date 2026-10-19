/*
* Audacity: A Digital Audio Editor
*/
#pragma once
#include <QObject>
#include <QVariantList>
#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "audio/iaudioengine.h"
#include "context/iglobalcontext.h"
#include "trackedit/iprojecthistory.h"
#include "trackedit/itracksviewrequestsservice.h"

class PlayableTrack;
class TrackRouting;
namespace au::trackedit {
class TrackRoutingModel : public QObject, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    Q_PROPERTY(qlonglong trackId READ trackId WRITE setTrackId NOTIFY dataChanged)
    Q_PROPERTY(QString title READ title NOTIFY dataChanged)
    Q_PROPERTY(QVariantList buses READ buses NOTIFY dataChanged)
    Q_PROPERTY(QVariantList sends READ sends NOTIFY dataChanged)
    Q_PROPERTY(int outputIndex READ outputIndex NOTIFY dataChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY dataChanged)
    Q_PROPERTY(QString error READ error NOTIFY dataChanged)

    muse::GlobalInject<audio::IAudioEngine> audioEngine;

    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<IProjectHistory> history { this };
    muse::ContextInject<ITracksViewRequestsService> viewRequests { this };

public:
    explicit TrackRoutingModel(QObject* parent = nullptr);
    TrackRoutingModel(const muse::modularity::ContextPtr& ctx, QObject* parent = nullptr);
    Q_INVOKABLE void init();
    Q_INVOKABLE void initPanel();
    Q_INVOKABLE void openEditor();
    qlonglong trackId() const { return m_trackId; }
    void setTrackId(qlonglong id);
    QString title() const;
    QVariantList buses() const;
    QVariantList sends() const;
    int outputIndex() const;
    bool editable() const;
    QString error() const { return m_error; }
    Q_INVOKABLE void setOutput(int index);
    Q_INVOKABLE void setSend(int busIndex, bool enabled, double gain, bool preFader);
    Q_INVOKABLE void addBus();

signals:
    void editorRequested();
    void dataChanged();

private:
    PlayableTrack* track() const;
    bool prepareEdit();
    void apply(TrackRouting routing);
    qlonglong m_trackId = -1;
    bool m_initialised = false;
    QString m_error;
};
}
