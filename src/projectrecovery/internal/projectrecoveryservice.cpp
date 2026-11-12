/*
 * Audacity: A Digital Audio Editor
 */
#include "projectrecoveryservice.h"

#include <QPromise>

#include <memory>

using namespace muse;
using namespace au::projectrecovery;

namespace {
template<typename T, typename Work>
QFuture<T> run(QThreadPool& pool, Work work)
{
    auto promise = std::make_shared<QPromise<T> >();
    QFuture<T> future = promise->future();
    pool.start([promise, work = std::move(work)]() {
        promise->start();
        promise->addResult(work());
        promise->finish();
    });
    return future;
}
}

QFuture<RetVal<Inspection> > ProjectRecoveryService::inspect(const io::path_t& path)
{
    return run<RetVal<Inspection> >(m_pool, [path]() {
        return ProjectInspector().inspect(path);
    });
}

QFuture<RetVal<RepairReport> > ProjectRecoveryService::repair(RepairMethod method, const io::path_t& original, const io::path_t& output)
{
    return run<RetVal<RepairReport> >(m_pool, [method, original, output]() {
        return ProjectRepairer().repair(method, original, output);
    });
}

void ProjectRecoveryService::stop()
{
    m_pool.waitForDone();
}
