/*
* Audacity: A Digital Audio Editor
*/
#include "plugin.h"

#include <cstring>
#include <string>

#include <wx/log.h>

#include "au3-files/FileNames.h"

#include "hostdsp.h"

#include "framework/global/log.h"

using namespace au::pluginhost;

namespace {
void host_log(void*, int32_t level, const char* msg)
{
    switch (level) {
    case AUPLUG_LOG_DEBUG: LOGD() << msg;
        break;
    case AUPLUG_LOG_INFO: LOGI() << msg;
        break;
    case AUPLUG_LOG_WARN: LOGW() << msg;
        break;
    default: LOGE() << msg;
        break;
    }
}

const char* host_data_dir(void*)
{
    // magic static: callable from any thread
    static const std::string dir = FileNames::DataDir().ToUTF8().data();
    return dir.c_str();
}

const void* host_get_extension(void*, const char* id)
{
    if (!id) {
        return nullptr;
    }
    if (std::strcmp(id, AUPLUG_DSP_EXT_ID) == 0) {
        return hostdsp::hostDspExt();
    }
    return nullptr;
}

const auplug_host_t s_host = {
    sizeof(auplug_host_t),
    AUPLUG_API_VERSION,
    nullptr,
    host_log,
    host_data_dir,
    host_get_extension,
};
}

std::unique_ptr<Plugin> Plugin::tryLoad(const muse::io::path_t& path)
{
    auto plugin = std::unique_ptr<Plugin>(new Plugin());
    plugin->m_path = path;

    {
        // Quietly probe: whether a non-ABI library is an error is the
        // caller's call.
        wxLogNull suppressErrors;
        if (!plugin->m_lib.Load(wxString::FromUTF8(path.toStdString()), wxDL_NOW | wxDL_VERBATIM | wxDL_QUIET)) {
            return nullptr;
        }
        if (!plugin->m_lib.HasSymbol(wxT(AUPLUG_ENTRY_SYMBOL))) {
            return nullptr;
        }
    }

    auto* entry = static_cast<const auplug_entry_t*>(plugin->m_lib.GetSymbol(wxT(AUPLUG_ENTRY_SYMBOL)));
    if (!entry) {
        return nullptr;
    }

    if (entry->api_version > AUPLUG_API_VERSION) {
        LOGE() << "plugin requires a newer host (plugin API " << entry->api_version
               << ", host API " << AUPLUG_API_VERSION << "): " << path;
        return nullptr;
    }

    if (!entry->desc || !entry->desc->id || !entry->desc->name || !entry->desc->vendor || !entry->desc->version
        || !entry->init || !entry->get_factory) {
        LOGE() << "plugin entry is incomplete: " << path;
        return nullptr;
    }

    if (!entry->init(&s_host)) {
        LOGE() << "plugin refused to initialize (host too old?): " << path;
        return nullptr;
    }

    plugin->m_entry = entry;
    plugin->m_inited = true;

    LOGI() << "plugin handshake ok: " << entry->desc->id << " v" << entry->desc->version
           << " (API " << entry->api_version << "): " << path;

    return plugin;
}

Plugin::~Plugin()
{
    if (m_inited && m_entry && m_entry->deinit) {
        m_entry->deinit();
    }
    // Never unload: effect adapters held elsewhere may still call
    // fx->destroy during shutdown, in unspecified teardown order.
    m_lib.Detach();
}

uint32_t Plugin::apiVersion() const
{
    return m_entry->api_version;
}

const auplug_effect_factory_t* Plugin::effectFactory() const
{
    return static_cast<const auplug_effect_factory_t*>(m_entry->get_factory(AUPLUG_EFFECT_FACTORY_ID));
}

const auplug_importer_factory_t* Plugin::importerFactory() const
{
    return static_cast<const auplug_importer_factory_t*>(m_entry->get_factory(AUPLUG_IMPORTER_FACTORY_ID));
}

const auplug_exporter_factory_t* Plugin::exporterFactory() const
{
    return static_cast<const auplug_exporter_factory_t*>(m_entry->get_factory(AUPLUG_EXPORTER_FACTORY_ID));
}

const auplug_views_t* Plugin::views() const
{
    return static_cast<const auplug_views_t*>(m_entry->get_factory(AUPLUG_VIEWS_ID));
}
