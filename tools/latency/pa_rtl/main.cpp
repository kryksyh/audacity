// Round-trip latency harness on PortAudio.
//
// Plays a pseudo-random burst once per period on one output channel and finds
// it in one captured input channel by cross-correlation. Output and input share
// one frame counter, so the burst offset is the round trip in samples. Needs a
// loopback cable from the output to the input.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "portaudio.h"
#ifdef _WIN32
#include "pa_win_wasapi.h"
#endif

namespace {
struct Options {
    bool list = false;
    std::string host;
    std::string input;
    std::string output;
    double rate = 48000.0;
    unsigned long frames = 0; // 0 = paFramesPerBufferUnspecified, as Audacity uses
    double latency = -1.0;    // suggested latency in seconds; < 0 = device default low
    int inChannel = 0;
    int outChannel = 0;
    double seconds = 6.0;
    double period = 1.0;
    float gain = 0.25f;
    bool exclusive = false;
    bool json = false;
    bool selftest = false;
    std::string wavPath;
    std::string alignReference;
    std::string alignTake;
};

constexpr size_t kBurstLength = 512;
constexpr size_t kMaxCallbacks = 1 << 20;

struct State {
    std::vector<float> burst;
    size_t periodFrames = 0;
    std::vector<float> capture;
    size_t position = 0;
    int outChannels = 0;
    int inChannels = 0;
    int outChannel = 0;
    int inChannel = 0;

    // Written by the callback only, read after the stream has stopped
    std::vector<int64_t> callbackStartNs;
    size_t callbacks = 0;
    unsigned long minFrames = ULONG_MAX;
    unsigned long maxFrames = 0;
    uint64_t flagCounts[5] = {};
};

int64_t nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int callback(const void* input, void* output, unsigned long frames,
             const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags, void* userData)
{
    State& s = *static_cast<State*>(userData);
    if (s.callbacks < s.callbackStartNs.size()) {
        s.callbackStartNs[s.callbacks] = nowNs();
    }
    ++s.callbacks;
    s.minFrames = std::min(s.minFrames, frames);
    s.maxFrames = std::max(s.maxFrames, frames);
    for (int bit = 0; bit < 5; ++bit) {
        if (flags & (1u << bit)) {
            ++s.flagCounts[bit];
        }
    }

    auto out = static_cast<float*>(output);
    auto in = static_cast<const float*>(input);
    for (unsigned long i = 0; i < frames; ++i) {
        const size_t n = s.position + i;
        const size_t phase = n % s.periodFrames;
        const float value = phase < s.burst.size() ? s.burst[phase] : 0.0f;
        for (int ch = 0; ch < s.outChannels; ++ch) {
            out[i * s.outChannels + ch] = ch == s.outChannel ? value : 0.0f;
        }
        if (n < s.capture.size()) {
            s.capture[n] = in ? in[i * s.inChannels + s.inChannel] : 0.0f;
        }
    }
    s.position += frames;
    return s.position >= s.capture.size() ? paComplete : paContinue;
}

void usage()
{
    std::puts(
        "pa_rtl [options]\n"
        "  --list                 list host APIs and devices\n"
        "  --host NAME            host API name substring (default: PortAudio default)\n"
        "  --in DEV --out DEV     device index or name substring (default: host defaults)\n"
        "  --rate HZ              sample rate (48000)\n"
        "  --frames N             frames per buffer, 0 = unspecified like Audacity (0)\n"
        "  --latency SEC          suggested latency for both directions (device default low)\n"
        "  --in-ch K --out-ch K   channel carrying the loopback (0)\n"
        "  --seconds S            run time (6)\n"
        "  --period S             burst period, must exceed the round trip (1)\n"
        "  --gain G               burst amplitude (0.25)\n"
        "  --exclusive            WASAPI exclusive mode (Windows)\n"
        "  --json                 one JSON line instead of text\n"
        "  --wav PATH             also write the captured channel as 32-bit float WAV\n"
        "  --align REF TAKE       offset of each burst of REF (e.g. bursts.wav) in TAKE, no audio\n"
        "  --selftest             check the detector on synthetic delays, no audio");
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--list") {
            o.list = true;
        } else if (a == "--host") {
            o.host = next();
        } else if (a == "--in") {
            o.input = next();
        } else if (a == "--out") {
            o.output = next();
        } else if (a == "--rate") {
            o.rate = std::atof(next());
        } else if (a == "--frames") {
            o.frames = std::strtoul(next(), nullptr, 10);
        } else if (a == "--latency") {
            o.latency = std::atof(next());
        } else if (a == "--in-ch") {
            o.inChannel = std::atoi(next());
        } else if (a == "--out-ch") {
            o.outChannel = std::atoi(next());
        } else if (a == "--seconds") {
            o.seconds = std::atof(next());
        } else if (a == "--period") {
            o.period = std::atof(next());
        } else if (a == "--gain") {
            o.gain = static_cast<float>(std::atof(next()));
        } else if (a == "--exclusive") {
            o.exclusive = true;
        } else if (a == "--json") {
            o.json = true;
        } else if (a == "--wav") {
            o.wavPath = next();
        } else if (a == "--align") {
            o.alignReference = next();
            o.alignTake = next();
        } else if (a == "--selftest") {
            o.selftest = true;
        } else {
            usage();
            return false;
        }
    }
    return true;
}

void listDevices()
{
    for (PaHostApiIndex h = 0; h < Pa_GetHostApiCount(); ++h) {
        const PaHostApiInfo* host = Pa_GetHostApiInfo(h);
        std::printf("host %d: %s%s\n", h, host->name, h == Pa_GetDefaultHostApi() ? " (default)" : "");
        for (int d = 0; d < host->deviceCount; ++d) {
            const PaDeviceIndex index = Pa_HostApiDeviceIndexToDeviceIndex(h, d);
            const PaDeviceInfo* dev = Pa_GetDeviceInfo(index);
            std::printf("  [%d] %s  in %d out %d  %.0f Hz  low in/out %.1f/%.1f ms  high in/out %.1f/%.1f ms\n",
                        index, dev->name, dev->maxInputChannels, dev->maxOutputChannels, dev->defaultSampleRate,
                        dev->defaultLowInputLatency * 1e3, dev->defaultLowOutputLatency * 1e3,
                        dev->defaultHighInputLatency * 1e3, dev->defaultHighOutputLatency * 1e3);
        }
    }
}

PaHostApiIndex findHost(const std::string& name)
{
    if (name.empty()) {
        return Pa_GetDefaultHostApi();
    }
    for (PaHostApiIndex h = 0; h < Pa_GetHostApiCount(); ++h) {
        if (std::strstr(Pa_GetHostApiInfo(h)->name, name.c_str())) {
            return h;
        }
    }
    return -1;
}

PaDeviceIndex findDevice(PaHostApiIndex host, const std::string& spec, bool input)
{
    const PaHostApiInfo* info = Pa_GetHostApiInfo(host);
    if (spec.empty()) {
        return input ? info->defaultInputDevice : info->defaultOutputDevice;
    }
    char* end = nullptr;
    const long number = std::strtol(spec.c_str(), &end, 10);
    if (end && *end == '\0') {
        return static_cast<PaDeviceIndex>(number);
    }
    for (int d = 0; d < info->deviceCount; ++d) {
        const PaDeviceIndex index = Pa_HostApiDeviceIndexToDeviceIndex(host, d);
        const PaDeviceInfo* dev = Pa_GetDeviceInfo(index);
        const int channels = input ? dev->maxInputChannels : dev->maxOutputChannels;
        if (channels > 0 && std::strstr(dev->name, spec.c_str())) {
            return index;
        }
    }
    return paNoDevice;
}

struct BurstHit {
    long lag = -1;
    double confidence = 0.0;
};

// Correlation peak over the RMS of all correlation values in the period
BurstHit findBurst(const std::vector<float>& capture, const std::vector<float>& burst, size_t begin, size_t span)
{
    BurstHit hit;
    if (begin + span + burst.size() > capture.size()) {
        return hit;
    }
    double best = 0.0;
    double sumSquares = 0.0;
    for (size_t lag = 0; lag < span; ++lag) {
        double c = 0.0;
        const float* x = capture.data() + begin + lag;
        for (size_t j = 0; j < burst.size(); ++j) {
            c += burst[j] * x[j];
        }
        sumSquares += c * c;
        if (std::fabs(c) > std::fabs(best)) {
            best = c;
            hit.lag = static_cast<long>(lag);
        }
    }
    const double rms = std::sqrt(sumSquares / span);
    hit.confidence = rms > 0 ? std::fabs(best) / rms : 0.0;
    return hit;
}

void writeWav(const std::string& path, const std::vector<float>& samples, uint32_t rate)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return;
    }
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(3); // IEEE float
    u16(1);
    u32(rate);
    u32(rate * 4);
    u16(4);
    u16(32);
    std::fwrite("data", 1, 4, f);
    u32(dataBytes);
    std::fwrite(samples.data(), sizeof(float), samples.size(), f);
    std::fclose(f);
}

std::vector<float> makeBurst(float gain)
{
    std::vector<float> burst;
    uint32_t seed = 0x1234567u;
    for (size_t i = 0; i < kBurstLength; ++i) {
        seed = seed * 1664525u + 1013904223u;
        burst.push_back((seed >> 31) ? gain : -gain);
    }
    return burst;
}

// First channel of a PCM 16/24/32-bit or float WAV, as float
bool readWav(const std::string& path, std::vector<float>& samples, uint32_t& rate)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(f);
    auto u16 = [&](size_t at) { return uint16_t(bytes[at] | bytes[at + 1] << 8); };
    auto u32 = [&](size_t at) { return uint32_t(u16(at) | uint32_t(u16(at + 2)) << 16); };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        return false;
    }
    uint16_t format = 0, channels = 0, bits = 0;
    for (size_t at = 12; at + 8 <= bytes.size();) {
        const uint32_t size = u32(at + 4);
        if (!std::memcmp(bytes.data() + at, "fmt ", 4)) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            bits = u16(at + 22);
            if (format == 0xFFFE) {
                format = u16(at + 32); // WAVE_FORMAT_EXTENSIBLE sub-format
            }
        } else if (!std::memcmp(bytes.data() + at, "data", 4)) {
            const size_t frameBytes = size_t(channels) * bits / 8;
            if (!frameBytes) {
                return false;
            }
            const size_t frames = std::min<size_t>(size, bytes.size() - at - 8) / frameBytes;
            samples.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                const unsigned char* p = bytes.data() + at + 8 + i * frameBytes;
                if (format == 3 && bits == 32) {
                    std::memcpy(&samples[i], p, 4);
                } else if (format == 1 && bits == 16) {
                    samples[i] = int16_t(p[0] | p[1] << 8) / 32768.0f;
                } else if (format == 1 && bits == 24) {
                    samples[i] = (int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8) / 8388608.0f;
                } else if (format == 1 && bits == 32) {
                    samples[i] = int32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24) / 2147483648.0f;
                } else {
                    return false;
                }
            }
            return true;
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

// Positive offset: the take is late against the reference. Accepts weaker
// peaks than the live run: a quiet acoustic loopback still gives one stable offset
int align(const Options& o)
{
    std::vector<float> reference, take;
    uint32_t referenceRate = 0, takeRate = 0;
    if (!readWav(o.alignReference, reference, referenceRate) || !readWav(o.alignTake, take, takeRate)) {
        std::fprintf(stderr, "cannot read %s or %s (PCM 16/24/32 or float WAV)\n", o.alignReference.c_str(), o.alignTake.c_str());
        return 1;
    }
    if (referenceRate != takeRate) {
        std::fprintf(stderr, "sample rates differ: %u vs %u\n", referenceRate, takeRate);
        return 1;
    }
    const std::vector<float> burst(reference.begin(), reference.begin() + std::min(kBurstLength, reference.size()));
    const size_t period = referenceRate;
    const size_t span = period - kBurstLength;
    std::vector<long> offsets;
    for (size_t k = 1; (k + 1) * period <= std::min(reference.size(), take.size()); ++k) {
        const size_t begin = k * period - period / 2;
        const BurstHit hit = findBurst(take, burst, begin, span);
        const long offset = hit.lag + static_cast<long>(begin) - static_cast<long>(k * period);
        std::printf("burst %2zu: offset %+6ld samples (%+8.3f ms) confidence %6.1f\n",
                    k, offset, offset * 1e3 / takeRate, hit.confidence);
        if (hit.lag >= 0 && hit.confidence >= 3.0) {
            offsets.push_back(offset);
        }
    }
    if (offsets.empty()) {
        std::printf("no burst found in the take\n");
        return 3;
    }
    std::sort(offsets.begin(), offsets.end());
    const long median = offsets[offsets.size() / 2];
    std::printf("take vs reference: median %+ld samples = %+.3f ms (bursts %zu, spread %ld)\n",
                median, median * 1e3 / takeRate, offsets.size(), offsets.back() - offsets.front());
    return 0;
}

// Delayed, attenuated, inverted copies of the burst train in noise
int selftest()
{
    const std::vector<float> burst = makeBurst(0.25f);
    const size_t period = 48000;
    const size_t span = period - kBurstLength;
    int failures = 0;
    uint32_t seed = 42;
    for (const size_t delay : { size_t(0), size_t(1), size_t(37), size_t(4410), size_t(8110), span - 1 }) {
        std::vector<float> capture(period * 3, 0.0f);
        for (size_t n = delay; n < capture.size(); ++n) {
            const size_t phase = (n - delay) % period;
            const float value = phase < burst.size() ? burst[phase] : 0.0f;
            seed = seed * 1664525u + 1013904223u;
            const float noise = ((seed >> 8) / float(1 << 24) - 0.5f) * 0.1f;
            capture[n] = -0.2f * value + noise;
        }
        for (size_t begin = 0; begin + period <= capture.size(); begin += period) {
            const BurstHit hit = findBurst(capture, burst, begin, span);
            const bool ok = hit.lag == static_cast<long>(delay) && hit.confidence >= 8.0;
            if (!ok) {
                ++failures;
            }
            std::printf("delay %5zu period at %6zu: lag %5ld confidence %6.1f %s\n",
                        delay, begin, hit.lag, hit.confidence, ok ? "ok" : "FAIL");
        }
    }
    std::printf("%s\n", failures ? "selftest FAILED" : "selftest passed");
    return failures ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    Options o;
    if (!parse(argc, argv, o)) {
        return 2;
    }
    if (o.selftest) {
        return selftest();
    }
    if (!o.alignTake.empty()) {
        return align(o);
    }

    if (Pa_Initialize() != paNoError) {
        std::fprintf(stderr, "Pa_Initialize failed\n");
        return 1;
    }
    struct Terminate {
        ~Terminate() { Pa_Terminate(); }
    } terminate;

    if (o.list) {
        listDevices();
        return 0;
    }

    const PaHostApiIndex host = findHost(o.host);
    if (host < 0) {
        std::fprintf(stderr, "host API not found: %s\n", o.host.c_str());
        return 1;
    }
    const PaDeviceIndex inDev = findDevice(host, o.input, true);
    const PaDeviceIndex outDev = findDevice(host, o.output, false);
    if (inDev == paNoDevice || outDev == paNoDevice) {
        std::fprintf(stderr, "device not found (in %d, out %d); try --list\n", inDev, outDev);
        return 1;
    }
    const PaDeviceInfo* inInfo = Pa_GetDeviceInfo(inDev);
    const PaDeviceInfo* outInfo = Pa_GetDeviceInfo(outDev);

    State s;
    s.inChannel = o.inChannel;
    s.outChannel = o.outChannel;
    s.inChannels = std::min(std::max(o.inChannel + 1, 1), inInfo->maxInputChannels);
    s.outChannels = std::min(std::max(o.outChannel + 1, 2), outInfo->maxOutputChannels);
    if (o.inChannel >= s.inChannels || o.outChannel >= s.outChannels) {
        std::fprintf(stderr, "channel out of range\n");
        return 1;
    }
    s.periodFrames = static_cast<size_t>(o.period * o.rate);
    s.capture.assign(static_cast<size_t>(o.seconds * o.rate), 0.0f);
    s.callbackStartNs.assign(kMaxCallbacks, 0);
    s.burst = makeBurst(o.gain);

    PaStreamParameters in {};
    in.device = inDev;
    in.channelCount = s.inChannels;
    in.sampleFormat = paFloat32;
    in.suggestedLatency = o.latency >= 0 ? o.latency : inInfo->defaultLowInputLatency;
    PaStreamParameters out {};
    out.device = outDev;
    out.channelCount = s.outChannels;
    out.sampleFormat = paFloat32;
    out.suggestedLatency = o.latency >= 0 ? o.latency : outInfo->defaultLowOutputLatency;

#ifdef _WIN32
    PaWasapiStreamInfo wasapiIn {};
    PaWasapiStreamInfo wasapiOut {};
    if (o.exclusive) {
        for (PaWasapiStreamInfo* w : { &wasapiIn, &wasapiOut }) {
            w->size = sizeof(PaWasapiStreamInfo);
            w->hostApiType = paWASAPI;
            w->version = 1;
            w->flags = paWinWasapiExclusive;
        }
        in.hostApiSpecificStreamInfo = &wasapiIn;
        out.hostApiSpecificStreamInfo = &wasapiOut;
    }
#endif

    PaStream* stream = nullptr;
    const auto openStart = std::chrono::steady_clock::now();
    PaError err = Pa_OpenStream(&stream, &in, &out, o.rate,
                                o.frames ? o.frames : paFramesPerBufferUnspecified,
                                paNoFlag, callback, &s);
    const double openMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - openStart).count();
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_OpenStream: %s\n", Pa_GetErrorText(err));
        return 1;
    }
    // Copied: the struct belongs to the stream and dies with Pa_CloseStream
    const PaStreamInfo streamInfo = *Pa_GetStreamInfo(stream);
    const PaStreamInfo* info = &streamInfo;

    const auto startStart = std::chrono::steady_clock::now();
    err = Pa_StartStream(stream);
    const double startMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startStart).count();
    if (err != paNoError) {
        std::fprintf(stderr, "Pa_StartStream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        return 1;
    }
    while (Pa_IsStreamActive(stream) == 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    if (!o.wavPath.empty()) {
        writeWav(o.wavPath, s.capture, static_cast<uint32_t>(info->sampleRate));
    }

    std::vector<long> lags;
    double minConfidence = 1e9;
    double bestRejected = 0.0;
    const size_t span = s.periodFrames - kBurstLength;
    for (size_t begin = 0; begin + s.periodFrames <= s.capture.size(); begin += s.periodFrames) {
        const BurstHit hit = findBurst(s.capture, s.burst, begin, span);
        if (!o.json) {
            std::printf("burst at %8zu: lag %6ld confidence %6.1f\n", begin, hit.lag, hit.confidence);
        }
        if (hit.lag >= 0 && hit.confidence >= 8.0) {
            lags.push_back(hit.lag);
            minConfidence = std::min(minConfidence, hit.confidence);
        } else {
            bestRejected = std::max(bestRejected, hit.confidence);
        }
    }

    double periodMeanMs = 0.0;
    double periodMaxMs = 0.0;
    const size_t timed = std::min(s.callbacks, s.callbackStartNs.size());
    for (size_t i = 1; i < timed; ++i) {
        const double ms = (s.callbackStartNs[i] - s.callbackStartNs[i - 1]) / 1e6;
        periodMeanMs += ms;
        periodMaxMs = std::max(periodMaxMs, ms);
    }
    if (timed > 1) {
        periodMeanMs /= static_cast<double>(timed - 1);
    }

    long median = -1;
    long spread = 0;
    if (!lags.empty()) {
        std::vector<long> sorted = lags;
        std::sort(sorted.begin(), sorted.end());
        median = sorted[sorted.size() / 2];
        spread = sorted.back() - sorted.front();
    }
    const double reportedFrames = (info->inputLatency + info->outputLatency) * info->sampleRate;

    if (o.json) {
        std::printf("{\"host\":\"%s\",\"in\":\"%s\",\"out\":\"%s\",\"rate\":%.0f,\"frames_requested\":%lu,"
                    "\"suggested_in_ms\":%.3f,\"suggested_out_ms\":%.3f,\"exclusive\":%s,"
                    "\"callback_frames_min\":%lu,\"callback_frames_max\":%lu,\"callback_period_mean_ms\":%.3f,"
                    "\"callback_period_max_ms\":%.3f,\"pa_input_latency_ms\":%.3f,\"pa_output_latency_ms\":%.3f,"
                    "\"measured_rtl_frames\":%ld,\"measured_rtl_ms\":%.3f,\"rtl_spread_frames\":%ld,\"bursts_found\":%zu,"
                    "\"min_confidence\":%.1f,\"reported_minus_measured_frames\":%.1f,"
                    "\"input_underflow\":%llu,\"input_overflow\":%llu,\"output_underflow\":%llu,\"output_overflow\":%llu,"
                    "\"priming_output\":%llu,\"open_ms\":%.2f,\"start_ms\":%.2f}\n",
                    Pa_GetHostApiInfo(host)->name, inInfo->name, outInfo->name, info->sampleRate, o.frames,
                    in.suggestedLatency * 1e3, out.suggestedLatency * 1e3, o.exclusive ? "true" : "false",
                    s.minFrames, s.maxFrames, periodMeanMs, periodMaxMs,
                    info->inputLatency * 1e3, info->outputLatency * 1e3,
                    median, median >= 0 ? median * 1e3 / info->sampleRate : -1.0, spread, lags.size(),
                    lags.empty() ? 0.0 : minConfidence, median >= 0 ? reportedFrames - median : 0.0,
                    (unsigned long long)s.flagCounts[0], (unsigned long long)s.flagCounts[1],
                    (unsigned long long)s.flagCounts[2], (unsigned long long)s.flagCounts[3],
                    (unsigned long long)s.flagCounts[4], openMs, startMs);
        return lags.empty() ? 3 : 0;
    }

    std::printf("host        %s\n", Pa_GetHostApiInfo(host)->name);
    std::printf("devices     in [%d] %s / out [%d] %s\n", inDev, inInfo->name, outDev, outInfo->name);
    std::printf("stream      %.0f Hz, requested %lu frames, suggested in/out %.2f/%.2f ms%s\n",
                info->sampleRate, o.frames, in.suggestedLatency * 1e3, out.suggestedLatency * 1e3,
                o.exclusive ? ", WASAPI exclusive" : "");
    std::printf("callbacks   %zu, frames %lu..%lu, period mean %.3f ms, max %.3f ms\n",
                s.callbacks, s.minFrames, s.maxFrames, periodMeanMs, periodMaxMs);
    std::printf("status      in underflow %llu, in overflow %llu, out underflow %llu, out overflow %llu, priming %llu\n",
                (unsigned long long)s.flagCounts[0], (unsigned long long)s.flagCounts[1],
                (unsigned long long)s.flagCounts[2], (unsigned long long)s.flagCounts[3],
                (unsigned long long)s.flagCounts[4]);
    float inputPeak = 0.0f;
    for (const float v : s.capture) {
        inputPeak = std::max(inputPeak, std::fabs(v));
    }
    std::printf("open/start  %.2f / %.2f ms\n", openMs, startMs);
    std::printf("input peak  %.4f%s\n", inputPeak, inputPeak == 0.0f ? " (silent: check the input and microphone permission)" : "");
    std::printf("reported    in %.3f ms + out %.3f ms = %.1f frames\n",
                info->inputLatency * 1e3, info->outputLatency * 1e3, reportedFrames);
    if (lags.empty()) {
        std::printf("measured    no burst found (best confidence %.1f, need 8): check the loopback cable, channels and gain\n",
                    bestRejected);
        return 3;
    }
    std::printf("measured    %ld frames = %.3f ms (bursts %zu, spread %ld frames, min confidence %.1f)\n",
                median, median * 1e3 / info->sampleRate, lags.size(), spread, minConfidence);
    std::printf("error       reported - measured = %+.1f frames\n", reportedFrames - median);
    return 0;
}
