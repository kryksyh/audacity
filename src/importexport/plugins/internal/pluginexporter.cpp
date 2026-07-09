/*
* Audacity: A Digital Audio Editor
*/
#include "pluginexporter.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "au3-files/wxFileNameWrapper.h"
#include "au3-import-export/ExportOptionsEditor.h"
#include "au3-import-export/ExportPluginHelpers.h"
#include "au3-math/SampleFormat.h"
#include "au3-mixer/Mix.h"
#include "au3-strings/Internat.h"
#include "au3-tags/Tags.h"

#include "framework/global/log.h"

using namespace au::importexport;

namespace {
constexpr size_t FRAMES_PER_RUN = 8192;

//! The plugin API has no per-format options yet; the export dialog gets none
class EmptyOptionsEditor final : public ExportOptionsEditor
{
public:
    explicit EmptyOptionsEditor(std::string name)
        : m_name(std::move(name)) {}

    std::string GetName() const override { return m_name; }
    int GetOptionsCount() const override { return 0; }
    bool GetOption(int, ExportOption&) const override { return false; }
    bool GetValue(ExportOptionID, ExportValue&) const override { return false; }
    bool SetValue(ExportOptionID, const ExportValue&) override { return false; }
    SampleRateList GetSampleRateList() const override { return {}; }
    void Store(audacity::BasicSettings&) const override {}
    void Load(const audacity::BasicSettings&) override {}

private:
    const std::string m_name;
};

class PluginExportProcessor final : public ExportProcessor
{
public:
    PluginExportProcessor(const auplug_exporter_factory_t& factory, uint32_t index, wxString formatName)
        : m_factory(factory), m_index(index), m_formatName(std::move(formatName)) {}

    bool Initialize(AudacityProject& project, const Parameters& parameters, const wxFileNameWrapper& filename,
                    double t0, double t1, bool selectedOnly, double rate, unsigned channels,
                    MixerOptions::Downmix* mixerSpec, const Tags* tags) override;
    ExportResult Process(ExportProcessorDelegate& delegate) override;

private:
    const auplug_exporter_factory_t& m_factory;
    const uint32_t m_index;
    const wxString m_formatName;

    struct {
        double t0 = 0.0;
        double t1 = 0.0;
        double rate = 0.0;
        unsigned channels = 0;
        std::string path;
        std::unique_ptr<Mixer> mixer;
        std::vector<std::pair<std::string, std::string> > tags;
    } context;
};

bool PluginExportProcessor::Initialize(AudacityProject& project, const Parameters&,
                                       const wxFileNameWrapper& filename, double t0, double t1, bool selectedOnly,
                                       double rate, unsigned channels, MixerOptions::Downmix* mixerSpec,
                                       const Tags* tags)
{
    context.t0 = t0;
    context.t1 = t1;
    context.rate = rate;
    context.channels = channels;
    context.path = filename.GetFullPath().ToUTF8().data();

    context.mixer = ExportPluginHelpers::CreateMixer(project, selectedOnly, t0, t1, channels, FRAMES_PER_RUN,
                                                     false, rate, floatSample, mixerSpec);

    if (!tags) {
        tags = &Tags::Get(project);
    }
    for (const auto& pair : tags->GetRange()) {
        context.tags.emplace_back(pair.first.ToUTF8().data(), pair.second.ToUTF8().data());
    }

    return true;
}

ExportResult PluginExportProcessor::Process(ExportProcessorDelegate& delegate)
{
    delegate.SetStatusString(Verbatim(wxString::Format(wxT("Exporting audio as %s"), m_formatName)));

    struct Source {
        Mixer* mixer = nullptr;
        ExportProcessorDelegate* delegate = nullptr;
        double t0 = 0.0;
        double t1 = 0.0;
        double rate = 0.0;
        unsigned channels = 0;
        std::vector<float*> ptrs;
        const std::vector<std::pair<std::string, std::string> >* tags = nullptr;
        ExportResult state = ExportResult::Success;
    } source;
    source.mixer = context.mixer.get();
    source.delegate = &delegate;
    source.t0 = context.t0;
    source.t1 = context.t1;
    source.rate = context.rate;
    source.channels = context.channels;
    source.tags = &context.tags;

    auplug_export_ctx_t ectx;
    ectx.struct_size = sizeof(ectx);
    ectx.ctx = &source;
    ectx.sample_rate = context.rate;
    ectx.channel_count = context.channels;

    ectx.read = [](void* c, uint32_t maxFrames, auplug_audio_buffer_t* out) {
        auto* s = static_cast<Source*>(c);
        if (!out || s->state != ExportResult::Success) {
            return false;
        }
        const size_t frames = s->mixer->Process(std::min<size_t>(maxFrames, FRAMES_PER_RUN));
        if (frames == 0) {
            return false;
        }
        s->state = ExportPluginHelpers::UpdateProgress(*s->delegate, *s->mixer, s->t0, s->t1);

        s->ptrs.resize(s->channels);
        for (unsigned ch = 0; ch < s->channels; ++ch) {
            s->ptrs[ch] = const_cast<float*>(reinterpret_cast<const float*>(s->mixer->GetBuffer(ch)));
        }
        out->channel_count = s->channels;
        out->frame_count = frames;
        out->sample_rate = s->rate;
        out->channels = s->ptrs.data();
        return true;
    };

    ectx.tag_count = [](void* c) {
        return static_cast<uint32_t>(static_cast<Source*>(c)->tags->size());
    };

    ectx.get_tag = [](void* c, uint32_t index, const char** key, const char** value) {
        auto* s = static_cast<Source*>(c);
        if (index >= s->tags->size() || !key || !value) {
            return false;
        }
        *key = (*s->tags)[index].first.c_str();
        *value = (*s->tags)[index].second.c_str();
        return true;
    };

    const int32_t res = m_factory.export_file(m_index, context.path.c_str(), &ectx);

    if (res == AUPLUG_PROCESS_ERROR) {
        if (m_factory.last_error) {
            LOGE() << "plugin exporter failed: " << m_factory.last_error(m_index);
        }
        return ExportResult::Error;
    }
    if (source.state != ExportResult::Success) {
        return source.state;
    }
    return res == AUPLUG_PROCESS_CANCELLED ? ExportResult::Cancelled : ExportResult::Success;
}
}

PluginExporter::PluginExporter(const auplug_exporter_factory_t& factory, uint32_t index)
    : m_factory(factory), m_index(index)
{
    auplug_exporter_desc_t desc;
    desc.struct_size = sizeof(desc);
    if (!factory.get_descriptor || !factory.get_descriptor(index, &desc)
        || !desc.id || !desc.format_name || !desc.extension || !factory.export_file) {
        return;
    }

    m_id = desc.id;
    m_formatName = desc.format_name;
    m_extension = desc.extension;
    m_maxChannels = desc.max_channels;
    m_valid = true;
}

int PluginExporter::GetFormatCount() const
{
    return 1;
}

FormatInfo PluginExporter::GetFormatInfo(int) const
{
    return {
        wxString::FromUTF8(m_id),
        Verbatim(wxString::FromUTF8(m_formatName)),
        { wxString::FromUTF8(m_extension) },
        m_maxChannels,
        true
    };
}

std::unique_ptr<ExportOptionsEditor> PluginExporter::CreateOptionsEditor(int, ExportOptionsEditor::Listener*) const
{
    return std::make_unique<EmptyOptionsEditor>(m_id);
}

std::unique_ptr<ExportProcessor> PluginExporter::CreateProcessor(int) const
{
    return std::make_unique<PluginExportProcessor>(m_factory, m_index, wxString::FromUTF8(m_formatName));
}
