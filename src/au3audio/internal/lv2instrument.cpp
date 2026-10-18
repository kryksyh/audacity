/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentprocessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include "au3-lv2/LoadLV2.h"
#include "au3-lv2/LV2UIFeaturesList.h"
#include "au3-lv2/LV2Wrapper.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/midi/midi.h"
#include "lv2/time/time.h"

namespace au::au3audio {
namespace {
const LilvPlugin& loadPlugin(const std::string& path)
{
    if (!LV2Symbols::InitializeGWorld()) {
        throw std::runtime_error("Could not initialize LV2 discovery.");
    }
    LV2EffectsModule provider;
    const auto wxPath = wxString::FromUTF8(path);
    if (const auto* existing = provider.GetPlugin(wxPath)) {
        return *existing;
    }
    provider.LoadBundle(wxPath);
    const auto* plugin = provider.GetPlugin(wxPath);
    if (!plugin) {
        throw std::runtime_error("The LV2 instrument could not be found.");
    }
    return *plugin;
}

bool requiresFeature(const LilvPlugin& plugin, const char* uri)
{
    const LilvNodePtr node { lilv_new_uri(LV2Symbols::gWorld, uri) };
    return lilv_plugin_has_feature(&plugin, node.get());
}

// LV2's makePath is usable during instantiation and during playback. Each
// instance owns its scratch directory; snapshots archive every referenced file.
class InstrumentFeatures final : public LV2FeaturesList
{
public:
    InstrumentFeatures(const LilvPlugin& plugin)
        : LV2FeaturesList(plugin), fixed(requiresFeature(plugin, LV2_BUF_SIZE__fixedBlockLength)
                                         || requiresFeature(plugin, LV2_BUF_SIZE__powerOf2BlockLength))
    {
        if (!scratch.isValid()) {
            throw std::runtime_error("Could not create LV2 instrument storage.");
        }
        if (fixed) {
            AddFeature(LV2_BUF_SIZE__fixedBlockLength, nullptr);
        }
        AddFeature(LV2_BUF_SIZE__powerOf2BlockLength, nullptr);
        // Do not promise power-of-two blocks to an ordinary variable-block plugin.
        if (!fixed) {
            std::erase_if(mFeatures, [](const auto& feature) { return !std::strcmp(feature.URI, LV2_BUF_SIZE__powerOf2BlockLength); });
        }
        AddFeature(LV2_STATE__makePath, &makePathFeature);
        AddFeature(LV2_STATE__freePath, &freePathFeature);
    }

    LV2_URID_Unmap* unmap() const
    {
        for (const auto* feature : GetFeaturePointers()) {
            if (feature && !std::strcmp(feature->URI, LV2_URID__unmap)) {
                return static_cast<LV2_URID_Unmap*>(feature->data);
            }
        }
        return nullptr;
    }

    static char* makePath(LV2_State_Make_Path_Handle handle, const char* relative)
    {
        auto& self = *static_cast<InstrumentFeatures*>(handle);
        const auto name = QDir::cleanPath(QString::fromUtf8(relative));
        if (QDir::isAbsolutePath(name) || name == ".." || name.startsWith("../")) {
            return nullptr;
        }
        const auto result = QDir(self.scratch.path()).filePath(name);
        if (!QDir().mkpath(QFileInfo(result).absolutePath())) {
            return nullptr;
        }
        const auto bytes = QFile::encodeName(result);
        auto* copy = static_cast<char*>(std::malloc(bytes.size() + 1));
        if (copy) {
            std::memcpy(copy, bytes.constData(), bytes.size() + 1);
        }
        return copy;
    }

    static void freePath(LV2_State_Free_Path_Handle, char* path) { std::free(path); }

    const bool fixed;
    QTemporaryDir scratch;
    LV2_State_Make_Path makePathFeature { this, makePath };
    LV2_State_Free_Path freePathFeature { this, freePath };
};

class Lv2Instrument final : public InstrumentProcessor, private LV2UIFeaturesList::UIHandler
{
public:
    Lv2Instrument(const std::string& path, const std::string& state)
        : m_effect(loadPlugin(path)), m_features(m_effect.mPlug), m_options(m_features),
        m_settings(m_effect.MakeSettings()), m_outputs(m_effect.MakeOutputs()), m_ports(m_effect.mPorts),
        m_values(std::make_unique<std::atomic<double>[]>(m_ports.mControlPorts.size())),
        m_processingValues(std::make_unique<std::atomic<double>[]>(m_ports.mControlPorts.size())),
        m_outputValues(std::make_unique<std::atomic<float>[]>(m_ports.mControlPorts.size()))
    {
        if (!m_options.mOk || !m_features.ValidateFeatures(lilv_plugin_get_uri(&m_effect.mPlug))
            || !m_ports.mMidiIn || !m_ports.mAudioOut) {
            throw std::runtime_error("The LV2 instrument requires unsupported ports or features.");
        }
        lv2_atom_forge_init(&m_forge, m_features.URIDMapFeature());
        m_midi = m_features.URID_Map(LV2_MIDI__MidiEvent);
        m_position = m_features.URID_Map(LV2_TIME__Position);
        m_frame = m_features.URID_Map(LV2_TIME__frame);
        m_speed = m_features.URID_Map(LV2_TIME__speed);
        m_tempo = m_features.URID_Map(LV2_TIME__beatsPerMinute);
        m_beat = m_features.URID_Map(LV2_TIME__barBeat);
        m_bar = m_features.URID_Map(LV2_TIME__bar);
        m_beatsPerBar = m_features.URID_Map(LV2_TIME__beatsPerBar);
        m_beatUnit = m_features.URID_Map(LV2_TIME__beatUnit);
        const auto& values = GetSettings(m_settings).values;
        for (size_t i = 0; i < values.size(); ++i) {
            m_idStrings.push_back(m_ports.mControlPorts[i]->mSymbol.ToStdString());
            m_values[i].store(normalized(i, values[i] * (m_ports.mControlPorts[i]->mSampleRate ? m_rate : 1.0)));
            m_processingValues[i] = m_values[i].load();
            m_outputValues[i].store(0.0f);
        }
        instantiate(44100.0, 512, state);
        QObject::connect(&m_uiTimer, &QTimer::timeout, &m_uiTimer, [this] { idle(); });
    }

    ~Lv2Instrument() override { detachView(); }

    void prepare(double rate, size_t maximumFrames) override
    {
        if (rate == m_rate && maximumFrames == m_maximumFrames) {
            reset();
            return;
        }
        const auto saved = state();
        detachView();
        m_wrapper.reset();
        instantiate(rate, maximumFrames, saved);
    }

    void reset() override
    {
        m_reset = true;
        m_pendingFrames = 0;
        m_pendingEvents.clear();
        m_readFrame = m_blockSize;
    }

    size_t latency() const override
    {
        return m_latency.load(std::memory_order_acquire);
    }

    std::vector<audio::InstrumentParameter> parameters() const override
    {
        std::vector<audio::InstrumentParameter> result;
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            const auto& port = *m_ports.mControlPorts[i];
            if (!port.mIsInput) {
                continue;
            }
            const double value = m_values[i].load(std::memory_order_relaxed);
            const auto display = QString::number(plain(i, value), 'g', 6) + " " + QString::fromStdWString(port.mUnits.ToStdWstring());
            result.push_back({ port.mSymbol.ToStdString(), port.mName.ToStdString(), display.toStdString(), value });
        }
        return result;
    }

    bool setParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value)) {
            return false;
        }
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            if (m_ports.mControlPorts[i]->mIsInput && m_idStrings[i] == id) {
                m_values[i].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
                m_processingValues[i].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
                m_changed.store(true);
                return true;
            }
        }
        return false;
    }

    bool setAutomatedParameter(const std::string& id, double value) override
    {
        if (!std::isfinite(value)) {
            return false;
        }
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            if (m_ports.mControlPorts[i]->mIsInput && m_idStrings[i] == id) {
                m_processingValues[i].store(std::clamp(value, 0.0, 1.0), std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }

    bool takeChanges() override { return m_changed.exchange(false); }
    void restoreManualParameters() noexcept override
    {
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            m_processingValues[i] = m_values[i].load();
        }
    }

    bool restoreManualParameter(const std::string& id) noexcept override
    {
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            if (m_ports.mControlPorts[i]->mIsInput && m_idStrings[i] == id) {
                m_processingValues[i] = m_values[i].load();
                return true;
            }
        }
        return false;
    }

    std::string state() const override
    {
        QTemporaryDir directory;
        if (!directory.isValid()) {
            throw std::runtime_error("Could not create an LV2 snapshot directory.");
        }
        // Port snapshots use atomics; the plugin's run() retains its own stable
        // control storage and is never made to race with document serialization.
        LV2EffectSettings settings;
        settings.values.resize(m_ports.mControlPorts.size());
        for (size_t i = 0; i < settings.values.size(); ++i) {
            settings.values[i] = m_ports.mControlPorts[i]->mIsInput
                                 ? plain(i, m_values[i].load(std::memory_order_relaxed))
                                 : m_outputValues[i].load(std::memory_order_relaxed);
        }
        struct PortSnapshot {
            const LV2Ports& ports;
            const LV2EffectSettings& settings;
        } snapshot { m_ports, settings };
        auto getValue = [](const char* symbol, void* data, uint32_t* size, uint32_t* type) -> const void* {
            const auto& snapshot = *static_cast<PortSnapshot*>(data);
            return snapshot.ports.GetPortValue(snapshot.settings, symbol, size, type);
        };
        const auto scratchPath = QFileInfo(m_features.scratch.path()).canonicalFilePath();
        const auto root = QFileInfo(directory.path()).canonicalFilePath();
        const auto savePath = QDir(root).filePath("state");
        const auto copyPath = QDir(root).filePath("copies");
        const auto linkPath = QDir(root).filePath("links");
        QDir().mkpath(savePath);
        QDir().mkpath(copyPath);
        QDir().mkpath(linkPath);
        // Lilv canonicalizes resource names. Canonicalize the roots too (on
        // macOS /var is a symlink), and create nested copy destinations.
        QDirIterator folders(scratchPath, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (folders.hasNext()) {
            const auto relative = QDir(scratchPath).relativeFilePath(folders.next());
            QDir(copyPath).mkpath(relative);
            QDir(savePath).mkpath(relative);
        }
        const auto scratch = QFile::encodeName(scratchPath);
        const auto dir = QFile::encodeName(savePath);
        const auto copy = QFile::encodeName(copyPath);
        const auto link = QFile::encodeName(linkPath);
        const auto features = m_wrapper->GetFeatures().GetFeaturePointers();
        const Lilv_ptr<LilvState, lilv_state_free> saved { lilv_state_new_from_instance(
                                                               &m_effect.mPlug, &m_wrapper->GetInstance(),
                                                               m_features.URIDMapFeature(), scratch.constData(), copy.constData(),
                                                               link.constData(),
                                                               dir.constData(), getValue, &snapshot, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                                                               features.data()) };
        if (!saved || lilv_state_save(LV2Symbols::gWorld, m_features.URIDMapFeature(), m_features.unmap(), saved.get(),
                                      nullptr, dir.constData(), "state.ttl") != 0) {
            throw std::runtime_error("Could not save the LV2 instrument state.");
        }
        QJsonArray files;
        QDirIterator iterator(savePath, QDir::Files, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (iterator.hasNext()) {
            QFile file(iterator.next());
            if (!file.open(QIODevice::ReadOnly)) {
                throw std::runtime_error("Could not archive an LV2 instrument resource.");
            }
            files.append(QJsonObject { { "path", QDir(savePath).relativeFilePath(file.fileName()) },
                             { "data", QString::fromLatin1(file.readAll().toBase64()) } });
        }
        QJsonObject parameters;
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            if (m_ports.mControlPorts[i]->mIsInput) {
                parameters[QString::fromStdString(m_ports.mControlPorts[i]->mSymbol.ToStdString())] = m_values[i].load();
            }
        }
        return QJsonDocument(QJsonObject { { "lv2Files", files }, { "parameters", parameters } })
               .toJson(QJsonDocument::Compact).toStdString();
    }

    void process(float* const* output, size_t channels, size_t frames, std::span<const MidiEvent> events,
                 const InstrumentProcessContext& context) override
    {
        if (frames > m_maximumFrames) {
            throw std::runtime_error("The LV2 audio block exceeds its prepared capacity.");
        }
        if (!m_features.fixed) {
            run(frames, events, context);
            for (size_t channel = 0; channel < channels; ++channel) {
                std::copy_n(m_audioOutput[std::min(channel, m_audioOutput.size() - 1)].data(), frames, output[channel]);
            }
            return;
        }
        // Fixed-block plugins have a streaming adapter. Partial mixer requests
        // never change run()'s block size, and note offsets survive boundaries.
        size_t event = 0;
        for (size_t frame = 0; frame < frames; ++frame) {
            if (!m_pendingFrames) {
                m_pendingContext = context;
                if (m_pendingContext.playing) {
                    m_pendingContext.sample += frame;
                }
                if (m_pendingContext.continuousSample) {
                    *m_pendingContext.continuousSample += frame;
                }
                if (m_pendingContext.renderSample) {
                    *m_pendingContext.renderSample += frame;
                }
                for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
                    m_pendingValues[i] = m_processingValues[i].load(std::memory_order_relaxed);
                }
            }
            while (event < events.size() && events[event].sample <= static_cast<int64_t>(frame)) {
                if (m_pendingEvents.size() == m_pendingEvents.capacity()) {
                    throw std::runtime_error("Too many simultaneous LV2 MIDI events.");
                }
                auto value = events[event++];
                value.sample = m_pendingFrames;
                m_pendingEvents.push_back(value);
            }
            if (++m_pendingFrames == m_blockSize) {
                run(m_blockSize, m_pendingEvents, m_pendingContext);
                m_pendingFrames = 0;
                m_pendingEvents.clear();
                m_readFrame = 0;
            }
            for (size_t channel = 0; channel < channels; ++channel) {
                output[channel][frame] = m_readFrame < m_blockSize
                                         ? m_audioOutput[std::min(channel, m_audioOutput.size() - 1)][m_readFrame] : 0.0f;
            }
            ++m_readFrame;
        }
    }

    bool attachView(void* parent, int x, int y, int width, int height) override
    {
        if (!parent || width <= 0 || height <= 0) {
            return false;
        }
        if (!m_ui) {
            if (m_viewUnavailable) {
                return false;
            }
            if (!buildView(parent)) {
                m_viewUnavailable = true;
                return false;
            }
        }
        m_container->setGeometry(x, y, m_size.width ? m_size.width : width, m_size.height ? m_size.height : height);
        if (m_uiWindow) {
            m_uiWindow->setGeometry(0, 0, m_container->width(), m_container->height());
            m_uiWindow->show();
        }
        m_container->show();
        return true;
    }

    void detachView() override
    {
        m_uiTimer.stop();
        if (m_uiWindow) {
            m_uiWindow->hide();
            m_uiWindow->setParent(nullptr);
        }
        m_foreignWindow.reset();
        m_uiWindow = nullptr;
        m_ui.reset();
        m_widgetContainer.reset();
        m_uiFeatures.reset();
        m_host.reset();
        m_container.reset();
        m_parent.reset();
    }

    audio::InstrumentEditorSize editorSize() const override { return m_size; }

private:
    double plain(size_t index, double value) const
    {
        const auto& port = *m_ports.mControlPorts[index];
        const double scale = port.mSampleRate ? m_rate : 1.0;
        double result = port.mLogarithmic && port.mMin > 0.0f && port.mMax > 0.0f
                        ? std::exp(std::log(port.mMin) + value * std::log(port.mMax / port.mMin))
                        : port.mMin + value * (port.mMax - port.mMin);
        if (port.mToggle) {
            result = value >= 0.5 ? port.mMax : port.mMin;
        } else if (port.mEnumeration && !port.mScaleValues.empty()) {
            result = port.mScaleValues[port.Discretize(result)];
        } else if (port.mInteger) {
            result = std::round(result);
        }
        return result * scale;
    }

    double normalized(size_t index, double value) const
    {
        const auto& port = *m_ports.mControlPorts[index];
        value /= port.mSampleRate ? m_rate : 1.0;
        const double result = port.mMax == port.mMin ? 0.0
                              : port.mLogarithmic && port.mMin > 0.0f && port.mMax > port.mMin
                              ? std::log(std::max(value, static_cast<double>(port.mMin)) / port.mMin) / std::log(port.mMax / port.mMin)
                              : (value - port.mMin) / (port.mMax - port.mMin);
        return std::clamp(result, 0.0, 1.0);
    }

    void instantiate(double rate, size_t maximumFrames, const std::string& state)
    {
        m_rate = rate;
        m_maximumFrames = std::max<size_t>(1, maximumFrames);
        m_blockSize = m_maximumFrames;
        if (m_features.fixed) {
            m_blockSize = 1;
            while (m_blockSize <= m_maximumFrames / 2) {
                m_blockSize *= 2;
            }
        }
        if (m_blockSize < m_options.mMinBlockSize) {
            throw std::runtime_error("The LV2 instrument requires a larger audio block.");
        }
        m_options.mBlockSize = m_blockSize;
        m_options.mMaxBlockSize = m_blockSize;
        m_options.mMinBlockSize = m_features.fixed ? m_blockSize : 1;
        m_options.mSeqSize = atomCapacity;
        m_options.mSampleRate = rate;
        m_portStates = std::make_unique<LV2PortStates>(m_ports);
        for (auto& cv : m_portStates->mCVPortStates) {
            cv.mBuffer.reinit(m_maximumFrames, true);
        }
        m_wrapper = LV2Wrapper::Create(m_options, m_ports, *m_portStates, GetSettings(m_settings), rate, m_outputs.get());
        if (!m_wrapper) {
            throw std::runtime_error("Could not instantiate the LV2 instrument.");
        }
        m_audioInput.assign(m_ports.mAudioIn, std::vector<float>(m_maximumFrames));
        m_audioOutput.assign(m_ports.mAudioOut, std::vector<float>(m_maximumFrames));
        m_atoms.clear();
        m_atoms.resize(m_ports.mAtomPorts.size());
        size_t input = 0, output = 0;
        for (const auto& port : m_ports.mAudioPorts) {
            auto* buffer = port->mIsInput ? m_audioInput[input++].data() : m_audioOutput[output++].data();
            lilv_instance_connect_port(&m_wrapper->GetInstance(), port->mIndex, buffer);
        }
        for (size_t i = 0; i < m_atoms.size(); ++i) {
            if (m_ports.mAtomPorts[i]->mIsInput) {
                m_atoms[i].resize(std::max<uint32_t>(atomCapacity, m_ports.mAtomPorts[i]->mMinimumSize));
                lilv_instance_connect_port(&m_wrapper->GetInstance(), m_ports.mAtomPorts[i]->mIndex, m_atoms[i].data());
            }
        }
        restore(state);
        m_wrapper->Activate();
        publishLatency();
        m_pendingEvents.reserve(8192);
        m_pendingValues.resize(m_ports.mControlPorts.size());
        reset();
    }

    void restore(const std::string& state)
    {
        if (state.empty()) {
            return;
        }
        const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(state)).object();
        if (!object["lv2Files"].isArray()) {
            throw std::runtime_error("The saved LV2 instrument state is invalid.");
        }
        m_restored = std::make_unique<QTemporaryDir>();
        if (!m_restored->isValid()) {
            throw std::runtime_error("Could not restore LV2 instrument storage.");
        }
        for (const auto& entry : object["lv2Files"].toArray()) {
            const auto file = entry.toObject();
            const auto relative = QDir::cleanPath(file["path"].toString());
            if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../")) {
                throw std::runtime_error("The LV2 instrument state contains an invalid resource path.");
            }
            const auto path = QDir(m_restored->path()).filePath(relative);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile output(path);
            const auto bytes = QByteArray::fromBase64(file["data"].toString().toLatin1());
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) {
                throw std::runtime_error("Could not restore an LV2 instrument resource.");
            }
        }
        const auto path = QFile::encodeName(QDir(m_restored->path()).filePath("state.ttl"));
        const Lilv_ptr<LilvState, lilv_state_free> saved {
            lilv_state_new_from_file(LV2Symbols::gWorld, m_features.URIDMapFeature(), nullptr, path.constData()) };
        if (!saved || !lilv_node_equals(lilv_state_get_plugin_uri(saved.get()), lilv_plugin_get_uri(&m_effect.mPlug))) {
            throw std::runtime_error("The LV2 state belongs to another instrument or is damaged.");
        }
        const auto features = m_wrapper->GetFeatures().GetFeaturePointers();
        auto setValue = [](const char* symbol, void* data, const void* value, uint32_t size, uint32_t type) {
            auto& self = *static_cast<Lv2Instrument*>(data);
            self.m_ports.SetPortValue(GetSettings(self.m_settings), symbol, value, size, type);
        };
        lilv_state_restore(saved.get(), &m_wrapper->GetInstance(), setValue, this, 0, features.data());
        const auto& values = GetSettings(m_settings).values;
        const auto parameters = object["parameters"].toObject();
        for (size_t i = 0; i < values.size(); ++i) {
            const auto id = QString::fromStdString(m_ports.mControlPorts[i]->mSymbol.ToStdString());
            m_values[i].store(parameters.contains(id) ? std::clamp(parameters[id].toDouble(), 0.0, 1.0) : normalized(i, values[i]));
            m_processingValues[i] = m_values[i].load();
        }
    }

    void run(size_t frames, std::span<const MidiEvent> events, const InstrumentProcessContext& context)
    {
        auto& values = GetSettings(m_settings).values;
        for (size_t i = 0; i < values.size(); ++i) {
            if (m_ports.mControlPorts[i]->mIsInput) {
                values[i] = plain(i, m_features.fixed ? m_pendingValues[i] : m_processingValues[i].load(std::memory_order_relaxed));
            }
        }
        for (size_t i = 0; i < m_ports.mAtomPorts.size(); ++i) {
            const auto& port = *m_ports.mAtomPorts[i];
            auto& state = *m_portStates->mAtomPortStates[i];
            if (!port.mIsInput) {
                state.ResetForInstanceOutput();
                continue;
            }
            LV2_Atom_Forge_Frame sequence;
            lv2_atom_forge_set_buffer(&m_forge, m_atoms[i].data(), m_atoms[i].size());
            lv2_atom_forge_sequence_head(&m_forge, &sequence, 0);
            if (port.mWantsPosition) {
                LV2_Atom_Forge_Frame position;
                const double beat = context.sample / m_rate * context.tempo / 60.0 * context.denominator / 4.0;
                lv2_atom_forge_frame_time(&m_forge, 0);
                lv2_atom_forge_object(&m_forge, &position, 0, m_position);
                lv2_atom_forge_key(&m_forge, m_frame);
                lv2_atom_forge_long(&m_forge, context.sample);
                lv2_atom_forge_key(&m_forge, m_speed);
                lv2_atom_forge_float(&m_forge, context.playing ? 1.0f : 0.0f);
                lv2_atom_forge_key(&m_forge, m_tempo);
                lv2_atom_forge_float(&m_forge, context.tempo);
                lv2_atom_forge_key(&m_forge, m_bar);
                lv2_atom_forge_long(&m_forge, static_cast<int64_t>(beat / context.numerator));
                lv2_atom_forge_key(&m_forge, m_beat);
                lv2_atom_forge_float(&m_forge, std::fmod(beat, context.numerator));
                lv2_atom_forge_key(&m_forge, m_beatsPerBar);
                lv2_atom_forge_float(&m_forge, context.numerator);
                lv2_atom_forge_key(&m_forge, m_beatUnit);
                lv2_atom_forge_int(&m_forge, context.denominator);
                lv2_atom_forge_pop(&m_forge, &position);
            }
            // UI events travel through the original host's SPSC atom ring.
            LV2_Atom atom;
            while (zix_ring_peek(state.mRing.get(), &atom, sizeof(atom)) == sizeof(atom)) {
                const auto size = lv2_atom_total_size(&atom);
                if (size > port.mMinimumSize || zix_ring_read_space(state.mRing.get()) < size) {
                    throw std::runtime_error("The LV2 editor sent an invalid atom message.");
                }
                zix_ring_read(state.mRing.get(), state.mBuffer.get(), size);
                lv2_atom_forge_frame_time(&m_forge, 0);
                if (!lv2_atom_forge_write(&m_forge, state.mBuffer.get(), size)) {
                    throw std::runtime_error("The LV2 editor sequence exceeds its prepared capacity.");
                }
            }
            if (port.mIsMidi) {
                auto writeMidi = [&](const MidiEvent& event) {
                    const uint8_t bytes[] { event.status, event.data1, event.data2 };
                    const auto type = event.status & 0xf0;
                    const uint32_t size = type == 0xc0 || type == 0xd0 ? 2 : 3;
                    lv2_atom_forge_frame_time(&m_forge, event.sample);
                    if (!lv2_atom_forge_atom(&m_forge, size, m_midi)
                        || !lv2_atom_forge_write(&m_forge, bytes, size)) {
                        throw std::runtime_error("The LV2 MIDI sequence exceeds its prepared capacity.");
                    }
                };
                if (m_reset) {
                    for (uint8_t channel = 0; channel < 16; ++channel) {
                        writeMidi({ 0, static_cast<uint8_t>(0xb0 | channel), 64, 0 });
                        writeMidi({ 0, static_cast<uint8_t>(0xb0 | channel), 120, 0 });
                        writeMidi({ 0, static_cast<uint8_t>(0xb0 | channel), 123, 0 });
                    }
                }
                for (const auto& event : events) {
                    if (event.sample >= 0 && event.sample < static_cast<int64_t>(frames)) {
                        writeMidi(event);
                    }
                }
            }
            lv2_atom_forge_pop(&m_forge, &sequence);
        }
        m_reset = false;
        lilv_instance_run(&m_wrapper->GetInstance(), frames);
        m_wrapper->ConsumeResponses();
        publishLatency();
        for (auto& state : m_portStates->mAtomPortStates) {
            state->ReceiveFromInstance();
        }
        const auto& outputs = static_cast<const LV2EffectOutputs&>(*m_outputs).values;
        for (size_t i = 0; i < outputs.size(); ++i) {
            m_outputValues[i].store(outputs[i], std::memory_order_relaxed);
        }
    }

    void publishLatency()
    {
        // Only the owner of run()/instantiation reads the plugin's plain
        // output port. UI/transport readers use the atomic snapshot instead.
        m_latency.store((m_features.fixed ? m_blockSize - 1 : 0)
                        + static_cast<size_t>(std::max(0.0f, m_wrapper->GetLatency())), std::memory_order_release);
    }

    bool buildView(void* parent)
    {
        const Lilv_ptr<LilvUIs, lilv_uis_free> uis { lilv_plugin_get_uis(&m_effect.mPlug) };
        const char* nativeType =
#if defined(Q_OS_MACOS)
            LV2_UI__CocoaUI;
#elif defined(Q_OS_WIN)
            LV2_UI__WindowsUI;
#else
            LV2_UI__X11UI;
#endif
        for (const auto* type : { nativeType, LV2_UI__Qt6UI }) {
            const LilvNodePtr typeNode { lilv_new_uri(LV2Symbols::gWorld, type) };
            LILV_FOREACH(uis, iter, uis.get()) {
                const auto* ui = lilv_uis_get(uis.get(), iter);
                if (!lilv_ui_is_a(ui, typeNode.get())) {
                    continue;
                }
                m_parent.reset(QWindow::fromWinId(reinterpret_cast<WId>(parent)));
                m_container = std::make_unique<QWindow>();
                m_container->setParent(m_parent.get());
                void* parentHandle = reinterpret_cast<void*>(m_container->winId());
                if (!std::strcmp(type, LV2_UI__Qt6UI)) {
                    m_widgetContainer = std::make_unique<QWidget>();
                    m_widgetContainer->setAttribute(Qt::WA_NativeWindow);
                    m_widgetContainer->winId();
                    m_widgetContainer->windowHandle()->setParent(m_container.get());
                    parentHandle = m_widgetContainer.get();
                }
                m_uiFeatures = std::make_unique<LV2UIFeaturesList>(m_wrapper->GetFeatures(), static_cast<UIHandler*>(this),
                                                                   lilv_ui_get_uri(ui), &m_wrapper->GetInstance(), parentHandle);
                if (!m_uiFeatures->mOk) {
                    detachView();
                    continue;
                }
                m_host.reset(suil_host_new(LV2UIFeaturesList::suil_port_write, LV2UIFeaturesList::suil_port_index, nullptr, nullptr));
                const LilvCharsPtr bundle { lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_bundle_uri(ui)), nullptr) };
                const LilvCharsPtr binary { lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_binary_uri(ui)), nullptr) };
                const auto features = m_uiFeatures->GetFeaturePointers();
                // Suil's callbacks dispatch through UIHandler, a secondary base.
                m_ui.reset(suil_instance_new(m_host.get(), static_cast<UIHandler*>(this), type,
                                             lilv_node_as_uri(lilv_plugin_get_uri(&m_effect.mPlug)),
                                             lilv_node_as_uri(lilv_ui_get_uri(ui)), type, bundle.get(), binary.get(), features.data()));
                if (!m_ui || !suil_instance_get_widget(m_ui.get())) {
                    detachView();
                    continue;
                }
                QLibrary library(QString::fromUtf8(binary.get()));
                library.setLoadHints(QLibrary::PreventUnloadHint | QLibrary::ResolveAllSymbolsHint);
                library.load();
                if (!std::strcmp(type, LV2_UI__Qt6UI)) {
                    auto* widget = static_cast<QWidget*>(suil_instance_get_widget(m_ui.get()));
                    widget->setParent(m_widgetContainer.get());
                    m_size = { widget->width(), widget->height() };
                    m_widgetContainer->resize(widget->size());
                    widget->show();
                    m_widgetContainer->show();
                    m_uiWindow = m_widgetContainer->windowHandle();
                } else {
                    m_foreignWindow.reset(QWindow::fromWinId(reinterpret_cast<WId>(suil_instance_get_widget(m_ui.get()))));
                    m_uiWindow = m_foreignWindow.get();
                    if (m_uiWindow && !m_size.width) {
                        m_size = { m_uiWindow->width(), m_uiWindow->height() };
                    }
                }
                if (!m_uiWindow) {
                    detachView();
                    continue;
                }
                m_uiWindow->setParent(m_container.get());
                m_uiValues.assign(m_ports.mControlPorts.size(), std::numeric_limits<float>::quiet_NaN());
                idle();
                m_uiTimer.start(30);
                return true;
            }
        }
        return false;
    }

    void idle()
    {
        if (!m_ui) {
            return;
        }
        for (size_t i = 0; i < m_ports.mControlPorts.size(); ++i) {
            const auto& port = *m_ports.mControlPorts[i];
            const float value = port.mIsInput ? plain(i, m_values[i].load(std::memory_order_relaxed))
                                : m_outputValues[i].load(std::memory_order_relaxed);
            if (value != m_uiValues[i]) {
                m_uiValues[i] = value;
                suil_instance_port_event(m_ui.get(), port.mIndex, sizeof(value), 0, &value);
            }
        }
        for (auto& state : m_portStates->mAtomPortStates) {
            if (!state->mpPort->mIsInput) {
                state->SendToDialog([&](const LV2_Atom* atom, uint32_t size) {
                    suil_instance_port_event(m_ui.get(), state->mpPort->mIndex, size, LV2Symbols::urid_EventTransfer, atom);
                });
            }
        }
        const auto* idle = static_cast<const LV2UI_Idle_Interface*>(suil_instance_extension_data(m_ui.get(), LV2_UI__idleInterface));
        if (idle && idle->idle) {
            idle->idle(suil_instance_get_handle(m_ui.get()));
        }
    }

    int ui_resize(int width, int height) override
    {
        if (width <= 0 || height <= 0) {
            return 1;
        }
        m_size = { width, height };
        return 0;
    }

    void ui_closed() override { m_uiTimer.stop(); }
    void suil_port_write(uint32_t port, uint32_t size, uint32_t protocol, const void* buffer) override
    {
        if (!buffer) {
            return;
        }
        if (protocol == 0 && size == sizeof(float)) {
            const auto found = m_ports.mControlPortMap.find(port);
            if (found != m_ports.mControlPortMap.end() && m_ports.mControlPorts[found->second]->mIsInput) {
                setParameter(m_ports.mControlPorts[found->second]->mSymbol.ToStdString(), normalized(found->second,
                                                                                                     *static_cast<const float*>(buffer)));
            }
        } else if (protocol == LV2Symbols::urid_EventTransfer) {
            for (const auto& state : m_portStates->mAtomPortStates) {
                if (state->mpPort->mIsInput && state->mpPort->mIndex == port) {
                    state->ReceiveFromDialog(buffer, size);
                    m_changed.store(true);
                }
            }
        }
    }

    uint32_t suil_port_index(const char* symbol) override
    {
        const LilvNodePtr node { lilv_new_string(LV2Symbols::gWorld, symbol) };
        const auto* port = lilv_plugin_get_port_by_symbol(&m_effect.mPlug, node.get());
        return port ? lilv_port_get_index(&m_effect.mPlug, port) : LV2UI_INVALID_PORT_INDEX;
    }

    static constexpr uint32_t atomCapacity = 262144;
    LV2EffectBase m_effect;
    InstrumentFeatures m_features;
    LV2InstanceFeaturesList m_options;
    EffectSettings m_settings;
    std::unique_ptr<EffectOutputs> m_outputs;
    const LV2Ports& m_ports;
    std::unique_ptr<std::atomic<double>[]> m_values;
    std::unique_ptr<std::atomic<double>[]> m_processingValues;
    std::vector<std::string> m_idStrings;
    std::unique_ptr<std::atomic<float>[]> m_outputValues;
    std::unique_ptr<LV2PortStates> m_portStates;
    std::unique_ptr<QTemporaryDir> m_restored;
    std::unique_ptr<LV2Wrapper> m_wrapper;
    std::vector<std::vector<float> > m_audioInput, m_audioOutput;
    std::vector<std::vector<uint8_t> > m_atoms;
    LV2_Atom_Forge m_forge {};
    LV2_URID m_midi {}, m_position {}, m_frame {}, m_speed {}, m_tempo {}, m_beat {}, m_bar {}, m_beatsPerBar {}, m_beatUnit {};
    double m_rate = 44100.0;
    size_t m_maximumFrames = 512, m_blockSize = 512;
    bool m_reset = true;
    bool m_viewUnavailable = false;
    std::atomic<bool> m_changed { false };
    std::atomic<size_t> m_latency { 0 };
    std::vector<MidiEvent> m_pendingEvents;
    std::vector<double> m_pendingValues;
    size_t m_pendingFrames = 0, m_readFrame = 512;
    InstrumentProcessContext m_pendingContext;
    Lilv_ptr<SuilHost, suil_host_free> m_host;
    std::unique_ptr<LV2UIFeaturesList> m_uiFeatures;
    Lilv_ptr<SuilInstance, suil_instance_free> m_ui;
    std::unique_ptr<QWindow> m_parent, m_container, m_foreignWindow;
    std::unique_ptr<QWidget> m_widgetContainer;
    QWindow* m_uiWindow = nullptr;
    QTimer m_uiTimer;
    audio::InstrumentEditorSize m_size;
    std::vector<float> m_uiValues;
};
}

std::shared_ptr<InstrumentProcessor> createLv2Instrument(const std::string& path, const std::string& state)
{
    return std::make_shared<Lv2Instrument>(path, state);
}
}
