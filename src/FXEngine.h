#pragma once
#include "FXModule.h"
#include "FXDSP.h"
#include "FXMacros.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace FX
{
    // One slot's controls for a block, as read from the host parameters.
    struct SlotParams
    {
        int   type = 0; // ModuleType as an integer, straight from the choice parameter
        bool  on   = true;
        float mix  = 1.0f;
        SlotValues values { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };
    };

    // Everything the FX section reads for one block.
    struct EngineParams
    {
        std::array<SlotParams, kNumSlots> slots {};
        std::array<float, 2> insertLevel { 1.0f, 1.0f };
        std::array<float, kNumMacros> macros {};
        const MacroSnapshot* macroSnapshot = nullptr;
        BlockContext context;
    };

    // The three mono buses the layers are summed into (§7.1's per-layer routing). A layer routed
    // "Both" is added to both insert buses.
    enum class Bus { dry = 0, insertA = 1, insertB = 2 };
    constexpr int kNumBuses = 3;

    // The longest a tail-producing module rings after its input stops, from its controls: a
    // reverb's decay plus pre-delay, a delay's repeats down to -60 dB (capped). Zero for the rest.
    double tailSeconds(ModuleType type, const SlotValues& values, double bpm);
}

// Modules retired by the audio thread, waiting for the message thread to delete them - deleting
// on the audio thread would mean freeing memory there (§2's cardinal rule). A lock-free
// single-producer single-consumer queue.
class FXRetiredModules
{
public:
    FXRetiredModules() = default;
    ~FXRetiredModules() { deleteAll(); }

    // Audio thread. The capacity is far beyond what can pile up between two message-thread drains
    // (at most two per slot per drain); should it ever fill regardless, the module is leaked rather
    // than freed here.
    void push(FXModule* module) noexcept;

    // Message thread.
    void deleteAll();

private:
    static constexpr int kCapacity = 256;
    std::array<FXModule*, kCapacity> items {};
    juce::AbstractFifo fifo { kCapacity };

    JUCE_DECLARE_NON_COPYABLE(FXRetiredModules)
};

// FXSlot — one module slot: holds the active module, does bypass and wet/dry mix identically for
// every module, and takes new modules from the message thread without locks.
//
// Bypass is exact. A bypassed module is not run at all, and the slot outputs its input delayed by
// the module's latency - so with oversampling off, a bypassed slot's output is its input, bit for
// bit (§6: "bypass = bit-identical null test"), and with oversampling on, it is the input exactly
// one latency later, which keeps it aligned with everything else. Latency never depends on bypass
// or mix, only on which module is in the slot.
//
// A module change crossfades: the outgoing module fades to the (delayed) dry signal over 10 ms, the
// new one is swapped in and fades up from it. An empty slot is a straight passthrough with no
// latency and no work.
class FXSlot
{
public:
    FXSlot() = default;
    ~FXSlot();

    // Message thread, audio stopped. maxLatency bounds any module's latency at this rate.
    void prepare(double sampleRate, int maxBlockSize, int maxLatency);

    // Message thread, audio stopped (prepareToPlay): installs a module directly, no crossfade, and
    // its controls jump to their values on the first block. Anything the slot held is deleted.
    void installNow(std::unique_ptr<FXModule> module);

    // Message thread, audio may be running: offers the next module. Returns a module offered
    // earlier that the audio thread never picked up, for the caller to delete.
    std::unique_ptr<FXModule> offer(std::unique_ptr<FXModule> module);

    // Audio thread: processes stereo in place.
    void process(float* left, float* right, int numSamples, const FX::SlotParams& params,
                 const FX::BlockContext& context, FXRetiredModules& retired) noexcept;

    // Audio thread, or any thread while audio is stopped: the active module's latency, or zero.
    int getLatencySamples() const noexcept;
    FX::ModuleType getActiveType() const noexcept;

private:
    static bool isAudible(const FXModule* module) noexcept
    {
        return module != nullptr && module->getType() != FX::ModuleType::none;
    }

    void deleteEverything();

    std::atomic<FXModule*> incoming { nullptr };
    FXModule* active = nullptr; // audio thread's, once installed
    FXModule* next   = nullptr; // picked up, waiting for `active` to fade out

    juce::SmoothedValue<float> onAmount, mixAmount; // wet = on x mix
    bool moduleNeedsReset = false; // set while bypassed: re-enabling starts from clean state
    bool jumpControls = true;      // after installNow: no fade on the first block

    std::vector<float> dry[2];
    FXDSP::IntegerDelay dryDelay[2];

    JUCE_DECLARE_NON_COPYABLE(FXSlot)
};

// FXEngine — the whole FX section (§7.1): the three mono buses the layers are routed into, Insert A
// and Insert B (six slots each, stereo), the latency alignment between the parallel routes, and the
// Master channel. The mono kick becomes stereo at the inserts' entry.
//
// Latency. Each route's latency is the sum of its slots'; the dry bus and the shorter insert are
// delayed to match the longer, so every route arrives together and a layer split across them stays
// phase-coherent - a kick's sub and click must not smear apart because one went through a
// distortion. The total, alignment plus Master, is what the host is told.
//
// Exactness. With every slot empty and every layer routed Dry, the output is the dry bus copied to
// both channels, bit for bit - nothing is added to it but zeros.
class FXEngine
{
public:
    FXEngine();
    ~FXEngine() = default;

    // Message thread, audio stopped. Sizes everything and rebuilds every slot's module at this rate.
    void prepare(double sampleRate, int maxBlockSize);

    // Message thread. Builds a new module for each slot whose type - or, for a nonlinear module,
    // the oversampling setting - differs from what that slot was last given, and hands it over
    // (installs it directly when audioStopped). Also frees modules the audio thread has retired.
    // Safe to call before prepare(): the request is remembered and built there.
    void setModuleTypes(const std::array<int, FX::kNumSlots>& types, int oversamplingLog2, bool audioStopped);

    // Audio thread. The buses hold getMaxBlockSize() samples each.
    int    getMaxBlockSize() const noexcept { return maxBlock; }
    float* getBus(FX::Bus bus) noexcept     { return buses[(size_t) bus].data(); }
    void   clearBuses(int numSamples) noexcept;

    // Audio thread: runs the inserts and Master on the first numSamples of the buses and writes
    // the stereo result to left/right.
    void process(float* left, float* right, int numSamples, const FX::EngineParams& params) noexcept;

    // Any thread: the total latency the last processed block had.
    int getLatencySamples() const noexcept { return latency.load(std::memory_order_relaxed); }

    // Any thread: a slot control's value after macros, normalised, as the last block used it.
    float getEffectiveValue(int slot, int target) const noexcept;

    // While audio is stopped (tests, prepare): the module type a slot is actually running.
    FX::ModuleType getActiveType(int slot) const noexcept { return slots[(size_t) slot].getActiveType(); }

    // The latency of one oversampled module at a setting, at the prepared rate and block size.
    int getModuleLatency(int oversamplingLog2) const noexcept;

    static int effectiveOversampling(FX::ModuleType type, int oversamplingLog2) noexcept
    {
        return FX::getInfo(type).oversampled ? oversamplingLog2 : 0;
    }

private:
    void processChain(int channel, float* left, float* right, int numSamples,
                      const FX::BlockContext& context) noexcept;
    int  chainLatency(int channel) const noexcept;

    double sampleRate = 0.0;
    int maxBlock = 0;
    std::array<int, FX::kMaxOversamplingLog2 + 1> moduleLatency {};

    std::array<FXSlot, FX::kNumSlots> slots;
    FXRetiredModules retired;

    // What each slot was last asked to run, as (type, oversampling); -1 before the first request.
    struct Request { int type = -1; int oversampling = -1; };
    std::array<Request, FX::kNumSlots> requested;
    int requestedOversampling = 0;

    std::array<std::vector<float>, FX::kNumBuses> buses;
    std::vector<float> insert[2][2]; // [insert][channel]

    FXDSP::IntegerDelay dryAlign;
    FXDSP::IntegerDelay insertAlign[2][2];
    juce::SmoothedValue<float> insertLevel[2];
    bool jumpLevels = true; // after prepare: the first block starts at the set levels, not from 0

    // This block's slot controls after macros. A member so the audio thread never builds it on the
    // stack per block (it is ~700 bytes), and so getEffectiveValue has something to mirror.
    std::array<FX::SlotParams, FX::kNumSlots> effective {};
    std::array<std::atomic<float>, FX::kNumSlots * FX::kNumTargets> effectiveForDisplay;

    std::atomic<int> latency { 0 };

    JUCE_DECLARE_NON_COPYABLE(FXEngine)
};
