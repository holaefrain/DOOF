#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Layer.h"
#include "LayerAudibility.h"
#include <array>
#include <memory>

// DOOFAudioProcessor — the audio engine and plugin host.
// Owns the signal path and satisfies the JUCE AudioProcessor contract so the
// plugin loads in any VST3/Standalone host.  All synthesis, mixing, and state
// serialisation live here or in classes it owns.  The editor is a separate
// object that attaches and detaches independently.
//
// Phase 1 signal path (mono, then copied to L/R):
//   MIDI note-on → SubVoice (sine + hardcoded pitch/amp envelopes) → gain → DC blocker → out
class DOOFAudioProcessor : public juce::AudioProcessor
{
public:
    DOOFAudioProcessor();
    ~DOOFAudioProcessor() override;

    // Called by the host before playback begins; prepares the voice and DC blocker.
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;

    // Called by the host when playback stops; release any resources allocated in prepareToPlay.
    void releaseResources() override;

    // Returns true if the requested channel layout is supported.
    // DOOF requires stereo out; the sidechain input (Phase 10 TRIGGER) is optional mono.
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    // Main audio callback — processes one block of audio and MIDI each cycle.
    // Must be real-time safe: no allocations, no locks, no I/O.
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // Creates and returns the GUI editor window; called by the host on UI open.
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ── Plugin identity ──────────────────────────────────────────────────────
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }   // MIDI notes trigger the engine
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; } // updated when reverb added (Phase 7)

    // ── Program/preset stubs (full browser added in Phase 11) ───────────────
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    // Serialises the current plugin state into a binary blob for host save/restore.
    void getStateInformation(juce::MemoryBlock& destData) override;

    // Restores plugin state from a blob previously written by getStateInformation.
    void setStateInformation(const void* data, int sizeInBytes) override;

    // APVTS provides thread-safe parameter access and host automation support.
    // Declared public so the editor can attach sliders/buttons directly via attachment objects.
    juce::AudioProcessorValueTreeState apvts;

    // Access to one layer's models and voice (§3.2). index must be in
    // [0, LayerAudibility::kNumLayers). Non-const access lets the editor's
    // canvas edit that layer's nodes directly (Phase 3 Step 3's
    // drag/add/delete/reshape interaction); overload resolution picks whichever
    // the caller's own constness allows.
    Layer&       getLayer(int index);
    const Layer& getLayer(int index) const;

    // Number of layers, so callers (the layer strip, tests) don't hardcode it.
    static constexpr int getNumLayers() { return LayerAudibility::kNumLayers; }

private:
    // Builds the parameter layout passed to the APVTS constructor.
    // Parameter IDs follow the stable namespaced format defined in §2 of project-reference.md.
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Reads the curve-tagged ENVELOPE children of `parent` into layer `index`'s two models.
    // Shared by both load paths, which differ only in which tree the envelopes hang off.
    void loadEnvelopesInto(int index, const juce::ValueTree& parent);

    // Puts one layer's envelopes back to the out-of-the-box shapes, clearing any existing nodes
    // and properties first. Used at construction and when a preset predates that layer.
    void seedLayerWithDefaults(int index);

    // Returns every per-layer mixer parameter to its default, master gain excepted. Needed before
    // loading a pre-mixer preset; see the call site in setStateInformation for why.
    void resetLayerParametersToDefaults();

    // Shared undo history for every layer's envelope models (Phase 3 §3.4: "Undo
    // / redo, 30 steps"), so a single undo reverts the most recent edit
    // chronologically, regardless of whether it touched pitch or amp.
    // UndoManager(1, 30): the tiny 1-unit budget is exceeded almost
    // immediately by any transaction, which forces JUCE's trimming logic to
    // constantly drop the oldest transaction down to the 30-transaction
    // floor — a real hard cap, not just a hint (see juce_UndoManager.cpp's
    // dropOldTransactionsIfTooLarge()).
    //
    // Declaration order matters: this must be constructed before the layers
    // below, whose models take a pointer to it — C++ constructs members in
    // declaration order regardless of initializer list order.
    juce::UndoManager envelopeUndoManager { 1, 30 };

    // The five layers (§3.2), each owning its own envelope models, publisher
    // and voice. Held by unique_ptr because Layer is neither default-
    // constructible nor movable (its publisher holds references to the two
    // models beside it), so a plain std::array<Layer, N> can't be formed;
    // these are allocated once in the constructor, never on the audio thread.
    std::array<std::unique_ptr<Layer>, LayerAudibility::kNumLayers> layers;

    // Cached APVTS raw-value pointers, resolved once in the constructor.
    //
    // apvts.getRawParameterValue(id) hashes a string to find the parameter,
    // which is fine on the message thread but wasteful in processBlock — with
    // 21 parameters the mixer would do that lookup 21 times per block, forever.
    // The pointers APVTS hands back are stable for the processor's lifetime, so
    // resolving them once and reading the atomics directly is both faster and
    // free of any string work on the audio thread.
    struct LayerParams
    {
        std::atomic<float>* type  = nullptr;
        std::atomic<float>* level = nullptr;
        std::atomic<float>* mute  = nullptr;
        std::atomic<float>* solo  = nullptr;
    };

    std::array<LayerParams, LayerAudibility::kNumLayers> layerParams;
    std::atomic<float>* masterGainParam = nullptr;

    // Reads one layer's mixer flags from the cached pointers above. Audio-thread
    // safe: plain atomic loads, no strings, no allocation. Choice and bool
    // parameters both surface as floats here, so type is compared against the
    // ParamIDs::LayerType index and the bools against 0.5 as a midpoint.
    LayerAudibility::LayerFlags getLayerFlags(int index) const;

    // Whether any layer in the mix has solo engaged, per §3.3's multi-solo
    // rule. Computed across all layers, so it must be evaluated once before
    // asking about any individual layer.
    bool isAnyLayerSoloed() const;

    // The gain a layer should be heading toward: its level parameter when
    // audible per §3.3, otherwise silence. Shared by prepareToPlay (which sets
    // it instantly) and processBlock (which ramps to it), so the two can't
    // disagree about what a layer's gain ought to be.
    float getTargetGain(int index, bool anySoloed) const;

    // Per-layer output gain: the layer's level when audible, 0 when not,
    // ramped rather than stepped. Without the ramp, toggling mute while a tail
    // rings would cut the waveform mid-cycle (an audible click), and automating
    // a level would produce zipper noise. Lives here rather than in SubVoice
    // because SubVoice.h deliberately includes only juce_core, so it stays
    // testable in the lean DOOFTests target — SmoothedValue is juce_audio_basics.
    std::array<juce::SmoothedValue<float>, LayerAudibility::kNumLayers> layerGain;

    // Ramp length for the gains above. Matches SubVoice's own choke fade, which
    // is already known to be short enough not to soften a kick's transient and
    // long enough to avoid a click.
    static constexpr double kGainRampSeconds = 0.005;

    // DC blocker state variables.
    // Implements y[n] = x[n] - x[n-1] + R * y[n-1] — a one-pole high-pass (~10 Hz) that
    // removes any DC offset introduced by the synthesis or FX chain.
    float dcBlockerX = 0.0f; // previous input sample
    float dcBlockerY = 0.0f; // previous output sample
    float dcBlockerR = 0.0f; // coefficient, computed from sample rate in prepareToPlay

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessor)
};
