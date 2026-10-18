/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>
#include <QVariantList>
#include <cstdint>
#include <wx/string.h>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "audio/iaudioengine.h"
#include "audio/ipluginautomation.h"
#include "trackedit/iprojecthistory.h"
#include "trackedit/itracksviewrequestsservice.h"
#include "au3-automation-data/AutomationData.h"

class PlayableTrack;
class RealtimeEffectState;

namespace au::automation {
class TrackAutomationModel : public QObject, public muse::async::Asyncable, public muse::Contextable
{
    Q_OBJECT
    Q_PROPERTY(qlonglong trackId READ trackId WRITE setTrackId NOTIFY dataChanged)
    Q_PROPERTY(QString title READ title NOTIFY dataChanged)
    Q_PROPERTY(QVariantList targets READ targets NOTIFY dataChanged)
    Q_PROPERTY(int targetIndex READ targetIndex WRITE setTargetIndex NOTIFY parametersChanged)
    Q_PROPERTY(QVariantList parameters READ parameters NOTIFY parametersChanged)
    Q_PROPERTY(int parameterIndex READ parameterIndex WRITE setParameterIndex NOTIFY dataChanged)
    Q_PROPERTY(QVariantList points READ points NOTIFY dataChanged)
    Q_PROPERTY(bool laneEnabled READ laneEnabled WRITE setLaneEnabled NOTIFY dataChanged)
    Q_PROPERTY(double duration READ duration NOTIFY dataChanged)
    Q_PROPERTY(double defaultValue READ defaultValue NOTIFY dataChanged)
    Q_PROPERTY(double valueScale READ valueScale NOTIFY dataChanged)
    Q_PROPERTY(bool editable READ editable NOTIFY dataChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)

    muse::GlobalInject<audio::IAudioEngine> audioEngine;
    muse::GlobalInject<audio::IInstrumentEditorFactory> instrumentFactory;
    muse::GlobalInject<audio::IInstrumentPlayback> instrumentPlayback;
    muse::GlobalInject<audio::IPluginAutomation> pluginAutomation;

    muse::ContextInject<context::IGlobalContext> globalContext { this };
    muse::ContextInject<trackedit::IProjectHistory> history { this };
    muse::ContextInject<trackedit::ITracksViewRequestsService> viewRequests { this };

public:
    explicit TrackAutomationModel(QObject* parent = nullptr);
    TrackAutomationModel(const muse::modularity::ContextPtr& ctx, QObject* parent = nullptr);
    Q_INVOKABLE void init();
    Q_INVOKABLE void initPanel();
    Q_INVOKABLE void openEditor();
    qlonglong trackId() const { return m_trackId; }
    void setTrackId(qlonglong id);
    QString title() const;
    QVariantList targets() const;
    int targetIndex() const { return m_targetIndex; }
    void setTargetIndex(int index);
    QVariantList parameters() const { return m_parameters; }
    int parameterIndex() const { return m_parameterIndex; }
    void setParameterIndex(int index);
    QVariantList points() const;
    double duration() const;
    double defaultValue() const;
    double valueScale() const;
    bool laneEnabled() const;
    void setLaneEnabled(bool enabled);
    bool editable() const;
    QString error() const { return m_error; }
    Q_INVOKABLE void refreshParameters();
    Q_INVOKABLE void setPoint(int index, double time, double value, double curve = 0.0);
    Q_INVOKABLE void removePoint(int index);
    Q_INVOKABLE void clearLane();

signals:
    void editorRequested();
    void dataChanged();
    void parametersChanged();
    void errorChanged();

private:
    PlayableTrack* track() const;
    std::shared_ptr<RealtimeEffectState> effect() const;
    AutomationData* automation() const;
    std::string parameterId() const;
    bool prepareEdit();
    void changed();
    void reload();
    void setError(const QString& error);

    qlonglong m_trackId = -1;
    int m_targetIndex = 0;
    int m_parameterIndex = 0;
    bool m_initialised = false;
    QVariantList m_parameters;
    QString m_parameterOwner;
    wxString m_instrumentState;
    std::weak_ptr<RealtimeEffectState> m_parameterEffect;
    uint64_t m_effectSettingsCounter = 0;
    QString m_error;
};
}
