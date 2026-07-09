/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QtQml/qqmlregistration.h>
#include <QQuickItem>

#include "modularity/ioc.h"
#include "pluginhost/ipluginhostservice.h"

namespace au::appshell {
//! Instantiates a plugin's AUPLUG_VIEW_PLUGIN_CONFIG QML (source text shipped
//! by the plugin, found by matching pluginId) as a child item. Unlike effect
//! views, no host context object is bound: there is no per-effect-instance
//! parameter model to expose at plugin scope.
class PluginConfigViewLoader : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString pluginId READ pluginId WRITE setPluginId NOTIFY pluginIdChanged FINAL)

    muse::GlobalInject<au::pluginhost::IPluginHostService> pluginHostService;

public:
    explicit PluginConfigViewLoader(QQuickItem* parent = nullptr);

    QString pluginId() const;
    void setPluginId(const QString& id);

signals:
    void pluginIdChanged();

private:
    void reload();

    QString m_pluginId;
    QObject* m_content = nullptr;
};
}
