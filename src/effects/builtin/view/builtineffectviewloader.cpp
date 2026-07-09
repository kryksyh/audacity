/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2024 MuseScore BVBA and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "builtineffectviewloader.h"
#include "effects/effects_base/effectstypes.h"
#include "effects/effects_base/internal/abstractviewlauncher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlEngine>
#include <QQmlContext>

#include "au3wrap/internal/wxtypes_convert.h"
#include "global/types/number.h"
#include "au3-effects/EffectManager.h"
#include "au3-effects/Effect.h"

#include "plugineffectviewbridge.h"

#include "log.h"

using namespace au::effects;

BuiltinEffectViewLoader::BuiltinEffectViewLoader(QObject* parent)
    : QObject(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

BuiltinEffectViewLoader::~BuiltinEffectViewLoader()
{
    if (m_contentItem) {
        m_contentItem->deleteLater();
        m_contentItem = nullptr;
    }
}

void BuiltinEffectViewLoader::load(int instanceId, QObject* itemParent, QObject* dialogView, bool usedDestructively)
{
    // TODO: could instancesRegister have a `typeByInstanceId` method?
    const auto effectId = instancesRegister()->effectIdByInstanceId(instanceId).toStdString();

    const Effect* const effect = effectsProvider()->effect(muse::String::fromStdString(effectId));
    IF_ASSERT_FAILED(effect) {
        LOGE() << "effect not available, instanceId: " << instanceId << ", effectId: " << effectId;
        return;
    }

    const muse::String type = au3::wxToString(effect->GetSymbol().Internal());

    QString url = viewRegister()->viewUrl(type);
    if (url.isEmpty()) {
        LOGE() << "Not found view for type: " << type;
        return;
    }
    LOGD() << "found view for type: " << type << ", url: " << url;

    QObject* obj = nullptr;
    if (url.startsWith(QLatin1String("auplug-view://"))) {
        obj = createModuleView(type, url, instanceId, itemParent);
        if (!obj) {
            return;
        }
    } else {
        QQmlEngine* qmlEngine = engine()->qmlEngine();

        //! NOTE We create extension UI using a separate engine to control what we provide,
        //! making it easier to maintain backward compatibility and stability.
        QQmlComponent component = QQmlComponent(qmlEngine, url);
        if (!component.isReady()) {
            LOGE() << "Failed to load QML file: " << url;
            LOGE() << component.errorString();
            return;
        }

        QQmlContext* ctx = qmlContext(itemParent);

        obj = component.createWithInitialProperties(
        {
            { "parent", QVariant::fromValue(itemParent) },
            { "instanceId", instanceId },
            { "dialogView", QVariant::fromValue(dialogView) },
            { "usedDestructively", usedDestructively }
        }, ctx);
    }

    m_contentItem = qobject_cast<QQuickItem*>(obj);
    if (!m_contentItem) {
        LOGE() << "Component not QuickItem, file: " << url;
    }

    if (m_contentItem) {
        if (muse::is_zero(m_contentItem->implicitHeight())) {
            m_contentItem->setImplicitHeight(m_contentItem->height());
            if (muse::is_zero(m_contentItem->implicitHeight())) {
                m_contentItem->setImplicitHeight(450);
            }
        }

        if (muse::is_zero(m_contentItem->implicitWidth())) {
            m_contentItem->setImplicitWidth(m_contentItem->width());
            if (muse::is_zero(m_contentItem->implicitWidth())) {
                m_contentItem->setImplicitWidth(200);
            }
        }
    }

    emit contentItemChanged();
}

namespace {
//! Host-side wrapper satisfying the viewer contract (title, isApplyAllowed,
//! init() etc.) on behalf of module views: module QML must not depend on
//! application-internal interfaces.
const char* MODULE_VIEW_SHIM_QML = R"qml(
import QtQuick

Item {
    property string title: ""
    property bool isApplyAllowed: true
    property bool isPreviewing: false
    property bool usesPresets: false
    property int numNavigationPanels: 0

    function init() {}
    function deinit() {}
    function manage(parent) {}
    function startPreview() {}
    function stopPreview() {}

    implicitWidth: children.length > 0 ? children[0].implicitWidth : 300
    implicitHeight: children.length > 0 ? children[0].implicitHeight : 200
}
)qml";

bool waitComponentReady(QQmlComponent& component)
{
    // setData compiles synchronously, but the first use of a not-yet-loaded
    // import module can leave the component in Loading state briefly.
    // Exclude user input (no re-entrant dialog close) and bound the wait.
    QElapsedTimer timer;
    timer.start();
    while (component.isLoading() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    }
    return component.isReady();
}
}

QObject* BuiltinEffectViewLoader::createModuleView(const muse::String& type, const QString& url, int instanceId,
                                                   QObject* itemParent)
{
    const muse::String qml = moduleViews()->effectViewQml(type);
    if (qml.empty()) {
        LOGE() << "no module view QML for type: " << type;
        return nullptr;
    }

    QQmlEngine* qmlEngine = engine()->qmlEngine();

    QQmlComponent component(qmlEngine);
    component.setData(qml.toQString().toUtf8(), QUrl(url));
    if (!waitComponentReady(component)) {
        LOGE() << "Failed to load module view QML for type: " << type;
        LOGE() << component.errorString();
        return nullptr;
    }

    QQmlComponent shimComponent(qmlEngine);
    shimComponent.setData(MODULE_VIEW_SHIM_QML, QUrl(QStringLiteral("auplug-view://shim")));
    if (!waitComponentReady(shimComponent)) {
        LOGE() << "Failed to load module view shim";
        LOGE() << shimComponent.errorString();
        return nullptr;
    }

    //! NOTE The view gets its own context with the `effect` object bound to
    //! this effect instance; the QML is module-shipped source text and only
    //! talks to the host through that object.
    auto* ctx = new QQmlContext(qmlEngine->rootContext(), this);
    auto* bridge = new PluginEffectViewBridge(instanceId, ctx);
    ctx->setContextProperty("effect", bridge);

    QObject* shim = shimComponent.createWithInitialProperties(
    {
        { "parent", QVariant::fromValue(itemParent) }
    }, ctx);
    if (!shim) {
        ctx->deleteLater();
        return nullptr;
    }

    // the context (and bridge) must live exactly as long as the view objects
    ctx->setParent(shim);

    QObject* view = component.createWithInitialProperties(
    {
        { "parent", QVariant::fromValue(shim) }
    }, ctx);
    if (!view) {
        LOGE() << "Failed to instantiate module view for type: " << type;
        shim->deleteLater();
        return nullptr;
    }
    view->setParent(shim);

    return shim;
}

QQuickItem* BuiltinEffectViewLoader::contentItem() const
{
    return m_contentItem;
}
