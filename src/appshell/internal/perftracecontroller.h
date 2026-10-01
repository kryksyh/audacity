/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <cstdint>

#include <QList>
#include <QMetaObject>

#include "framework/global/io/path.h"

class QQuickWindow;

namespace au::appshell {
class PerfTraceController
{
public:
    bool isRunning() const;

    void start(QQuickWindow* window);
    bool stopAndSave(const muse::io::path_t& filePath);

private:
    QList<QMetaObject::Connection> m_connections;

    // Each is touched only by the thread that emits its signals
    int64_t m_guiAwakeNs = -1;
    int64_t m_frameBeginNs = -1;
    int64_t m_syncBeginNs = -1;
    int64_t m_renderBeginNs = -1;
    int64_t m_passBeginNs = -1;
    int64_t m_lastSwapNs = -1;
};
}
