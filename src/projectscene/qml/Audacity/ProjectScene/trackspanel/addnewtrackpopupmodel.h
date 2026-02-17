/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QObject>

#include "modularity/ioc.h"
#include "iglobalconfiguration.h"

#include <QtQml/qqmlregistration.h>

namespace au::projectscene {
class AddNewTrackPopupModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    muse::GlobalInject<muse::IGlobalConfiguration> globalConfiguration;

public:
    explicit AddNewTrackPopupModel(QObject* parent = nullptr);

    Q_INVOKABLE bool isAddLabelAvailable() const;
};
}
