#pragma once
#pragma once
/**********************************************************************

  Audacity: A Digital Audio Editor

**********************************************************************/

#pragma once

#include "timeline/irulerformat.h"

namespace au::projectscene {
class BeatsMeasuresFormat : public IRulerFormat
{
public:
    IntervalInfo intervalInfo(TimelineContext* context) override;
    QString label(double d, const IntervalInfo& timeIntervalInfo, TickType tickType, TimelineContext* context) override;
};
}
