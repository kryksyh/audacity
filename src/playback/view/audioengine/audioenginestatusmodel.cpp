/*
 * Audacity: A Digital Audio Editor
 */
#include "audioenginestatusmodel.h"

#include <cmath>

#include <QClipboard>
#include <QGuiApplication>
#include <QVariantMap>

#include "framework/global/translation.h"

using namespace au::playback;

namespace {
constexpr int POLL_INTERVAL_MS = 250;
constexpr int RECENT_DROPOUT_POLLS = 12;

int percent(float share)
{
    return static_cast<int>(std::lround(share * 100.0f));
}

QString milliseconds(double ms)
{
    return muse::qtrc("playback", "%1 ms").arg(ms, 0, 'f', 1);
}

QVariantMap row(const QString& label, const QString& value)
{
    return { { "label", label }, { "value", value } };
}
}

AudioEngineStatusModel::AudioEngineStatusModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
    m_timer.setInterval(POLL_INTERVAL_MS);
    connect(&m_timer, &QTimer::timeout, this, &AudioEngineStatusModel::poll);
}

void AudioEngineStatusModel::start()
{
    if (!engineDiagnostics()) {
        return;
    }
    poll();
    m_timer.start();
}

void AudioEngineStatusModel::stop()
{
    m_timer.stop();
}

void AudioEngineStatusModel::openDetails()
{
    interactive()->open("audacity://playback/audio_engine_status");
}

void AudioEngineStatusModel::resetCounters()
{
    engineDiagnostics()->resetCounters();
    m_pollsSinceDropout = -1;
    poll();
}

void AudioEngineStatusModel::copyDetails()
{
    QStringList lines;
    for (const QVariant& item : details()) {
        const QVariantMap map = item.toMap();
        lines << map.value("label").toString() + ": " + map.value("value").toString();
    }
    QGuiApplication::clipboard()->setText(lines.join('\n'));
}

void AudioEngineStatusModel::poll()
{
    const uint64_t dropoutsBefore = m_current.dropouts;
    m_current = engineDiagnostics()->diagnostics();

    if (m_polled && m_current.dropouts > dropoutsBefore) {
        m_pollsSinceDropout = 0;
    } else if (m_pollsSinceDropout >= 0) {
        ++m_pollsSinceDropout;
    }
    m_polled = true;

    emit statusChanged();
}

bool AudioEngineStatusModel::streamActive() const
{
    return m_current.streamActive;
}

QString AudioEngineStatusModel::summary() const
{
    if (!m_current.streamActive || m_current.framesPerBuffer == 0) {
        return muse::qtrc("playback", "Audio idle");
    }
    //: Sample rate, audio buffer size and output latency, e.g. "48 kHz · 256 frames · 5.3 ms"
    return muse::qtrc("playback", "%1 kHz · %2 frames · %3")
           .arg(m_current.sampleRate / 1000.0, 0, 'g', 3)
           .arg(m_current.framesPerBuffer)
           .arg(milliseconds(m_current.reportedOutputLatencyMs));
}

int AudioEngineStatusModel::loadPercent() const
{
    return percent(m_current.averageLoad);
}

int AudioEngineStatusModel::peakLoadPercent() const
{
    return percent(m_current.peakLoad);
}

int AudioEngineStatusModel::dropouts() const
{
    return static_cast<int>(m_current.dropouts);
}

bool AudioEngineStatusModel::recentDropout() const
{
    return m_pollsSinceDropout >= 0 && m_pollsSinceDropout < RECENT_DROPOUT_POLLS;
}

QVariantList AudioEngineStatusModel::details() const
{
    const audio::AudioEngineDiagnostics& c = m_current;
    const double bufferMs = c.sampleRate > 0 ? 1000.0 * c.framesPerBuffer / c.sampleRate : 0.0;

    QVariantList rows;
    rows << row(muse::qtrc("playback", "Status"),
                c.streamActive ? muse::qtrc("playback", "Running") : muse::qtrc("playback", "Idle"));
    rows << row(muse::qtrc("playback", "Sample rate"), muse::qtrc("playback", "%1 Hz").arg(c.sampleRate, 0, 'f', 0));
    rows << row(muse::qtrc("playback", "Buffer"),
                muse::qtrc("playback", "%1 frames (%2)").arg(c.framesPerBuffer).arg(milliseconds(bufferMs)));
    rows << row(muse::qtrc("playback", "Input latency (reported by driver)"), milliseconds(c.reportedInputLatencyMs));
    rows << row(muse::qtrc("playback", "Output latency (reported by driver)"), milliseconds(c.reportedOutputLatencyMs));
    rows << row(muse::qtrc("playback", "Callback load"),
                muse::qtrc("playback", "%1 % average, %2 % peak").arg(loadPercent()).arg(peakLoadPercent()));
    rows << row(muse::qtrc("playback", "Callbacks"), QString::number(c.callbacks));
    rows << row(muse::qtrc("playback", "Dropouts"), QString::number(c.dropouts));
    rows << row(muse::qtrc("playback", "Callback over time budget"), QString::number(c.overBudgetCallbacks));
    rows << row(muse::qtrc("playback", "Output underflow (driver)"), QString::number(c.outputUnderflows));
    rows << row(muse::qtrc("playback", "Input overflow (driver)"), QString::number(c.inputOverflows));
    rows << row(muse::qtrc("playback", "Playback buffer ran dry"), QString::number(c.playbackStarvations));
    rows << row(muse::qtrc("playback", "Lost recorded samples"), QString::number(c.lostCaptureFrames));
    return rows;
}
