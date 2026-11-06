/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QObject>
#include <QVariantList>
#include <qqmlintegration.h>

#include "framework/global/modularity/ioc.h"
#include "audio/driver/iaudiodrivercontroller.h"
#include "audio/driver/ilatencymeasurement.h"

namespace au::appshell {
class LatencyMeasurementModel : public QObject, public muse::Contextable
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(QVariantList settings READ settings NOTIFY changed)
    Q_PROPERTY(QString resultText READ resultText NOTIFY changed)
    Q_PROPERTY(QString reportedText READ reportedText NOTIFY changed)
    Q_PROPERTY(QString detailsText READ detailsText NOTIFY changed)
    Q_PROPERTY(QString warningText READ warningText NOTIFY changed)
    Q_PROPERTY(int signalsPlayed READ signalsPlayed NOTIFY progressChanged)

    muse::GlobalInject<audio::ILatencyMeasurement> latencyMeasurement;
    muse::GlobalInject<audio::IAudioDriverController> audioDriverController;

public:
    enum class State {
        Idle, Measuring, Measured, Failed
    };
    Q_ENUM(State)

    explicit LatencyMeasurementModel(QObject* parent = nullptr);
    ~LatencyMeasurementModel() override;

    Q_INVOKABLE void start();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reset();
    Q_INVOKABLE void pollProgress();
    Q_INVOKABLE bool apply();

    State state() const { return m_state; }
    QVariantList settings() const;
    QString resultText() const;
    QString reportedText() const;
    QString detailsText() const;
    QString warningText() const;
    int signalsPlayed() const { return m_signalsPlayed; }

signals:
    void changed();
    void progressChanged();

private:
    State m_state = State::Idle;
    audio::LatencyMeasurement m_result;
    bool m_ownsMeasurement = false;
    int m_signalsPlayed = 0;
    QString m_applyError;
};
}
