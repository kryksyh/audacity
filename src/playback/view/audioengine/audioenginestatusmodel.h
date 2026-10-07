/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

#include "framework/global/modularity/ioc.h"
#include "framework/interactive/iinteractive.h"

#include "audio/iaudioenginediagnostics.h"

namespace au::playback {
//! Polls the audio engine health for the status bar and the details dialog
class AudioEngineStatusModel : public QObject, public muse::Contextable
{
    Q_OBJECT

    Q_PROPERTY(bool streamActive READ streamActive NOTIFY statusChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY statusChanged)
    Q_PROPERTY(int loadPercent READ loadPercent NOTIFY statusChanged)
    Q_PROPERTY(int peakLoadPercent READ peakLoadPercent NOTIFY statusChanged)
    Q_PROPERTY(int dropouts READ dropouts NOTIFY statusChanged)
    Q_PROPERTY(bool recentDropout READ recentDropout NOTIFY statusChanged)
    Q_PROPERTY(QVariantList details READ details NOTIFY statusChanged)

    muse::GlobalInject<audio::IAudioEngineDiagnostics> engineDiagnostics;
    muse::ContextInject<muse::IInteractive> interactive { this };

public:
    explicit AudioEngineStatusModel(QObject* parent = nullptr);

    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void openDetails();
    Q_INVOKABLE void resetCounters();
    Q_INVOKABLE void copyDetails();

    bool streamActive() const;
    QString summary() const;
    int loadPercent() const;
    int peakLoadPercent() const;
    int dropouts() const;
    bool recentDropout() const;
    QVariantList details() const;

signals:
    void statusChanged();

private:
    void poll();

    QTimer m_timer;
    audio::AudioEngineDiagnostics m_current;
    bool m_polled = false;
    int m_pollsSinceDropout = -1;
};
}
