/*
* Audacity: A Digital Audio Editor
*/
#include "pluginconfigviewloader.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>

#include "pluginhost/api/audacity_plugin.h"

#include "framework/global/log.h"

using namespace au::appshell;

namespace {
//! Module-shipped QML can transiently be "Loading" the first time one of
//! its imports is used; exclude user input and bound the wait.
bool waitComponentReady(QQmlComponent& component)
{
    QElapsedTimer timer;
    timer.start();
    while (component.isLoading() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    return component.isReady();
}
}

PluginConfigViewLoader::PluginConfigViewLoader(QQuickItem* parent)
    : QQuickItem(parent)
{
}

QString PluginConfigViewLoader::pluginId() const
{
    return m_pluginId;
}

void PluginConfigViewLoader::setPluginId(const QString& id)
{
    if (m_pluginId == id) {
        return;
    }
    m_pluginId = id;
    emit pluginIdChanged();
    reload();
}

void PluginConfigViewLoader::reload()
{
    if (m_content) {
        m_content->deleteLater();
        m_content = nullptr;
    }
    if (m_pluginId.isEmpty()) {
        return;
    }

    const muse::String pluginId = muse::String::fromQString(m_pluginId);
    QString qml;
    for (const au::pluginhost::PluginView& view : pluginHostService()->views()) {
        if (view.role == AUPLUG_VIEW_PLUGIN_CONFIG && view.pluginId == pluginId) {
            qml = view.qml.toQString();
            break;
        }
    }
    if (qml.isEmpty()) {
        LOGW() << "no plugin config view for plugin: " << m_pluginId;
        return;
    }

    QQmlEngine* engine = qmlEngine(this);
    if (!engine) {
        LOGE() << "no QML engine available for plugin config view: " << m_pluginId;
        return;
    }

    QQmlComponent component(engine);
    component.setData(qml.toUtf8(), QUrl(QStringLiteral("auplug-config://") + m_pluginId));
    if (!waitComponentReady(component)) {
        LOGE() << "failed to load plugin config view for " << m_pluginId << ": " << component.errorString();
        return;
    }

    auto* ctx = new QQmlContext(engine->rootContext(), this);
    m_content = component.createWithInitialProperties({}, ctx);
    if (!m_content) {
        LOGE() << "failed to instantiate plugin config view for " << m_pluginId << ": " << component.errorString();
        ctx->deleteLater();
        return;
    }
    ctx->setParent(m_content);

    if (auto* contentItem = qobject_cast<QQuickItem*>(m_content)) {
        contentItem->setParentItem(this);
        contentItem->setPosition(QPointF(0, 0));
        contentItem->setWidth(width());
        contentItem->setHeight(height());
        connect(this, &QQuickItem::widthChanged, contentItem, [this, contentItem]() { contentItem->setWidth(width()); });
        connect(this, &QQuickItem::heightChanged, contentItem, [this, contentItem]() { contentItem->setHeight(height()); });
        setImplicitWidth(contentItem->implicitWidth() > 0 ? contentItem->implicitWidth() : 400);
        setImplicitHeight(contentItem->implicitHeight() > 0 ? contentItem->implicitHeight() : 300);
    }
}
