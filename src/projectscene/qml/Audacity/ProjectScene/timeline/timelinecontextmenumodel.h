#pragma once
#pragma once
/**********************************************************************

  Audacity: A Digital Audio Editor

**********************************************************************/

#pragma once

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"

#include "uicomponents/qml/Muse/UiComponents/abstractmenumodel.h"
#include <QtQml>

namespace au::projectscene {
class TimelineContextMenuModel : public muse::uicomponents::AbstractMenuModel
{
    Q_OBJECT

    QML_ELEMENT

    muse::Inject<context::IGlobalContext> globalContext { this };

public:
    Q_INVOKABLE void load() override;

private:

    muse::uicomponents::MenuItemList makeRulerItems();
};
}
