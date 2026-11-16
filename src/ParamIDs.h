#pragma once
#include <juce_core/juce_core.h>
#include "ClickVoice.h"
#include "SubVoice.h"
#include "PitchSnap.h"

// ParamIDs — the stable APVTS parameter ID strings, in one place.
//
// Shared between PluginProcessor (which declares the parameters) and the GUI
// (which attaches controls to them by ID). Kept in a header rather than in
// PluginProcessor.cpp specifically so the layer strip can't build these
// strings itself: a second copy of the format would be free to drift, and
// per §2 of project-architecture.md an ID that any preset has been saved
// against can never be renamed or removed.
namespace ParamIDs
{
    // Master output gain, applied after the layer mix. Predates the mixer,
    // when it was the single Sub layer's own gain — hence the name, which is
    // now historical but frozen by the never-rename rule above.
    static const juce::String subGain = "sub.gain";

    // ── Tuning (Phase 6, §3.5) ───────────────────────────────────────────────

    // Master tuning in cents: shifts every Sub layer's pitch, keytracked or not, which is what
    // moving the A4 = 440 Hz reference means. One semitone either way; anything further is a
    // transposition, and keytracking already does that.
    static const juce::String masterTune = "master.tune";
    static constexpr float masterTuneMaxCents = 100.0f;

    // The MIDI note at which keytracked nodes play exactly as drawn. C3 (60) by default: Ableton's
    // Drum Rack sends C3 to a pad's instrument unless told otherwise, so a kick there plays as
    // drawn. Settable per patch (user decision, 2026-11-16).
    static const juce::String masterKeyRoot = "master.keyRoot";
    static constexpr int keyRootDefault = 60;

    // Note names as Ableton prints them, where middle C (60) is C3. One implementation, shared with
    // the canvas's pitch readout.
    inline juce::String noteName(int midiNote) { return PitchSnap::noteName(midiNote); }

    // Choice indices for layerN.type. These integers ARE the parameter's
    // stored choice indices, so this order is part of the preset contract and
    // must only ever be appended to.
    enum class LayerType { off = 0, sub = 1, click = 2 };

    // Display names for the choices above; index-aligned with LayerType.
    inline juce::StringArray layerTypeChoices() { return { "Off", "Sub", "Click" }; }

    // Per-layer parameter IDs. layerIndex is the zero-based internal index,
    // but the ID it produces is one-based ("layer1.level" for index 0) to
    // match the layer numbering shown in the GUI.
    inline juce::String layerType  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".type";  }
    inline juce::String layerLevel (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".level"; }
    inline juce::String layerMute  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".mute";  }
    inline juce::String layerSolo  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".solo";  }

    // ── Click layer (Phase 5) ────────────────────────────────────────────────

    // Choice indices for layerN.click.type: the four synthesised flavours, then four sample slots.
    //
    // Permanently eight entries. A Choice parameter's entry list is part of its range, not just a
    // label set, so growing this list later would change the range of a parameter that presets and
    // host automation lanes are already stored against — §2's never-rename rule applied to ranges.
    // Fixing the slot count now means the number of factory samples can change freely without ever
    // touching the parameter contract; a slot with nothing behind it is simply silent.
    enum class ClickType
    {
        tick = 0, noise = 1, snap = 2, thump = 3,
        sample1 = 4, sample2 = 5, sample3 = 6, sample4 = 7
    };

    // Where the sample slots begin, and how many there are. Step 4's library maps its embedded
    // assets against these rather than restating 4 and 8 in another file.
    static constexpr int firstClickSampleSlot = 4;
    static constexpr int numClickSampleSlots  = 4;

    // The first four choice indices are ClickVoice::Type values and are passed straight through to
    // the voice, so the two enums must agree exactly. Asserted here rather than trusted to a
    // comment: reordering either one is otherwise a silent change to what every saved preset means.
    static_assert((int) ClickType::tick  == (int) ClickVoice::Type::tick,  "ClickType/ClickVoice::Type disagree on tick");
    static_assert((int) ClickType::noise == (int) ClickVoice::Type::noise, "ClickType/ClickVoice::Type disagree on noise");
    static_assert((int) ClickType::snap  == (int) ClickVoice::Type::snap,  "ClickType/ClickVoice::Type disagree on snap");
    static_assert((int) ClickType::thump == (int) ClickVoice::Type::thump, "ClickType/ClickVoice::Type disagree on thump");
    static_assert(firstClickSampleSlot == ClickVoice::kNumTypes,
                  "The sample slots must start where the synthesised types end");

    // Display names for the choices above; index-aligned with ClickType.
    inline juce::StringArray clickTypeChoices()
    {
        return { "Tick", "Noise", "Snap", "Thump", "Sample 1", "Sample 2", "Sample 3", "Sample 4" };
    }

    // Decay range, in seconds. §6 describes a click as a 1-20 ms transient; the ceiling sits above
    // that so a click can also be pushed into short-burst territory when layered under a sub.
    // Stored in seconds because that is what ClickVoice takes. ClickPanel shows it in ms, which is
    // display only and never a change to the stored value.
    static constexpr float clickDecayMinSeconds     = 0.001f;
    static constexpr float clickDecayMaxSeconds     = 0.050f;
    static constexpr float clickDecayDefaultSeconds = 0.008f;

    inline juce::String layerClickType  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".click.type";  }
    inline juce::String layerClickTone  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".click.tone";  }
    inline juce::String layerClickDecay (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".click.decay"; }

    // ── Sub layer (Phase 6) ──────────────────────────────────────────────────

    // Sustain mode (§3.5): loop between the keytracked nodes while a note is held, then finish the
    // tail on release.
    inline juce::String layerSubSustain (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.sustain"; }

    // Harmonics (§3.1): a series above the fundamental. Gain is the first harmonic's level against
    // the fundamental, Ratio how quickly each further one falls off, Decay how long they last.
    inline juce::String layerSubHarmGain   (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.harmGain";   }
    inline juce::String layerSubHarmRatio  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.harmRatio";  }
    inline juce::String layerSubHarmDecay  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.harmDecay";  }
    inline juce::String layerSubHarmPreset (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.harmPreset"; }

    // Choice entries for harmPreset, index-aligned with SubVoice::HarmonicSet. Append only - the
    // same never-reorder rule as every other choice list here.
    inline juce::StringArray harmPresetChoices() { return { "All", "Odd", "Even", "Octaves" }; }

    static_assert(SubVoice::kNumHarmonicSets == 4, "harmPresetChoices() has drifted from SubVoice::HarmonicSet");

    // Phase rotation (§3.6), degrees: the sine's phase at the note start, or at the phase-locked
    // node when there is one.
    inline juce::String layerSubPhase (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".sub.phase"; }

    // Decay range, seconds. Skewed around the default, like the click decay: the useful range is
    // short, a long one is occasionally wanted for a buzzy tail.
    static constexpr float subHarmDecayMinSeconds     = 0.005f;
    static constexpr float subHarmDecayMaxSeconds     = 2.0f;
    static constexpr float subHarmDecayDefaultSeconds = 0.15f;
}
