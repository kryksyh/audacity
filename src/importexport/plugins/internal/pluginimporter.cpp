/*
* Audacity: A Digital Audio Editor
*/
#include "pluginimporter.h"

#include <algorithm>

#include <wx/arrstr.h>

#include "au3-import-export/ImportProgressListener.h"
#include "au3-import-export/ImportUtils.h"
#include "au3-math/SampleFormat.h"
#include "au3-tags/Tags.h"
#include "au3-wave-track/WaveTrack.h"

#include "framework/global/log.h"

using namespace au::importexport;

namespace {
FileExtensions extensionsFor(const auplug_importer_factory_t& factory, uint32_t index)
{
    FileExtensions result;
    auplug_importer_desc_t desc;
    desc.struct_size = sizeof(desc);
    if (factory.get_descriptor && factory.get_descriptor(index, &desc) && desc.extensions) {
        for (const wxString& ext : wxSplit(wxString::FromUTF8(desc.extensions), ';')) {
            if (!ext.empty()) {
                result.push_back(ext);
            }
        }
    }
    return result;
}
}

PluginImporter::PluginImporter(const auplug_importer_factory_t& factory, uint32_t index)
    : ImportPlugin(extensionsFor(factory, index)), m_factory(factory), m_index(index)
{
    auplug_importer_desc_t desc;
    desc.struct_size = sizeof(desc);
    if (!factory.get_descriptor || !factory.get_descriptor(index, &desc)
        || !desc.id || !desc.format_name) {
        return;
    }

    m_id = desc.id;
    m_formatName = desc.format_name;
    m_valid = !mExtensions.empty();
}

wxString PluginImporter::GetPluginStringID()
{
    return wxString::FromUTF8(m_id);
}

TranslatableString PluginImporter::GetPluginFormatDescription()
{
    return Verbatim(wxString::FromUTF8(m_formatName));
}

std::unique_ptr<ImportFileHandle> PluginImporter::Open(const FilePath& filename, AudacityProject*)
{
    if (!m_valid || !m_factory.can_open || !m_factory.import) {
        return nullptr;
    }
    if (!m_factory.can_open(m_index, filename.ToUTF8().data())) {
        return nullptr;
    }
    return std::make_unique<PluginImportHandle>(m_factory, m_index, filename, wxString::FromUTF8(m_formatName));
}

PluginImportHandle::PluginImportHandle(const auplug_importer_factory_t& factory, uint32_t index,
                                       const FilePath& filename, const wxString& formatName)
    : ImportFileHandleEx(filename), m_factory(factory), m_index(index), m_formatName(formatName)
{
}

TranslatableString PluginImportHandle::GetFileDescription()
{
    return Verbatim(m_formatName);
}

auto PluginImportHandle::GetFileUncompressedBytes() -> ByteCount
{
    return 0;
}

double PluginImportHandle::GetDuration() const
{
    return 0.0;
}

int PluginImportHandle::GetRequiredTrackCount() const
{
    return 1;
}

wxInt32 PluginImportHandle::GetStreamCount()
{
    return 1;
}

const TranslatableStrings& PluginImportHandle::GetStreamInfo()
{
    static const TranslatableStrings empty;
    return empty;
}

void PluginImportHandle::SetStreamUsage(wxInt32, bool)
{
}

void PluginImportHandle::Import(ImportProgressListener& progressListener, WaveTrackFactory* trackFactory,
                                TrackHolders& outTracks, Tags* tags,
                                std::optional<LibFileFormats::AcidizerTags>&)
{
    BeginImport();
    outTracks.clear();

    struct Sink {
        PluginImportHandle* self = nullptr;
        ImportProgressListener* listener = nullptr;
        WaveTrackFactory* factory = nullptr;
        TrackHolders* outTracks = nullptr;
        Tags* tags = nullptr;
        std::shared_ptr<WaveTrack> track;
        uint32_t channels = 0;
        bool tagsCleared = false;
    } sink;
    sink.self = this;
    sink.listener = &progressListener;
    sink.factory = trackFactory;
    sink.outTracks = &outTracks;
    sink.tags = tags;

    auplug_import_ctx_t ictx;
    ictx.struct_size = sizeof(ictx);
    ictx.ctx = &sink;

    ictx.begin_stream = [](void* c, uint32_t channelCount, double sampleRate) {
        auto* s = static_cast<Sink*>(c);
        if (channelCount == 0 || sampleRate <= 0) {
            return false;
        }
        if (s->track) {
            ImportUtils::FinalizeImport(*s->outTracks, *s->track);
        }
        s->track = ImportUtils::NewWaveTrack(*s->factory, channelCount, floatSample, sampleRate);
        s->channels = channelCount;
        return s->track != nullptr;
    };

    ictx.append = [](void* c, const auplug_audio_buffer_t* audio) {
        auto* s = static_cast<Sink*>(c);
        if (!s->track || !audio || !audio->channels || audio->channel_count != s->channels) {
            return false;
        }
        unsigned chn = 0;
        ImportUtils::ForEachChannel(*s->track, [&](WaveChannel& channel) {
            channel.AppendBuffer(reinterpret_cast<constSamplePtr>(audio->channels[chn]), floatSample,
                                 static_cast<size_t>(audio->frame_count), 1, floatSample);
            ++chn;
        });
        return true;
    };

    ictx.add_tag = [](void* c, const char* key, const char* value) {
        auto* s = static_cast<Sink*>(c);
        if (!s->tags || !key || !value) {
            return false;
        }
        if (!s->tagsCleared) {
            s->tags->Clear();
            s->tagsCleared = true;
        }
        s->tags->SetTag(wxString::FromUTF8(key), wxString::FromUTF8(value));
        return true;
    };

    ictx.progress = [](void* c, double fraction) {
        auto* s = static_cast<Sink*>(c);
        s->listener->OnImportProgress(std::clamp(fraction, 0.0, 1.0));
        return !s->self->IsCancelled() && !s->self->IsStopped();
    };

    const int32_t res = m_factory.import(m_index, GetFilename().ToUTF8().data(), &ictx);

    if (res == AUPLUG_PROCESS_OK || (res == AUPLUG_PROCESS_CANCELLED && IsStopped())) {
        if (sink.track) {
            ImportUtils::FinalizeImport(outTracks, *sink.track);
            sink.track.reset();
        }
        progressListener.OnImportResult(IsStopped()
                                        ? ImportProgressListener::ImportResult::Stopped
                                        : ImportProgressListener::ImportResult::Success);
        return;
    }

    if (res == AUPLUG_PROCESS_CANCELLED) {
        progressListener.OnImportResult(ImportProgressListener::ImportResult::Cancelled);
        return;
    }

    if (m_factory.last_error) {
        LOGE() << "plugin importer failed: " << m_factory.last_error(m_index);
    }
    progressListener.OnImportResult(ImportProgressListener::ImportResult::Error);
}
