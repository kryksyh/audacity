/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <qobjectdefs.h>

namespace au::projectscene {
class DirectionType
{
    Q_GADGET

public:
    enum class Direction {
        Left = 0,
        Right
    };
    Q_ENUM(Direction)
};

using Direction = DirectionType::Direction;
}