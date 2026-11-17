#pragma once
#include <juce_core/juce_core.h>
#include <array>

// FXModuleInfo — the catalogue of FX modules (§3.7, §7.1) and the shape of the FX section, in one
// place. juce_core only, so the engine, the GUI, the host-facing parameter text and the lean test
// target all read the same definitions.
//
// Every slot carries six generic parameters, p1-p6, stored normalised 0-1 (§7.1: "a fixed generic
// set of slot parameters per slot that each module maps onto"). This file is that mapping: for each
// module, what each of the six means, its unit, its range and its default. A module's DSP reads its
// controls through toValue() with these same specs, so what the GUI shows and what the module does
// can never disagree.
namespace FX
{
    // ── Shape of the FX section ──────────────────────────────────────────────

    // Two inserts and a master (§7.1). The integers are the order the slot parameters are laid
    // out in and the order macro assignments store, so they are part of the preset contract.
    enum class Channel { insertA = 0, insertB = 1, master = 2 };
    constexpr int kNumChannels     = 3;

    // Six slots per channel (user decision, 2026-11-17). More can be appended later; never fewer.
    constexpr int kSlotsPerChannel = 6;
    constexpr int kNumSlots        = kNumChannels * kSlotsPerChannel;

    // p1-p6. A macro can also target a slot's mix, which it addresses as index 6.
    constexpr int kNumSlotParams   = 6;
    constexpr int kMixTarget       = kNumSlotParams;
    constexpr int kNumTargets      = kNumSlotParams + 1;

    constexpr int kNumMacros       = 4;

    inline int     slotIndex(Channel channel, int slotInChannel) { return (int) channel * kSlotsPerChannel + slotInChannel; }
    inline Channel channelOf(int slot)                           { return (Channel) (slot / kSlotsPerChannel); }
    inline int     slotInChannel(int slot)                       { return slot % kSlotsPerChannel; }

    // Display names for the three channels, index-aligned with Channel.
    juce::String channelName(Channel channel);

    // Where each layer's signal goes (§7.1): straight to Master, through one insert, or through both
    // in parallel at full level (user decision, 2026-11-17). These integers are layerN.route's
    // stored choice indices - append only.
    enum class Route { dry = 0, insertA = 1, insertB = 2, both = 3 };
    inline juce::StringArray routeChoices() { return { "Dry", "Insert A", "Insert B", "Both" }; }

    // Global oversampling for the nonlinear modules: the stored choice index is the power of two.
    inline juce::StringArray oversamplingChoices() { return { "Off", "2x", "4x", "8x" }; }
    constexpr int kMaxOversamplingLog2 = 3;

    // ── Modules ──────────────────────────────────────────────────────────────

    // These integers ARE the stored choice indices of every fx.*.slotN.type parameter. The list is
    // fixed at these fourteen for good: a Choice's entry list is part of its range, so appending
    // later would silently remap every automation lane drawn against it (same rule as click.type).
    enum class ModuleType
    {
        none = 0,
        clip, tube, wave, tape, drive,   // distortions (§3.7)
        bitCrusher, ringMod,
        filter, eq,
        compressor, dualComp,
        delay, reverb
    };
    constexpr int kNumModuleTypes = 14;

    juce::StringArray moduleTypeChoices();

    // How a control's value reads.
    enum class Unit { none, decibels, hertz, milliseconds, seconds, percent, ratio, bits, choice };

    // How a normalised 0-1 position maps to the value. Logarithmic for frequencies and times (§0.5's
    // log skew), power for a ratio's long tail.
    enum class Scale { linear, logarithmic, power };

    struct ParamSpec
    {
        const char* name = nullptr; // nullptr: this module does not use the parameter
        Unit  unit  = Unit::none;
        Scale scale = Scale::linear;
        float minValue = 0.0f;
        float maxValue = 1.0f;
        float defaultValue = 0.0f;  // in the control's own units, not normalised
        float exponent = 1.0f;      // Scale::power only

        // Unit::choice and Unit::bits are whole steps: the value is rounded to the nearest one.
        const char* const* choices = nullptr;
        int numChoices = 0;

        bool isUsed() const noexcept     { return name != nullptr; }
        bool isDiscrete() const noexcept { return unit == Unit::choice || unit == Unit::bits; }
    };

    struct ModuleInfo
    {
        const char* name;
        std::array<ParamSpec, kNumSlotParams> params;
        float defaultMix;  // delays and reverbs blend in rather than replacing the kick
        bool  oversampled; // nonlinear: runs at the global oversampling rate
    };

    const ModuleInfo& getInfo(ModuleType type);

    // Normalised 0-1 <-> the control's value, per its spec. Discrete controls round to a step.
    float toValue(const ParamSpec& spec, float normalised);
    float toNormalised(const ParamSpec& spec, float value);
    float defaultNormalised(const ParamSpec& spec);

    // Display text ("2.50 kHz", "-12.0 dB", "4.0:1", "Low Pass"), and the reverse for typed entry,
    // which returns a normalised value. Typed text is read in the control's unit unless it says
    // otherwise: "2k" and "2 kHz" are 2000 Hz, "1.5 s" on a millisecond control is 1500 ms.
    juce::String toText(const ParamSpec& spec, float normalised);
    float        fromText(const ParamSpec& spec, const juce::String& text);

    // The value of a tempo-synced delay division, in quarter-note beats. index is the Division
    // control's choice index.
    double divisionBeats(int index);
    constexpr int kNumDivisions = 15;

    inline float dbToGain(float decibels) { return std::pow(10.0f, decibels * 0.05f); }
}
