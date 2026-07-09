/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <memory>

#include <wx/dynlib.h>

#include "pluginhost/api/audacity_plugin.h"

#include "framework/global/io/path.h"

namespace au::pluginhost {
//! A loaded stable-ABI plugin: owns the library handle and the negotiated
//! entry for its lifetime.
class Plugin
{
public:
    //! nullptr if the library has no ABI entry or the handshake fails.
    static std::unique_ptr<Plugin> tryLoad(const muse::io::path_t& path);

    ~Plugin();

    const auplug_desc_t& desc() const { return *m_entry->desc; }
    const muse::io::path_t& path() const { return m_path; }

    //! The API version the plugin was built against (<= the host's)
    uint32_t apiVersion() const;

    const auplug_effect_factory_t* effectFactory() const;
    const auplug_importer_factory_t* importerFactory() const;
    const auplug_exporter_factory_t* exporterFactory() const;
    const auplug_views_t* views() const;

private:
    Plugin() = default;

    wxDynamicLibrary m_lib;
    const auplug_entry_t* m_entry = nullptr;
    muse::io::path_t m_path;
    bool m_inited = false;
};
}
