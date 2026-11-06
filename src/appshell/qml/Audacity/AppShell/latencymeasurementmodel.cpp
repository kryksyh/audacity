/*
 * Audacity: A Digital Audio Editor
 */
#include "latencymeasurementmodel.h"

#include <QPointer>

#include "framework/global/translation.h"

using namespace au::appshell;
using Status = au::audio::LatencyMeasurement::Status;

namespace {
QString milliseconds(double value)
{
    return muse::qtrc("latency", "%1 ms").arg(value, 0, 'f', 1);
}
}

LatencyMeasurementModel::LatencyMeasurementModel(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

LatencyMeasurementModel::~LatencyMeasurementModel()
{
    cancel();
}

void LatencyMeasurementModel::start()
{
    if (m_state == State::Measuring) {
        return;
    }
    m_state = State::Measuring;
    m_result = {};
    m_applyError.clear();
    m_signalsPlayed = 0;
    m_ownsMeasurement = true;
    emit changed();
    emit progressChanged();

    QPointer<LatencyMeasurementModel> self(this);
    latencyMeasurement()->measure(iocContext(), [self](const audio::LatencyMeasurement& result) {
        if (!self) {
            return;
        }
        self->m_ownsMeasurement = false;
        self->m_result = result;
        self->m_state = result.status == Status::Measured ? State::Measured
                        : result.status == Status::Cancelled ? State::Idle : State::Failed;
        emit self->changed();
    });
}

void LatencyMeasurementModel::cancel()
{
    if (m_ownsMeasurement) {
        latencyMeasurement()->cancel();
    }
}

void LatencyMeasurementModel::reset()
{
    if (m_state == State::Measuring) {
        return;
    }
    m_state = State::Idle;
    m_result = {};
    m_applyError.clear();
    emit changed();
}

void LatencyMeasurementModel::pollProgress()
{
    const int progress = latencyMeasurement()->signalsPlayed();
    if (m_signalsPlayed != progress) {
        m_signalsPlayed = progress;
        emit progressChanged();
    }
}

bool LatencyMeasurementModel::apply()
{
    if (m_state != State::Measured) {
        return false;
    }
    audio::AudioConfigurationChange change;
    change.automaticLatencyCompensation = false;
    change.latencyCompensation = -m_result.roundTripMs();
    const auto result = audioDriverController()->apply(iocContext(), change);
    if (!result.succeeded()) {
        m_applyError = muse::qtrc("latency", "Could not apply latency compensation. Try again after playback and recording stop.");
        emit changed();
        return false;
    }
    return true;
}

QVariantList LatencyMeasurementModel::settings() const
{
    const auto configuration = audioDriverController()->configuration();
    const auto output = configuration.outputDevice.value_or(audioDriverController()->systemDefaultOutputDevice(configuration.api));
    const auto input = configuration.inputDevice.value_or(audioDriverController()->systemDefaultInputDevice(configuration.api));
    auto row = [](const QString& title, const QString& value) {
        return QVariantMap { { "title", title }, { "value", value } };
    };
    const QString compensation = configuration.automaticLatencyCompensation
                                 ? muse::qtrc("latency", "Automatic") : milliseconds(configuration.latencyCompensation);
    return {
        row(muse::qtrc("latency", "Host"), QString::fromStdString(configuration.api)),
        row(muse::qtrc("latency", "Playback device"), QString::fromStdString(output)),
        row(muse::qtrc("latency", "Recording device"), QString::fromStdString(input)),
        row(muse::qtrc("latency", "Recording channels"), QString::number(configuration.inputChannels)),
        row(muse::qtrc("latency", "Buffer length"), milliseconds(configuration.bufferLength)),
        row(muse::qtrc("latency", "Latency compensation"), compensation),
    };
}

QString LatencyMeasurementModel::resultText() const
{
    if (m_state != State::Measured) {
        return {};
    }
    return muse::qtrc("latency", "%1 (%2 samples at %3 Hz)")
           .arg(milliseconds(m_result.roundTripMs())).arg(m_result.roundTripFrames).arg(m_result.sampleRate, 0, 'f', 0);
}

QString LatencyMeasurementModel::reportedText() const
{
    if (m_state != State::Measured) {
        return {};
    }
    return muse::qtrc("latency", "%1 (input %2 + output %3)")
           .arg(milliseconds(m_result.reportedInputLatencyMs + m_result.reportedOutputLatencyMs))
           .arg(milliseconds(m_result.reportedInputLatencyMs)).arg(milliseconds(m_result.reportedOutputLatencyMs));
}

QString LatencyMeasurementModel::detailsText() const
{
    if (m_result.signalsSent == 0) {
        return {};
    }
    return muse::qtrc("latency", "%1 of %2 test signals detected.").arg(m_result.signalsFound).arg(m_result.signalsSent);
}

QString LatencyMeasurementModel::warningText() const
{
    if (!m_applyError.isEmpty()) {
        return m_applyError;
    }
    if (m_state == State::Failed) {
        switch (m_result.status) {
        case Status::Busy:
            return muse::qtrc("latency", "Recording or another audio operation is in progress. Stop it, then try again.");
        case Status::DeviceError:
            return muse::qtrc("latency", "Could not open the audio devices. Check the selected devices and try again.");
        case Status::NoSignal:
            return muse::qtrc("latency",
                              "Too few test signals were detected. Check the cable, or move the microphone closer to the speaker.");
        case Status::Measured:
        case Status::Cancelled:
            return {};
        }
    }
    if (m_state != State::Measured) {
        return {};
    }
    QStringList warnings;
    if (m_result.inputPeak >= 0.99f) {
        warnings << muse::qtrc("latency", "The input signal clipped. Lower the input level and repeat the test.");
    }
    const double spreadMs = 1000.0 * m_result.spreadFrames / m_result.sampleRate;
    if (spreadMs > 1.0) {
        warnings << muse::qtrc("latency",
                               "The result changed by %1 between test signals. The input and output devices might use different clocks.")
            .arg(milliseconds(spreadMs));
    }
    if (m_result.signalsFound < m_result.signalsSent) {
        warnings << muse::qtrc("latency", "Some test signals were not detected. Check the connection and reduce background noise.");
    }
    return warnings.join('\n');
}
