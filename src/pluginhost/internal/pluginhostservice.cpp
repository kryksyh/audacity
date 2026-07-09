/*
* Audacity: A Digital Audio Editor
*/
#include "pluginhostservice.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "pluginhost/plugin.h"

#include "framework/global/io/dir.h"
#include "framework/global/log.h"

using namespace au::pluginhost;

namespace {
std::vector<std::string> moduleFileFilters()
{
#if defined(__APPLE__)
    return { "*.dylib" };
#elif defined(_WIN32)
    return { "*.dll" };
#else
    return { "*.so" };
#endif
}
}

PluginHostService::~PluginHostService() = default;

const std::vector<Plugin*>& PluginHostService::plugins() const
{
    ensureLoaded();
    return m_pluginPtrs;
}

const std::vector<PluginView>& PluginHostService::views() const
{
    ensureLoaded();
    return m_views;
}

void PluginHostService::ensureLoaded() const
{
    if (!m_loaded) {
        const_cast<PluginHostService*>(this)->loadModules();
    }
}

void PluginHostService::loadModules()
{
    m_loaded = true;

    muse::io::paths_t dirs {
        globalConfiguration()->userAppDataPath() + "/plugins",
        globalConfiguration()->appDataPath() + "/plugins",
    };

    if (const char* env = std::getenv("AUDACITY_PLUGINS_PATH")) {
        dirs.push_back(muse::io::path_t(env));
    }

    for (const muse::io::path_t& dir : dirs) {
        muse::RetVal<muse::io::paths_t> files
            = muse::io::Dir::scanFiles(dir, moduleFileFilters(), muse::io::ScanMode::FilesInCurrentDir);
        if (!files.ret) {
            continue;
        }
        for (const muse::io::path_t& file : files.val) {
            std::unique_ptr<Plugin> plugin = Plugin::tryLoad(file);
            if (!plugin) {
                LOGW() << "not a plugin ABI library, ignoring: " << file;
                continue;
            }

            const std::string id = plugin->desc().id;
            const bool duplicate = std::any_of(m_plugins.begin(), m_plugins.end(), [&](const auto& m) {
                return id == m->desc().id;
            });
            if (duplicate) {
                // the user dir is scanned first and wins
                LOGW() << "plugin " << id << " already loaded, ignoring: " << file;
                continue;
            }

            if (const auplug_views_t* views = plugin->views()) {
                const uint32_t count = views->count();
                for (uint32_t i = 0; i < count; ++i) {
                    auplug_view_desc_t view;
                    view.struct_size = sizeof(view);
                    if (!views->get_view(i, &view) || !view.qml) {
                        continue;
                    }
                    m_views.push_back({ view.role,
                                        view.ref ? muse::String::fromUtf8(view.ref) : muse::String(),
                                        muse::String::fromUtf8(view.qml),
                                        muse::String::fromUtf8(plugin->desc().id),
                                        muse::String::fromUtf8(plugin->desc().name) });
                }
            }

            m_pluginPtrs.push_back(plugin.get());
            m_plugins.push_back(std::move(plugin));
        }
    }

    LOGI() << "loaded plugins: " << m_plugins.size();
}
