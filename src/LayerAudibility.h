#pragma once
#include <array>

// LayerAudibility — the mixer audibility rule from §3.3 of
// project-architecture.md, as a pure function of the flags involved.
//
// Deliberately free of any JUCE dependency: the audio engine, the GUI's
// solo/dim rendering (§3.3: "GUI mirrors engine"), and the lean DOOFTests
// target all call this same function, so the displayed state can never
// disagree with what's actually audible.
namespace LayerAudibility
{
    // Layer count per §3.2. Every per-layer array in DOOF is sized by this.
    static constexpr int kNumLayers = 5;

    // One layer's mixer-relevant flags, read from the APVTS each block.
    struct LayerFlags
    {
        bool isOff = false; // type == Off: layer is not part of the mix at all
        bool mute  = false;
        bool solo  = false;
    };

    // True when at least one layer that is actually in the mix has solo set.
    //
    // Off layers are ignored here: an Off layer produces no sound, so letting
    // its stale solo flag silence every other layer would be a confusing dead
    // end (a silent patch with no visible cause). §3.3's table says only
    // "≥1 layer soloed" and doesn't address type, so this is our reading of
    // it — flipping to the literal any-flag-set version is a one-line change
    // plus the matching assertions in LayerAudibilityTest.
    bool anyLayerSoloed(const std::array<LayerFlags, kNumLayers>& layers);

    // The §3.3 audibility rule. anySoloed comes from anyLayerSoloed() above,
    // computed once across all layers before asking about any single one.
    //
    // | flags       | no layer soloed | >=1 layer soloed |
    // |-------------|-----------------|------------------|
    // | neither     | audible         | silent           |
    // | solo        | audible         | audible          |
    // | mute        | silent          | silent           |
    // | solo + mute | silent          | silent           |  (mute wins)
    //
    // An Off layer is silent in every column, independent of the other flags.
    bool isAudible(bool isOff, bool mute, bool solo, bool anySoloed);

    // Convenience overload for callers already holding a LayerFlags.
    bool isAudible(const LayerFlags& flags, bool anySoloed);

    // How one layer should look, per §3.3's "GUI mirrors engine: soloed layers light up; layers
    // silenced *because* of someone else's solo are dimmed."
    struct Appearance
    {
        bool lit    = false; // this layer is one of the soloed ones
        bool dimmed = false; // audible on its own, but silenced by another layer's solo
    };

    // Lives here rather than in the GUI so the displayed state is derived from the same rule the
    // mixer applies, and the two cannot drift apart. Both fields are defined in terms of
    // isAudible() above for exactly that reason.
    //
    // Note "dimmed" is narrower than "silent": a muted or Off layer is silent too, but it already
    // says so through its own controls, and §3.3 reserves dimming for the case with no local
    // cause. So dimmed means "silent now, but would be audible if nothing were soloed".
    Appearance appearanceFor(const LayerFlags& flags, bool anySoloed);
}
