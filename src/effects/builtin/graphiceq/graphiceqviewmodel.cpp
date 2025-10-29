/*
 * Audacity: A Digital Audio Editor
 */
#include "graphiceqviewmodel.h"

#include "graphiceq.h"
#include "graphiceqbandsmodel.h"

#include "log.h"

namespace au::effects {
GraphicEqViewModel::GraphicEqViewModel(const EffectInstanceId instanceId, QObject* parent)
    : BuiltinEffectModel(instanceId, parent), mBandsModel(new GraphicEqBandsModel(parent, effect<GraphicEq>()))
{
}

void GraphicEqViewModel::doReload()
{
    mBandsModel->reload();
    emit bandsModelChanged();
}

GraphicEqBandsModel* GraphicEqViewModel::bandsModel() const
{
    return mBandsModel;
}

double GraphicEqViewModel::minDbGain() const
{
    return EqualizationBandSliders::minDbGain;
}

double GraphicEqViewModel::maxDbGain() const
{
    return EqualizationBandSliders::maxDbGain;
}
}
