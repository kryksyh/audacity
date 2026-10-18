/**********************************************************************

  Audacity: A Digital Audio Editor

**********************************************************************/

#pragma once

#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "trackedit/iselectioncontroller.h"

#include "uicomponents/qml/Muse/UiComponents/abstractmenumodel.h"

namespace au::projectscene {
class MultiClipContextMenuModel : public muse::uicomponents::AbstractMenuModel
{
    Q_OBJECT
    muse::ContextInject<context::IGlobalContext> globalContext{ this };
    muse::ContextInject<trackedit::ISelectionController> selectionController{ this };

public:
    Q_INVOKABLE void load() override;

private:
    muse::uicomponents::MenuItemList makeItems();
};
}
