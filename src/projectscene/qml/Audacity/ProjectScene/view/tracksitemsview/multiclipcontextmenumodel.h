/**********************************************************************

  Audacity: A Digital Audio Editor

**********************************************************************/

#pragma once

#include <QtQml/qqmlregistration.h>

#include "uicomponents/qml/Muse/UiComponents/abstractmenumodel.h"

namespace au::projectscene {
class MultiClipContextMenuModel : public muse::uicomponents::AbstractMenuModel
{
    Q_OBJECT
    QML_ELEMENT

public:
    Q_INVOKABLE void load() override;

private:
    muse::uicomponents::MenuItemList makeItems();
};
}
