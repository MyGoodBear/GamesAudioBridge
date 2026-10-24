#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

// Cross-process single-producer/single-consumer ring shared by the injected hook
// and the x64 ASIO host. Keep layout fixed-width and identical in both processes.
namespace GAB_ASIO_IPC {
constexpr wchar_t kMappingName[] = L"Local\\GameAudioBridge_ASIO_Ring_v1";
constexpr uint32_t kMagic = 0x31424147; // GAB1
constexpr LONG kMaxChannels = 8;
constexpr LONG64 kCapacityFrames = 32768;
struct SharedRing {
    uint32_t magic;
    volatile LONG sourceChannels;
    volatile LONG sampleRate;       // ASIO hardware output rate
    volatile LONG inputSampleRate;  // latest intercepted game stream rate
    volatile LONG inputBits;        // latest intercepted source bit depth
    volatile LONG resamplingEnabled; // 1 = convert mismatched rates, 0 = reject
    volatile LONG formatMismatch;   // nonzero when input rate differs from ASIO rate
    volatile LONG writerLock;
    volatile LONG64 writeFrame;
    volatile LONG64 readFrame;
    double samples[kCapacityFrames * kMaxChannels];
};
inline size_t SampleIndex(LONG64 frame, LONG channel) {
    return static_cast<size_t>(frame % kCapacityFrames) * kMaxChannels + static_cast<size_t>(channel);
}
}
