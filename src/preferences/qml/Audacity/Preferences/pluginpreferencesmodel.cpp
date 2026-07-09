/*
 * Audacity: A Digital Audio Editor
 */
#include "pluginpreferencesmodel.h"

#include "io/dir.h"
#include "pluginhost/api/audacity_plugin.h"

namespace au::appshell {
static QStringList toQStringList(const muse::io::paths_t& paths)
{
    QStringList result;
    result.reserve(static_cast<int>(paths.size()));
    for (const auto& p : paths) {
        result.push_back(p.toQString());
    }
    return result;
}

static muse::io::paths_t toPathsT(const QStringList& paths)
{
    muse::io::paths_t result;
    result.reserve(static_cast<size_t>(paths.size()));
    for (const auto& p : paths) {
        result.push_back(muse::io::path_t(p));
    }
    return result;
}

PluginPreferencesModel::PluginPreferencesModel(QObject* parent)
    : QObject(parent)
{
}

void PluginPreferencesModel::init()
{
    effectsConfiguration()->effectMenuOrganizationChanged().onNotify(this, [this] {
        emit effectMenuOrganizationChanged();
    });

    effectsConfiguration()->lv2CustomPathsChanged().onNotify(this, [this] {
        emit lv2CustomPathsChanged();
    });

    effectsConfiguration()->vst3CustomPathsChanged().onNotify(this, [this] {
        emit vst3CustomPathsChanged();
    });
}

au::effects::EffectMenuOrganization PluginPreferencesModel::effectMenuOrganization() const
{
    return effectsConfiguration()->effectMenuOrganization();
}

void PluginPreferencesModel::setEffectMenuOrganization(effects::EffectMenuOrganization organization)
{
    if (effectMenuOrganization() == organization) {
        return;
    }

    effectsConfiguration()->setEffectMenuOrganization(organization);
}

QStringList PluginPreferencesModel::lv2CustomPaths() const
{
    return toQStringList(effectsConfiguration()->lv2CustomPaths());
}

QStringList PluginPreferencesModel::vst3CustomPaths() const
{
    return toQStringList(effectsConfiguration()->vst3CustomPaths());
}

void PluginPreferencesModel::addLv2Path()
{
    QStringList paths = lv2CustomPaths();
    paths.append(QString());
    effectsConfiguration()->setLv2CustomPaths(toPathsT(paths));
    emit lv2CustomPathsChanged();
}

void PluginPreferencesModel::setLv2Path(int index, const QString& path)
{
    QStringList paths = lv2CustomPaths();
    if (index < 0 || index >= paths.size()) {
        return;
    }
    paths[index] = path;
    effectsConfiguration()->setLv2CustomPaths(toPathsT(paths));
}

void PluginPreferencesModel::removeLv2Path(int index)
{
    QStringList paths = lv2CustomPaths();
    if (index < 0 || index >= paths.size()) {
        return;
    }
    paths.removeAt(index);
    effectsConfiguration()->setLv2CustomPaths(toPathsT(paths));
    emit lv2CustomPathsChanged();
}

void PluginPreferencesModel::addVst3Path()
{
    QStringList paths = vst3CustomPaths();
    paths.append(QString());
    effectsConfiguration()->setVst3CustomPaths(toPathsT(paths));
    emit vst3CustomPathsChanged();
}

void PluginPreferencesModel::setVst3Path(int index, const QString& path)
{
    QStringList paths = vst3CustomPaths();
    if (index < 0 || index >= paths.size()) {
        return;
    }
    paths[index] = path;
    effectsConfiguration()->setVst3CustomPaths(toPathsT(paths));
}

void PluginPreferencesModel::removeVst3Path(int index)
{
    QStringList paths = vst3CustomPaths();
    if (index < 0 || index >= paths.size()) {
        return;
    }
    paths.removeAt(index);
    effectsConfiguration()->setVst3CustomPaths(toPathsT(paths));
    emit vst3CustomPathsChanged();
}

bool PluginPreferencesModel::pathExists(const QString& path) const
{
    if (path.isEmpty()) {
        return true; // empty path represents an unfilled, freshly-added row
    }
    return muse::io::Dir(muse::io::path_t(path)).exists();
}

bool PluginPreferencesModel::lv2Supported() const
{
    // `EffectFamily::LV2` is only declared in the platform-gated enum on
    // Linux (see `effects::EffectFamilies::EffectFamily`). On other
    // platforms there is nothing to ask the provider about.
#ifdef Q_OS_LINUX
    return effectsProvider()->hasEffectFamily(effects::EffectFamily::LV2);
#else
    return false;
#endif
}

bool PluginPreferencesModel::vst3Supported() const
{
    return effectsProvider()->hasEffectFamily(effects::EffectFamily::VST3);
}

QVariantList PluginPreferencesModel::pluginConfigViews() const
{
    QVariantList result;
    for (const au::pluginhost::PluginView& view : pluginHostService()->views()) {
        if (view.role != AUPLUG_VIEW_PLUGIN_CONFIG) {
            continue;
        }
        QVariantMap item;
        item["id"] = view.pluginId.toQString();
        item["name"] = view.pluginName.toQString();
        result << item;
    }
    return result;
}
}
