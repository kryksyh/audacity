/*
 * Audacity: A Digital Audio Editor
 *
 * Test-only virtual MIDI source. Build on macOS with:
 * clang++ -std=c++17 midi_fixture_macos.cpp -framework CoreMIDI -framework CoreAudio -framework CoreFoundation -o midi_fixture
 * Run: midi_fixture /tmp/audacity-midi-qa [--unique-id ID]
 * The optional ID recreates an already-removed test source for reconnect QA.
 * Write request.txt lines: delay_ms status data1 data2 (decimal), then create
 * a ready file. A done file records the result; sent.txt records actual event
 * times in steady-clock seconds. No physical destination is used.
 */
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreAudio/HostTime.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
}

int main(int argc, char** argv)
{
    if (argc != 2 && (argc != 4 || std::string(argv[2]) != "--unique-id")) {
        std::cerr << "usage: midi_fixture <request-directory> [--unique-id ID]\n";
        return 2;
    }
    SInt32 requestedId = 0;
    if (argc == 4) {
        try { requestedId = std::stoi(argv[3]); }
        catch (const std::exception&) { std::cerr << "invalid source ID\n"; return 2; }
        if (!requestedId) { std::cerr << "source ID must be nonzero\n"; return 2; }
    }
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    auto status = MIDIClientCreate(CFSTR("Audacity Native MIDI QA"), nullptr, nullptr, &client);
    if (status == noErr) status = MIDISourceCreate(client, CFSTR("Audacity Native MIDI QA"), &source);
    if (status == noErr && requestedId) status = MIDIObjectSetIntegerProperty(source, kMIDIPropertyUniqueID, requestedId);
    SInt32 sourceId = 0;
    if (status == noErr) status = MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &sourceId);
    if (status == noErr && requestedId && sourceId != requestedId) status = kMIDIIDNotUnique;
    if (status != noErr) {
        std::cerr << "CoreMIDI source creation failed: " << status << '\n';
        if (source) MIDIEndpointDispose(source);
        if (client) MIDIClientDispose(client);
        return 1;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::cout << "SOURCE_ID " << sourceId << "\nREADY Audacity Native MIDI QA\n" << std::flush;
    while (!stopped) {
        if (!std::filesystem::exists(directory / "ready")) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
            continue;
        }
        std::filesystem::remove(directory / "ready");
        std::filesystem::remove(directory / "done");
        std::ifstream requests(directory / "request.txt");
        std::ofstream timings(directory / "sent.txt");
        timings << std::setprecision(17);
        std::string line, error;
        int sent = 0;
        while (!stopped && std::getline(requests, line)) {
            if (line.empty()) continue;
            std::istringstream values(line);
            int delay = 0, message = 0, first = 0, second = 0;
            if (!(values >> delay >> message >> first >> second) || delay < 0 || delay > 10000
                || message < 0x80 || message > 0xef || first < 0 || first > 127 || second < 0 || second > 127) {
                error = "invalid MIDI request";
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            MIDIPacketList list {};
            auto* packet = MIDIPacketListInit(&list);
            const Byte data[] { static_cast<Byte>(message), static_cast<Byte>(first), static_cast<Byte>(second) };
            const auto type = message & 0xf0;
            const auto size = type == 0xc0 || type == 0xd0 ? 2u : 3u;
            const auto hostTime = AudioGetCurrentHostTime();
            const auto time = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (!MIDIPacketListAdd(&list, sizeof(list), packet, hostTime, size, data)) {
                error = "cannot create MIDI packet";
                break;
            }
            status = MIDIReceived(source, &list);
            if (status != noErr) {
                error = "MIDIReceived failed: " + std::to_string(status);
                break;
            }
            timings << message << ' ' << first << ' ' << second << ' ' << time << '\n';
            ++sent;
        }
        timings.close();
        std::ofstream(directory / "done") << (error.empty() ? "OK " + std::to_string(sent) : error) << '\n';
        std::cout << "SENT " << sent << (error.empty() ? "" : " ERROR " + error) << '\n' << std::flush;
    }
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    return 0;
}
