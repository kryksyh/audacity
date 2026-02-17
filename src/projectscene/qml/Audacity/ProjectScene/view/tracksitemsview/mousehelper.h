/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <QtQml/qqmlregistration.h>

#include <QQuickItem>

namespace au::projectscene {
class MouseHelper : public QObject
{
    Q_OBJECT
    QML_ELEMENT
public:
    Q_INVOKABLE void callUngrabMouseOnItem(QQuickItem* item);
};
}
