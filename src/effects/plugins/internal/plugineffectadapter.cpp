/*
* Audacity: A Digital Audio Editor
*/
#include "plugineffectadapter.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "au3-basic-ui/BasicUI.h"
#include "au3-components/SettingsVisitor.h"
#include "au3-effects/EffectOutputTracks.h"
#include "au3-label-track/AnalysisTracks.h"
#include "au3-label-track/LabelTrack.h"
#include "au3-strings/TranslatableString.h"
#include "au3-time-frequency-selection/SelectedRegion.h"
#include "au3-track/TimeWarper.h"
#include "au3-wave-track/WaveTrack.h"

#include "pluginhost/hostdsp.h"

#include "framework/global/log.h"

using namespace au::effects;

PluginEffectAdapter::PluginEffectAdapter(const auplug_effect_factory_t& factory, const auplug_effect_desc_t& desc)
    : m_desc(desc)
{
    m_fx = factory.create(desc.id);
    IF_ASSERT_FAILED(m_fx) {
        return;
    }

    m_paramsExt = static_cast<const auplug_params_ext_t*>(m_fx->get_extension(m_fx, AUPLUG_PARAMS_EXT_ID));
    initParams();
}

PluginEffectAdapter::~PluginEffectAdapter()
{
    if (m_fx) {
        m_fx->destroy(m_fx);
    }
}

void PluginEffectAdapter::initParams()
{
    if (!m_paramsExt) {
        return;
    }

    const uint32_t count = m_paramsExt->count(m_fx);
    m_params.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        auplug_param_info_t info;
        info.struct_size = sizeof(info);
        if (!m_paramsExt->get_info(m_fx, i, &info) || !info.key) {
            continue;
        }

        Param p;
        p.id = info.id;
        p.key = info.key;
        p.wxKey = wxString::FromUTF8(info.key);
        p.label = wxString::FromUTF8(info.label ? info.label : info.key);
        p.type = info.type;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.defaultValue = info.default_value;
        p.defaultString = wxString::FromUTF8(info.default_string ? info.default_string : "");

        switch (p.type) {
        case AUPLUG_PARAM_DOUBLE:
            p.num = m_paramsExt->get_number(m_fx, p.id);
            break;
        case AUPLUG_PARAM_INT:
            p.inum = static_cast<int>(m_paramsExt->get_number(m_fx, p.id));
            break;
        case AUPLUG_PARAM_BOOL:
            p.flag = m_paramsExt->get_number(m_fx, p.id) != 0.0;
            break;
        case AUPLUG_PARAM_ENUM: {
            p.inum = static_cast<int>(m_paramsExt->get_number(m_fx, p.id));
            // max_value < min_value means the plugin currently has no choices
            // (e.g. no AI models found on disk).
            const int nchoices = p.maxValue >= p.minValue
                                 ? static_cast<int>(p.maxValue - p.minValue) + 1 : 0;
            p.symbols.reserve(nchoices);
            char buf[128];
            for (int c = 0; c < nchoices; ++c) {
                const double value = p.minValue + c;
                if (m_paramsExt->value_to_text(m_fx, p.id, value, buf, sizeof(buf))) {
                    p.symbols.push_back(wxString::FromUTF8(buf));
                } else {
                    p.symbols.push_back(wxString::Format(wxT("%d"), c));
                }
            }
            break;
        }
        case AUPLUG_PARAM_STRING: {
            const char* s = m_paramsExt->get_string(m_fx, p.id);
            p.str = wxString::FromUTF8(s ? s : "");
            break;
        }
        default:
            LOGW() << "unknown param type " << p.type << " for key " << p.key;
            continue;
        }

        m_params.push_back(std::move(p));
    }
}

void PluginEffectAdapter::pushParams()
{
    if (!m_paramsExt) {
        return;
    }

    for (const Param& p : m_params) {
        switch (p.type) {
        case AUPLUG_PARAM_DOUBLE:
            m_paramsExt->set_number(m_fx, p.id, p.num);
            break;
        case AUPLUG_PARAM_INT:
        case AUPLUG_PARAM_ENUM:
            m_paramsExt->set_number(m_fx, p.id, p.inum);
            break;
        case AUPLUG_PARAM_BOOL:
            m_paramsExt->set_number(m_fx, p.id, p.flag ? 1.0 : 0.0);
            break;
        case AUPLUG_PARAM_STRING:
            m_paramsExt->set_string(m_fx, p.id, p.str.ToUTF8().data());
            break;
        default:
            break;
        }
    }
}

PluginPath PluginEffectAdapter::pathForId(const char* effectId)
{
    return wxString::Format(wxT("auplug://%s"), wxString::FromUTF8(effectId));
}

PluginPath PluginEffectAdapter::GetPath() const
{
    return pathForId(m_desc.id);
}

ComponentInterfaceSymbol PluginEffectAdapter::GetSymbol() const
{
    return ComponentInterfaceSymbol { wxString::FromUTF8(m_desc.id),
                                      Verbatim(wxString::FromUTF8(m_desc.name)) };
}

VendorSymbol PluginEffectAdapter::GetVendor() const
{
    return VendorSymbol { Verbatim(wxString::FromUTF8(m_desc.vendor)) };
}

wxString PluginEffectAdapter::GetVersion() const
{
    return wxString::FromUTF8(m_desc.version);
}

TranslatableString PluginEffectAdapter::GetDescription() const
{
    return Verbatim(wxString::FromUTF8(m_desc.description ? m_desc.description : ""));
}

::EffectType PluginEffectAdapter::GetType() const
{
    switch (m_desc.type) {
    case AUPLUG_EFFECT_GENERATOR: return EffectTypeGenerate;
    case AUPLUG_EFFECT_ANALYZER: return EffectTypeAnalyze;
    default: return EffectTypeProcess;
    }
}

EffectFamilySymbol PluginEffectAdapter::GetFamily() const
{
    return ComponentInterfaceSymbol { wxT("Plugin") };
}

bool PluginEffectAdapter::IsInteractive() const
{
    return !m_params.empty();
}

template<typename Self, typename Visitor>
bool PluginEffectAdapter::visitImpl(Self& self, Visitor& visitor)
{
    for (auto& p : self.m_params) {
        switch (p.type) {
        case AUPLUG_PARAM_DOUBLE:
            visitor.Define(p.num, p.wxKey.wc_str(), p.defaultValue, p.minValue, p.maxValue);
            break;
        case AUPLUG_PARAM_INT:
            visitor.Define(p.inum, p.wxKey.wc_str(), static_cast<int>(p.defaultValue),
                           static_cast<int>(p.minValue), static_cast<int>(p.maxValue));
            break;
        case AUPLUG_PARAM_BOOL:
            visitor.Define(p.flag, p.wxKey.wc_str(), p.defaultValue != 0.0);
            break;
        case AUPLUG_PARAM_ENUM:
            // A plugin's choice list may be empty at runtime (e.g. no AI
            // models installed); indexing an empty EnumValueSymbol array
            // in ShuttleGetAutomation::DefineEnum is undefined behavior.
            if (!p.symbols.empty()) {
                visitor.DefineEnum(p.inum, p.wxKey.wc_str(), static_cast<int>(p.defaultValue),
                                   p.symbols.data(), p.symbols.size());
            }
            break;
        case AUPLUG_PARAM_STRING:
            visitor.Define(p.str, p.wxKey.wc_str(), p.defaultString);
            break;
        default:
            break;
        }
    }
    return true;
}

bool PluginEffectAdapter::VisitSettings(SettingsVisitor& visitor, EffectSettings&)
{
    return visitImpl(*this, visitor);
}

bool PluginEffectAdapter::VisitSettings(ConstSettingsVisitor& visitor, const EffectSettings&) const
{
    return visitImpl(*this, visitor);
}

wxString PluginEffectAdapter::labelForKey(const wxString& key) const
{
    for (const Param& p : m_params) {
        if (p.wxKey == key) {
            return p.label;
        }
    }
    return {};
}

namespace {
//! One selected track presented to the plugin
struct TrackSegment {
    WaveTrack* track = nullptr;
    double curT0 = 0.0;   // this track's selected region, absolute seconds
    double curT1 = 0.0;
    double rate = 0.0;
    std::string nameUtf8;
    // storage backing the last read on this track index
    std::vector<std::vector<float>> channelData;
    std::vector<float*> channelPtrs;
};

struct PendingLabel {
    std::string trackName;
    double startSec = 0.0;
    double endSec = 0.0;
    std::string text;
};

struct ProcessState {
    EffectOutputTracks* outputs = nullptr;
    double t0 = 0.0;   // selection, absolute seconds
    double t1 = 0.0;
    bool isGenerator = false;
    std::function<bool (double, const char*)> progress;
    std::vector<TrackSegment> segments;

    // Labels may arrive from plugin worker threads, but tracks have thread
    // affinity; buffer them and materialize on the process thread afterwards.
    std::mutex labelMutex;
    std::vector<PendingLabel> pendingLabels;
};

uint32_t ctx_track_count(void* c)
{
    return static_cast<uint32_t>(static_cast<ProcessState*>(c)->segments.size());
}

bool ctx_track_info(void* c, uint32_t index, auplug_track_info_t* out)
{
    auto* st = static_cast<ProcessState*>(c);
    if (index >= st->segments.size() || !out) {
        return false;
    }
    const TrackSegment& seg = st->segments[index];

    out->name = seg.nameUtf8.c_str();
    out->channel_count = static_cast<uint32_t>(seg.track->Channels().size());
    out->sample_rate = seg.rate;
    out->sel_start_sec = seg.curT0 - st->t0;
    out->sel_end_sec = seg.curT1 - st->t0;
    out->track_start_sec = seg.track->GetStartTime() - st->t0;
    out->track_end_sec = seg.track->GetEndTime() - st->t0;
    return true;
}

bool readRange(TrackSegment& seg, double startAbs, double endAbs, double desiredRate, auplug_audio_buffer_t* out)
{
    startAbs = std::max(startAbs, seg.track->GetStartTime());
    endAbs = std::min(endAbs, seg.track->GetEndTime());
    if (endAbs <= startAbs || !out) {
        return false;
    }

    const sampleCount start = seg.track->TimeToLongSamples(startAbs);
    const size_t len = (seg.track->TimeToLongSamples(endAbs) - start).as_size_t();
    const size_t nch = seg.track->Channels().size();

    std::vector<std::vector<float>> data(nch);
    std::vector<const float*> ptrs(nch);
    size_t ci = 0;
    for (const auto& channel : seg.track->Channels()) {
        data[ci].resize(len);
        if (!channel->GetFloats(data[ci].data(), start, len)) {
            return false;
        }
        ptrs[ci] = data[ci].data();
        ++ci;
    }

    if (desiredRate > 0 && desiredRate != seg.rate) {
        seg.channelData = au::pluginhost::hostdsp::resample(ptrs.data(), nch, len, seg.rate, desiredRate);
        out->sample_rate = desiredRate;
    } else {
        seg.channelData = std::move(data);
        out->sample_rate = seg.rate;
    }

    seg.channelPtrs.resize(nch);
    for (size_t i = 0; i < nch; ++i) {
        seg.channelPtrs[i] = seg.channelData[i].data();
    }

    out->channel_count = static_cast<uint32_t>(nch);
    out->frame_count = seg.channelData.empty() ? 0 : seg.channelData[0].size();
    out->channels = seg.channelPtrs.data();
    return true;
}

bool ctx_read_track(void* c, uint32_t index, double desiredRate, auplug_audio_buffer_t* out)
{
    auto* st = static_cast<ProcessState*>(c);
    if (index >= st->segments.size()) {
        return false;
    }
    TrackSegment& seg = st->segments[index];
    return readRange(seg, seg.curT0, seg.curT1, desiredRate, out);
}

bool ctx_read_track_range(void* c, uint32_t index, double startSec, double endSec, double desiredRate,
                          auplug_audio_buffer_t* out)
{
    auto* st = static_cast<ProcessState*>(c);
    if (index >= st->segments.size()) {
        return false;
    }
    return readRange(st->segments[index], st->t0 + startSec, st->t0 + endSec, desiredRate, out);
}

//! Maps the plugin's channels onto the given count: truncates extra channels,
//! averaging them all into one when the target is mono
std::vector<std::vector<float>> adaptChannels(const auplug_audio_buffer_t* audio, size_t targetCh)
{
    const size_t frames = static_cast<size_t>(audio->frame_count);
    std::vector<std::vector<float>> chans(targetCh);

    if (targetCh == 1 && audio->channel_count > 1) {
        chans[0].resize(frames);
        for (uint32_t c = 0; c < audio->channel_count; ++c) {
            const float* src = audio->channels[c];
            for (size_t i = 0; i < frames; ++i) {
                chans[0][i] += src[i];
            }
        }
        const float scale = 1.0f / audio->channel_count;
        for (size_t i = 0; i < frames; ++i) {
            chans[0][i] *= scale;
        }
    } else {
        for (size_t c = 0; c < targetCh; ++c) {
            const float* src = audio->channels[std::min<size_t>(c, audio->channel_count - 1)];
            chans[c].assign(src, src + frames);
        }
    }
    return chans;
}

//! Builds a flushed temporary track (an EmptyCopy of 'base') holding the
//! given channel data at the given rate
WaveTrack::Holder makeTempTrack(const WaveTrack& base, const std::vector<std::vector<float>>& chans, double rate)
{
    auto tmp = base.EmptyCopy(chans.size());
    tmp->SetRate(rate);
    size_t ci = 0;
    for (const auto& channel : tmp->Channels()) {
        channel->Append(reinterpret_cast<constSamplePtr>(chans[ci].data()), floatSample, chans[ci].size());
        ++ci;
    }
    tmp->Flush();
    return tmp;
}

bool ctx_write_track(void* c, uint32_t index, const auplug_audio_buffer_t* audio)
{
    auto* st = static_cast<ProcessState*>(c);
    if (index >= st->segments.size() || !audio || !audio->channels
        || audio->channel_count == 0 || audio->frame_count == 0) {
        return false;
    }
    TrackSegment& seg = st->segments[index];

    try {
        const size_t trackCh = seg.track->Channels().size();
        const size_t outCh = std::min<size_t>(audio->channel_count, trackCh);
        std::vector<std::vector<float>> chans = adaptChannels(audio, outCh);

        // An empty track adopts the plugin's rate (keeps generated audio
        // unconverted); otherwise the output is resampled to the track rate.
        double targetRate = seg.rate;
        if (audio->sample_rate != seg.rate) {
            if (seg.track->IsEmpty(seg.track->GetStartTime(), seg.track->GetEndTime())) {
                seg.track->SetRate(audio->sample_rate);
                seg.rate = audio->sample_rate;
                targetRate = audio->sample_rate;
            } else {
                std::vector<const float*> ptrs(chans.size());
                for (size_t i = 0; i < chans.size(); ++i) {
                    ptrs[i] = chans[i].data();
                }
                chans = au::pluginhost::hostdsp::resample(ptrs.data(), chans.size(), chans[0].size(), audio->sample_rate, targetRate);
            }
        }

        const auto tmp = makeTempTrack(*seg.track, chans, targetRate);

        if (st->isGenerator) {
            const double outDur = chans[0].size() / targetRate;
            const PasteTimeWarper warper { st->t1, st->t0 + outDur };
            seg.track->ClearAndPaste(st->t0, st->t1, *tmp, true, false, &warper);
        } else {
            seg.track->ClearAndPaste(seg.curT0, seg.curT1, *tmp);
        }
        return true;
    } catch (const std::exception& e) {
        LOGE() << "write_track failed: " << e.what();
        return false;
    }
}

bool ctx_add_audio_track(void* c, const char* name, double startSec, const auplug_audio_buffer_t* audio)
{
    auto* st = static_cast<ProcessState*>(c);
    if (st->segments.empty() || !audio || !audio->channels
        || audio->channel_count == 0 || audio->frame_count == 0 || audio->sample_rate <= 0) {
        return false;
    }

    try {
        const WaveTrack& base = *st->segments[0].track;
        const size_t nch = std::min<uint32_t>(audio->channel_count, 2);
        const std::vector<std::vector<float>> chans = adaptChannels(audio, nch);

        const auto tmp = makeTempTrack(base, chans, audio->sample_rate);

        auto out = base.EmptyCopy(nch);
        out->SetRate(audio->sample_rate);
        out->SetName(wxString::FromUTF8(name ? name : ""));

        const double startAbs = std::max(0.0, st->t0 + startSec);
        const double dur = chans[0].size() / audio->sample_rate;
        out->ClearAndPaste(startAbs, startAbs + dur, *tmp);

        st->outputs->AddToOutputTracks(std::move(out));
        return true;
    } catch (const std::exception& e) {
        LOGE() << "add_audio_track failed: " << e.what();
        return false;
    }
}

bool ctx_add_label(void* c, const char* trackName, double startSec, double endSec, const char* text)
{
    auto* st = static_cast<ProcessState*>(c);
    if (!trackName || !text) {
        return false;
    }

    std::lock_guard<std::mutex> guard(st->labelMutex);
    st->pendingLabels.push_back({ trackName, startSec, endSec, text });
    return true;
}

bool ctx_progress(void* c, double fraction, const char* message)
{
    auto* st = static_cast<ProcessState*>(c);
    return st->progress(fraction, message);
}
}

bool PluginEffectAdapter::Process(EffectInstance&, EffectSettings& settings)
{
    if (!m_fx) {
        return false;
    }

    const auto* processExt
        = static_cast<const auplug_process_offline_ext_t*>(m_fx->get_extension(m_fx, AUPLUG_PROCESS_OFFLINE_EXT_ID));
    if (!processExt) {
        LOGE() << "effect has no offline processing capability: " << m_desc.id;
        return false;
    }

    pushParams();

    EffectOutputTracks outputs { *mTracks, GetType(), { { mT0, mT1 } } };

    ProcessState state;
    state.outputs = &outputs;
    state.t0 = mT0;
    state.t1 = mT1;
    state.isGenerator = GetType() == EffectTypeGenerate;
    state.progress = [this](double fraction, const char* message) {
        const TranslatableString msg = message ? Verbatim(wxString::FromUTF8(message)) : XO("Processing...");
        const bool cancelled = TotalProgress(std::clamp(fraction, 0.0, 1.0), msg);
        return !cancelled;
    };

    for (auto* track : outputs.Get().Selected<WaveTrack>()) {
        TrackSegment seg;
        seg.track = track;
        if (state.isGenerator) {
            // generators may run with an empty selection (insert at cursor)
            seg.curT0 = mT0;
            seg.curT1 = mT1;
        } else {
            seg.curT0 = std::max(track->GetStartTime(), mT0);
            seg.curT1 = std::min(track->GetEndTime(), mT1);
            if (seg.curT1 <= seg.curT0) {
                continue;
            }
        }
        seg.rate = track->GetRate();
        seg.nameUtf8 = track->GetName().ToUTF8().data();
        state.segments.push_back(std::move(seg));
    }

    auplug_process_ctx_t pctx {};
    pctx.struct_size = sizeof(pctx);
    pctx.ctx = &state;
    pctx.duration_sec = state.isGenerator ? settings.extra.GetDuration() : (mT1 - mT0);
    pctx.track_count = ctx_track_count;
    pctx.track_info = ctx_track_info;
    pctx.read_track = ctx_read_track;
    pctx.read_track_range = ctx_read_track_range;
    pctx.write_track = ctx_write_track;
    pctx.add_audio_track = ctx_add_audio_track;
    pctx.add_label = ctx_add_label;
    pctx.progress = ctx_progress;

    const int32_t result = processExt->process(m_fx, &pctx);

    if (result == AUPLUG_PROCESS_OK) {
        outputs.Commit();

        std::map<std::string, std::shared_ptr<AddedAnalysisTrack> > labelTracks;
        for (const PendingLabel& label : state.pendingLabels) {
            auto& slot = labelTracks[label.trackName];
            if (!slot) {
                slot = AddAnalysisTrack(*this, wxString::FromUTF8(label.trackName));
            }
            slot->get()->AddLabel(SelectedRegion(mT0 + label.startSec, mT0 + label.endSec),
                                  wxString::FromUTF8(label.text));
        }
        for (auto& [name, addedTrack] : labelTracks) {
            addedTrack->Commit();
        }
        return true;
    }

    if (result == AUPLUG_PROCESS_ERROR) {
        const char* err = processExt->last_error ? processExt->last_error(m_fx) : nullptr;
        LOGE() << "plugin effect failed: " << m_desc.id << ", error: " << (err ? err : "unknown");
        BasicUI::ShowMessageBox(Verbatim(wxString::FromUTF8(err ? err : "The effect failed.")));
    }
    return false;
}
