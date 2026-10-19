/*
* Audacity: A Digital Audio Editor
*/
#include "instrumentview.h"

#include <QQuickWindow>

using namespace au::trackedit;

InstrumentView::InstrumentView(QQuickItem* parent)
    : QQuickItem(parent)
{
    m_timer.setInterval(50);
    connect(&m_timer, &QTimer::timeout, this, &InstrumentView::updateView);
    connect(this, &QQuickItem::visibleChanged, this, &InstrumentView::updateView);
    connect(this, &QQuickItem::windowChanged, this, &InstrumentView::updateView);
    m_timer.start();
}

InstrumentView::~InstrumentView()
{
    // QQuickItem emits signals during its destructor.
    disconnect(this, &QQuickItem::visibleChanged, this, &InstrumentView::updateView);
    disconnect(this, &QQuickItem::windowChanged, this, &InstrumentView::updateView);
    if (m_editor) {
        m_editor->detachView();
    }
}

void InstrumentView::setModel(PianoRollModel* model)
{
    if (m_model == model) {
        return;
    }
    if (m_model) {
        disconnect(m_model, nullptr, this, nullptr);
    }
    m_model = model;
    if (m_model) {
        connect(m_model, &PianoRollModel::instrumentEditorChanged, this, &InstrumentView::updateView);
    }
    updateView();
    emit modelChanged();
}

void InstrumentView::updateView()
{
    const auto editor = m_model ? m_model->instrumentEditor() : nullptr;
    if (m_editor != editor || !isVisible() || !window() || !window()->isVisible()) {
        if (m_editor) {
            m_editor->detachView();
        }
        m_editor = editor;
    }
    bool available = false;
    if (m_editor && m_editor->active() && isVisible() && window() && window()->isVisible()) {
        const auto position = mapToScene(QPointF());
        available = m_editor->attachView(reinterpret_cast<void*>(window()->winId()), qRound(position.x()), qRound(position.y()),
                                         qRound(width()), qRound(height()));
    }
    if (available) {
        const auto size = m_editor->editorSize();
        setImplicitWidth(size.width);
        setImplicitHeight(size.height);
    }
    if (m_available != available) {
        m_available = available;
        emit availableChanged();
    }
}
