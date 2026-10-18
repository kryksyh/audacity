/*
* Audacity: A Digital Audio Editor
*/
#pragma once

#include <QQuickItem>
#include <QTimer>

#include "pianorollmodel.h"

namespace au::trackedit {
class InstrumentView : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(PianoRollModel * model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
public:
    explicit InstrumentView(QQuickItem* parent = nullptr);
    ~InstrumentView() override;
    PianoRollModel* model() const { return m_model; }
    void setModel(PianoRollModel* model);
    bool available() const { return m_available; }

signals:
    void modelChanged();
    void availableChanged();

private:
    void updateView();
    QPointer<PianoRollModel> m_model;
    std::shared_ptr<audio::IInstrumentEditor> m_editor;
    QTimer m_timer;
    bool m_available = false;
};
}
