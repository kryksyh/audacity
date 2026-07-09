/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>
#include <QQmlComponent>
#include <QQuickItem>

#include "global/async/asyncable.h"
#include "modularity/ioc.h"

#include "effects/builtin/ibuiltineffectsviewregister.h"
#include "effects/effects_base/ieffectsuiengine.h"
#include "effects/effects_base/ieffectinstancesregister.h"
#include "effects/effects_base/ieffectsprovider.h"
#include "effects/effects_base/iplugineffectviews.h"

namespace au::effects {
class BuiltinEffectViewLoader : public QObject, public muse::async::Asyncable, muse::Contextable
{
    Q_OBJECT

    Q_PROPERTY(QQuickItem * contentItem READ contentItem NOTIFY contentItemChanged FINAL)

    muse::GlobalInject<IBuiltinEffectsViewRegister> viewRegister;
    muse::GlobalInject<IEffectInstancesRegister> instancesRegister;
    muse::GlobalInject<IEffectsProvider> effectsProvider;
    muse::GlobalInject<IPluginEffectViews> moduleViews;

    muse::ContextInject<IEffectsUiEngine> engine { this };

public:
    BuiltinEffectViewLoader(QObject* parent = nullptr);
    ~BuiltinEffectViewLoader() override;

    QQuickItem* contentItem() const;

    Q_INVOKABLE void load(int instanceId, QObject* itemParent, QObject* dialogView, bool usedDestructively);

private:
    //! Instantiates a module-shipped QML view from source text, with the
    //! `effect` context object bound to this instance
    QObject* createModuleView(const muse::String& type, const QString& url, int instanceId, QObject* itemParent);

signals:
    void titleChanged();
    void contentItemChanged();

    void closeRequested();

private:
    QQuickItem* m_contentItem = nullptr;
};
}
