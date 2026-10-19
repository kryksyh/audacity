/*
 * Audacity: A Digital Audio Editor
 */
#include "audiounitview.h"

#include <QQuickWindow>

#include "framework/global/log.h"

#include "tracktionaudio/internal/tracktioneffectinstance.h"
#include "audiounitcontrol.h"

namespace au::effects {
AudioUnitView::AudioUnitView(QQuickItem* parent)
    : QQuickItem(parent), muse::Contextable(muse::iocCtxForQmlObject(this))
{
}

AudioUnitView::~AudioUnitView()
{
    deinit();
}

int AudioUnitView::instanceId() const
{
    return m_instanceId;
}

void AudioUnitView::setInstanceId(int newInstanceId)
{
    if (m_instanceId == newInstanceId) {
        return;
    }
    m_instanceId = newInstanceId;
    emit instanceIdChanged();
}

void AudioUnitView::init()
{
    m_instance = std::dynamic_pointer_cast<tracktionaudio::TracktionHostedEffectInstance>(
        instancesRegister()->instanceById(m_instanceId));
    if (!m_instance || !m_instance->audioUnit()) {
        LOGE() << "no AudioUnit instance for instanceId: " << m_instanceId;
        return;
    }

    const EffectId effectId = instancesRegister()->effectIdByInstanceId(m_instanceId);
    const bool isGraphical = (configuration()->effectUIMode(effectId) == EffectUIMode::VendorUI);

    if (!m_instance->Init() || !m_instance->openAudioUnitEditor()) {
        LOGE() << "failed to pre-initialize AU instance for editor, instanceId: " << m_instanceId;
        return;
    }

    m_auControl = std::make_unique<AUControl>(window());
    const auto unit = m_instance->audioUnit();
    if (!m_auControl->create(AudioComponentInstanceGetComponent(unit), unit, isGraphical)) {
        LOGE() << "failed to create AU control for effect " << effectId.toStdString()
               << ", instanceId: " << m_instanceId << ", graphical: " << isGraphical;
        m_auControl.reset();
        m_instance->closeAudioUnitEditor();
        return;
    }

    connect(m_auControl.get(), &AUControl::sizeChanged, this, [this](){
        updateViewGeometry();
    });

    m_auControl->show();

    updateViewGeometry();
}

void AudioUnitView::deinit()
{
    m_auControl.reset();
    if (m_instance) {
        m_instance->closeAudioUnitEditor();
    }
    m_instance.reset();
}

void AudioUnitView::reload()
{
    deinit();
    init();
}

void AudioUnitView::updateViewGeometry()
{
    if (!m_auControl) {
        return;
    }

    QSize requiredSize = m_auControl->size();
    const auto availableSize = window()->screen()->availableSize();

    const int titleBarHeight = window()->frameGeometry().height() - window()->geometry().height();

    const int requiredWidth = requiredSize.width();
    const int requiredHeight = requiredSize.height();

    const int availableWidth = availableSize.width() - 2 * m_sidePadding;
    const int availableHeight = availableSize.height() - titleBarHeight - m_topPadding - m_bottomPadding;

    const int newWidth = std::min(requiredWidth, availableWidth);
    const int newHeight = std::min(requiredHeight, availableHeight);

    const int implicitWidth = std::max(m_minimumWidth, newWidth);
    const int sidePadding = std::max(m_sidePadding, (implicitWidth - newWidth) / 2);

    setImplicitWidth(implicitWidth);
    setImplicitHeight(newHeight);

    m_auControl->setGeometry(sidePadding, m_topPadding, newWidth, newHeight);
}

int AudioUnitView::minimumWidth() const
{
    return m_minimumWidth;
}

void AudioUnitView::setMinimumWidth(int newMinimumWidth)
{
    if (m_minimumWidth == newMinimumWidth) {
        return;
    }
    m_minimumWidth = newMinimumWidth;
    emit minimumWidthChanged();
}

int AudioUnitView::bottomPadding() const
{
    return m_bottomPadding;
}

void AudioUnitView::setBottomPadding(int newBottomPadding)
{
    if (m_bottomPadding == newBottomPadding) {
        return;
    }
    m_bottomPadding = newBottomPadding;
    emit bottomPaddingChanged();
    updateViewGeometry();
}

int AudioUnitView::topPadding() const
{
    return m_topPadding;
}

void AudioUnitView::setTopPadding(int newTopPadding)
{
    if (m_topPadding == newTopPadding) {
        return;
    }
    m_topPadding = newTopPadding;
    emit topPaddingChanged();
    updateViewGeometry();
}

int AudioUnitView::sidePadding() const
{
    return m_sidePadding;
}

void AudioUnitView::setSidePadding(int newSidePadding)
{
    if (m_sidePadding == newSidePadding) {
        return;
    }
    m_sidePadding = newSidePadding;
    emit sidePaddingChanged();
    updateViewGeometry();
}
} // namespace au::effects
