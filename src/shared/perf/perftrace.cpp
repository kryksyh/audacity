/*
 * Audacity: A Digital Audio Editor
 */
#include "perftrace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace au::perf {
namespace {
void appendJsonString(std::string& out, const char* str)
{
    out += '"';
    for (const char* c = str; *c; ++c) {
        switch (*c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            if (static_cast<unsigned char>(*c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(*c)));
                out += escaped;
            } else {
                out += *c;
            }
        }
    }
    out += '"';
}

void appendUs(std::string& out, int64_t ns)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(ns) / 1000.0);
    out += buf;
}
}

const char* categoryName(Category category)
{
    switch (category) {
    case Category::Frame: return "frame";
    case Category::Backend: return "backend";
    case Category::Model: return "model";
    case Category::Qml: return "qml";
    case Category::Paint: return "paint";
    case Category::Render: return "render";
    case Category::Audio: return "audio";
    }
    return "unknown";
}

Tracer& Tracer::instance()
{
    static Tracer tracer;
    return tracer;
}

int64_t Tracer::nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Tracer::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_relaxed);
}

void Tracer::setThreadName(const char* name)
{
    threadBuffer().name.store(name, std::memory_order_relaxed);
}

void Tracer::recordZone(const char* name, Category category, int64_t startNs, int64_t endNs)
{
    push({ name, startNs, std::max<int64_t>(endNs - startNs, 0), 0.0, category });
}

void Tracer::recordCounter(const char* name, double value)
{
    push({ name, nowNs(), -1, value, Category::Frame });
}

Tracer::ThreadBuffer& Tracer::threadBuffer()
{
    thread_local ThreadBuffer* buffer = nullptr;
    if (!buffer) {
        auto newBuffer = std::make_unique<ThreadBuffer>();
        newBuffer->events.resize(kThreadBufferCapacity);

        std::lock_guard lock(m_buffersMutex);
        newBuffer->tid = static_cast<int>(m_buffers.size()) + 1;
        buffer = newBuffer.get();
        m_buffers.push_back(std::move(newBuffer));
    }
    return *buffer;
}

void Tracer::push(const Event& event)
{
    ThreadBuffer& buffer = threadBuffer();
    const uint64_t index = buffer.written.load(std::memory_order_relaxed);
    buffer.events[index % kThreadBufferCapacity] = event;
    buffer.written.store(index + 1, std::memory_order_release);
}

void Tracer::registerCallCounter(CallCounter* counter)
{
    std::lock_guard lock(m_callCountersMutex);
    m_callCounters.push_back(counter);
}

void Tracer::flushCallCounters()
{
    std::lock_guard lock(m_callCountersMutex);
    for (CallCounter* counter : m_callCounters) {
        const int64_t count = counter->m_count.exchange(0, std::memory_order_relaxed);
        // Skip idle repeats, but emit the first zero so the graph drops back down
        if (count != 0 || counter->m_lastFlushed != 0) {
            recordCounter(counter->m_name, static_cast<double>(count));
        }
        counter->m_lastFlushed = count;
    }
}

void Tracer::clear()
{
    {
        std::lock_guard lock(m_buffersMutex);
        for (auto& buffer : m_buffers) {
            buffer->written.store(0, std::memory_order_release);
        }
    }

    std::lock_guard lock(m_callCountersMutex);
    for (CallCounter* counter : m_callCounters) {
        counter->m_count.store(0, std::memory_order_relaxed);
        counter->m_lastFlushed = 0;
    }
}

std::string Tracer::toChromeTraceJson() const
{
    std::string out = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[";
    bool first = true;
    auto beginEvent = [&](int tid, const char* ph) {
        out += first ? "\n" : ",\n";
        first = false;
        out += "{\"pid\":1,\"tid\":" + std::to_string(tid) + ",\"ph\":\"" + ph + "\",\"name\":";
    };

    std::lock_guard lock(m_buffersMutex);
    for (const auto& buffer : m_buffers) {
        const char* name = buffer->name.load(std::memory_order_relaxed);
        if (name) {
            beginEvent(buffer->tid, "M");
            out += "\"thread_name\",\"args\":{\"name\":";
            appendJsonString(out, name);
            out += "}}";
        }

        const uint64_t written = buffer->written.load(std::memory_order_acquire);
        const uint64_t count = std::min<uint64_t>(written, kThreadBufferCapacity);
        for (uint64_t i = written - count; i < written; ++i) {
            const Event& event = buffer->events[i % kThreadBufferCapacity];
            const bool isCounter = event.durNs < 0;
            beginEvent(buffer->tid, isCounter ? "C" : "X");
            appendJsonString(out, event.name);
            out += ",\"ts\":";
            appendUs(out, event.startNs);
            if (isCounter) {
                char value[32];
                std::snprintf(value, sizeof(value), "%.17g", event.value);
                out += ",\"args\":{\"value\":";
                out += value;
                out += "}}";
            } else {
                out += ",\"dur\":";
                appendUs(out, event.durNs);
                out += ",\"cat\":\"";
                out += categoryName(event.category);
                out += "\"}";
            }
        }
    }
    out += "\n]}\n";
    return out;
}
}
