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
class SelectionContextMenuModel : public muse::uicomponents::AbstractMenuModel
{
    Q_OBJECT

    QML_ELEMENT

public:
    Q_INVOKABLE void load() override;

private:

    muse::uicomponents::MenuItemList makeItems();
};
}
