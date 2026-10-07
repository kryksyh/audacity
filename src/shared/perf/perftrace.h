/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace au::perf {
enum class Category : uint8_t {
    Frame,
    Backend,
    Model,
    Qml,
    Paint,
    Render,
    Audio,
};

const char* categoryName(Category category);

class CallCounter;

class Tracer
{
public:
    static constexpr size_t kThreadBufferCapacity = 1 << 18;

    static Tracer& instance();
    static int64_t nowNs();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // `name` must outlive the tracer (string literal); events store the pointer
    void setThreadName(const char* name);
    void recordZone(const char* name, Category category, int64_t startNs, int64_t endNs);
    void recordCounter(const char* name, double value);

    // A named track written by one thread on behalf of another, e.g. audio
    // callback records drained on the GUI thread. One writer per lane
    int createLane(const char* name);
    void recordLaneZone(int lane, const char* name, Category category, int64_t startNs, int64_t endNs);
    void recordLaneCounter(int lane, const char* name, int64_t timeNs, double value);

    void registerCallCounter(CallCounter* counter);
    // Called on every flush, on the flushing thread
    using FlushSource = void (*)();
    void registerFlushSource(FlushSource source);
    // Emits the calls counted since the previous flush as counter samples
    void flushCallCounters();

    void clear();
    std::string toChromeTraceJson() const;

private:
    struct Event {
        const char* name = nullptr;
        int64_t startNs = 0;
        int64_t durNs = 0; // < 0 marks a counter sample
        double value = 0.0;
        Category category = Category::Frame;
    };

    // Single writer (the owning thread); written before the slot it counts is readable
    struct ThreadBuffer {
        int tid = 0;
        std::atomic<const char*> name = nullptr;
        std::atomic<uint64_t> written = 0;
        std::vector<Event> events;
    };

    ThreadBuffer& threadBuffer();
    void push(const Event& event);
    static void push(ThreadBuffer& buffer, const Event& event);

    std::atomic<bool> m_enabled = false;
    mutable std::mutex m_buffersMutex;
    std::vector<std::unique_ptr<ThreadBuffer> > m_buffers;
    std::array<ThreadBuffer*, 8> m_lanes {};
    int m_laneCount = 0;

    mutable std::mutex m_callCountersMutex;
    std::vector<CallCounter*> m_callCounters;
    std::vector<FlushSource> m_flushSources;
};

class CallCounter
{
public:
    explicit CallCounter(const char* name)
        : m_name(name)
    {
        Tracer::instance().registerCallCounter(this);
    }

    void increment() { m_count.fetch_add(1, std::memory_order_relaxed); }

private:
    friend class Tracer;

    const char* m_name;
    std::atomic<int64_t> m_count = 0;
    int64_t m_lastFlushed = 0;
};

class Zone
{
public:
    Zone(const char* name, Category category)
        : m_name(name), m_category(category),
        m_startNs(Tracer::instance().isEnabled() ? Tracer::nowNs() : -1) {}

    ~Zone()
    {
        if (m_startNs >= 0) {
            Tracer::instance().recordZone(m_name, m_category, m_startNs, Tracer::nowNs());
        }
    }

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;

private:
    const char* m_name;
    Category m_category;
    int64_t m_startNs;
};
}

#define AU_PERF_CONCAT_IMPL(a, b) a##b
#define AU_PERF_CONCAT(a, b) AU_PERF_CONCAT_IMPL(a, b)

#define AU_PERF_ZONE(name, category) \
    ::au::perf::Zone AU_PERF_CONCAT(auPerfZone, __LINE__)(name, ::au::perf::Category::category)

#define AU_PERF_COUNT_CALL(name) \
    do { \
        static ::au::perf::CallCounter auPerfCallCounter(name); \
        if (::au::perf::Tracer::instance().isEnabled()) { \
            auPerfCallCounter.increment(); \
        } \
    } while (false)

#define AU_PERF_COUNTER(name, value) \
    do { \
        auto& auPerfTracer = ::au::perf::Tracer::instance(); \
        if (auPerfTracer.isEnabled()) { \
            auPerfTracer.recordCounter(name, static_cast<double>(value)); \
        } \
    } while (false)
