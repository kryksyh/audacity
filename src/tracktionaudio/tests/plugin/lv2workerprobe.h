/*
 * Audacity: A Digital Audio Editor
 */
#pragma once

#include <atomic>

struct Lv2WorkerProbe {
    std::atomic<bool> entered { false };
    std::atomic<bool> release { false };
    std::atomic<bool> finished { false };
    std::atomic<bool> cleaned { false };
    std::atomic<bool> cleanedWhileRunning { false };
    std::atomic<bool> deactivatedWhileRunning { false };
    std::atomic<int> workCalls { 0 };
    std::atomic<int> replies { 0 };
    std::atomic<int> runs { 0 };
    std::atomic<int> endRuns { 0 };
    std::atomic<int> repliesAtEnd { 0 };
    std::atomic<int> emptyRequests { 0 };
    std::atomic<int> emptyReplies { 0 };
};
