#pragma once
#include "FXModuleInfo.h"
#include <array>
#include <memory>

namespace FX
{
    // What a module needs to know about the block it is processing, beyond its own controls.
    struct BlockContext
    {
        double bpm = 120.0; // host tempo, for the delay's synced divisions; 120 when the host has none
    };

    using SlotValues = std::array<float, kNumSlotParams>; // p1-p6, normalised 0-1
}

// FXModule — one module in an FX slot (§7.1's "common FXModule interface (prepare/process/reset/
// param list) makes slots interchangeable"). Stereo throughout: the FX domain is stereo end to end,
// with the mono kick copied to both channels at the FX entry (§7.1).
//
// Threading. A module is built and prepared on the message thread, then handed to the audio thread
// (FXSlot) and never touched by the message thread again until it is retired. So prepare() may
// allocate; everything else runs on the audio thread and must not.
//
// Structure is fixed at construction: the module type and the oversampling factor never change for
// a given object. Changing either builds a new module, which is what keeps the latency of a module
// constant for its whole life - the engine compensates for latency, and a latency that could move
// underneath it would make every parallel route drift.
//
// Bypass and the wet/dry mix are not the module's business: FXSlot does both, identically for every
// module, so "bypass is bit-identical" (§6) is proven once rather than thirteen times.
class FXModule
{
public:
    explicit FXModule(FX::ModuleType moduleType) : type(moduleType) {}
    virtual ~FXModule() = default;

    FX::ModuleType getType() const noexcept { return type; }

    // Message thread, before the audio thread ever sees the module. Allocates whatever the module
    // needs at this rate and block size, and leaves it reset.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;

    // Audio thread: clears all internal state - delay lines, filter memories, envelopes - without
    // allocating. The next setParameters() jumps straight to its values instead of gliding from
    // wherever the controls were before.
    virtual void reset() noexcept = 0;

    // Samples of delay the module adds, at the session rate. Fixed from prepare() on.
    virtual int getLatencySamples() const noexcept { return 0; }

    // Audio thread, once per block before process(): the slot's six controls, normalised, with
    // macros already applied. Modules smooth whatever would otherwise zipper.
    virtual void setParameters(const FX::SlotValues& normalised, const FX::BlockContext& context) noexcept = 0;

    // Audio thread: processes stereo audio in place. numSamples never exceeds prepare()'s maxBlockSize.
    virtual void process(float* left, float* right, int numSamples) noexcept = 0;

protected:
    // A control's value in its own units, read through the shared catalogue so the DSP and the
    // display can never disagree about what a knob position means.
    float valueOf(const FX::SlotValues& normalised, int index) const noexcept
    {
        return FX::toValue(FX::getInfo(type).params[(size_t) index], normalised[(size_t) index]);
    }

private:
    const FX::ModuleType type;

    JUCE_DECLARE_NON_COPYABLE(FXModule)
};

namespace FX
{
    // Builds an unprepared module. ModuleType::none gives a module that passes audio through
    // untouched, which is how an emptied slot is handed to the audio thread. oversamplingLog2 is
    // ignored by the modules that are not oversampled, so it never forces them to be rebuilt.
    std::unique_ptr<FXModule> createModule(ModuleType type, int oversamplingLog2);

    // The latency one oversampled module adds at a given oversampling setting. The same for every
    // nonlinear module, since all of them use the same up/down filters. Message thread: it builds
    // a filter pair to measure.
    int oversamplingLatency(int oversamplingLog2, int maxBlockSize);
}
