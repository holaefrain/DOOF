#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Layer.h"
#include "ClickSampleLibrary.h"
#include "LayerAudibility.h"
#include "LayerViewPrefs.h"
#include "FXEngine.h"
#include "MacroAssignments.h"
#include <array>
#include <memory>

// DOOFAudioProcessor — the audio engine and plugin host.
// Owns the signal path and satisfies the JUCE AudioProcessor contract so the
// plugin loads in any VST3/Standalone host.  All synthesis, mixing, and state
// serialisation live here or in classes it owns.  The editor is a separate
// object that attaches and detaches independently.
//
// Signal path (Phase 7, §7.1):
//   MIDI → five layers (mono) → each summed into its route's bus (Dry / Insert A / Insert B / both)
//        → FXEngine: inserts (stereo from here on), route alignment, Master FX
//        → master gain → DC blocker → out
class DOOFAudioProcessor : public juce::AudioProcessor,
                           private juce::Timer
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
    // MIDI dispatch is sample-accurate: the block is rendered in spans between events, so a
    // note-on at sample N sounds at sample N rather than at the start of the block.
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
    // The longest FX tail the current slots can produce (reverb decay, delay repeats), kept current
    // by the FX poll below. Zero with no delay or reverb in any slot.
    double getTailLengthSeconds() const override { return fxTailSeconds.load(std::memory_order_relaxed); }

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

    // One layer's canvas view settings. Lives here rather than in the editor because the host may
    // close and reopen the editor at any point, and these are saved with the preset. Returned by
    // reference so the editor can write straight through as the user changes them.
    LayerViewPrefs&       getLayerViewPrefs(int index);
    const LayerViewPrefs& getLayerViewPrefs(int index) const;

    // The decoded factory click samples behind click.type's Sample 1-4 entries. The const overload
    // is what the click panel uses to name each slot. The non-const one exists so integration
    // tests can load a generated WAV through the real engine, since resources/clicks/ is normally
    // empty - and is bound by loadSlotFromMemory's own rule: message thread, audio not running.
    ClickSampleLibrary&       getClickSamples()       { return clickSamples; }
    const ClickSampleLibrary& getClickSamples() const { return clickSamples; }

    // ── FX (Phase 7) ─────────────────────────────────────────────────────────

    // The FX section. Read by the FX page (effective values for its macro display) and by tests.
    const FXEngine& getFXEngine() const { return fx; }

    // The four macros' assignments - structured state the FX page edits directly. Message thread.
    MacroAssignments& getMacroAssignments() { return macroAssignments; }

    // Message thread: brings the FX engine's modules in line with the slot type and oversampling
    // parameters, and the reported latency and tail with the result. Runs on a ~30 Hz poll, since
    // the types can change by host automation as well as by the GUI; tests call it directly.
    void updateFXModules();

    // Message thread: puts a module in a slot the way the FX page does - the type, switched on,
    // with that module's own defaults for its six controls and its mix - each as a host gesture.
    // The slot's macro assignments are cleared: they addressed the outgoing module's controls.
    void setFXSlotModule(int slot, FX::ModuleType type);

    // Message thread: swaps everything about two slots - type, on, mix, the six controls and the
    // macro assignments (user decision 2026-11-17: settings and assignments travel with the module).
    void swapFXSlots(int slotA, int slotB);

private:
    // Builds the parameter layout passed to the APVTS constructor.
    // Parameter IDs follow the stable namespaced format defined in §2 of project-reference.md.
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // The skewed seconds range shared by every layer's click.decay parameter. A function rather
    // than a constant because NormalisableRange's skew is set after construction; public-adjacent
    // callers (the Phase 5 panel, tests) go through the parameter itself, so this stays private.
    static juce::NormalisableRange<float> clickDecayRange();

    // Likewise for every layer's sub.harmDecay.
    static juce::NormalisableRange<float> harmDecayRange();

    // Renders one contiguous span: every layer's voice ticked and its output, through its smoothed
    // gain, added to its route's FX bus starting at busOffset. processBlock calls this once per gap
    // between MIDI events, which is what makes note-on dispatch sample-accurate. A zero-length span
    // is a valid no-op.
    void renderLayers(int busOffset, int numSamples);

    // After the FX: master gain, then the DC blocker, per channel.
    void applyOutputStage(float* left, float* right, int numSamples, float gain);

    // Reads every FX parameter for this block into fxParams. Audio thread: atomics only.
    void readFXParams();

    // The FX poll (updateFXModules).
    void timerCallback() override;
    static constexpr int kFXPollHz = 30;

    // Reads the curve-tagged ENVELOPE children of `parent` into layer `index`'s two models.
    // Shared by both load paths, which differ only in which tree the envelopes hang off.
    void loadEnvelopesInto(int index, const juce::ValueTree& parent);

    // Puts one layer's envelopes back to the out-of-the-box shapes, clearing any existing nodes
    // and properties first. Used at construction and when a preset predates that layer.
    void seedLayerWithDefaults(int index);

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

    // Factory click samples, decoded once here on the message thread. Declared before the layers
    // below for the same reason as the undo manager: their voices are handed pointers into it, so
    // it must be constructed first and destroyed last. C++ constructs members in declaration order
    // regardless of initialiser list order.
    ClickSampleLibrary clickSamples;

    // The five layers (§3.2), each owning its own envelope models, publisher
    // and voice. Held by unique_ptr because Layer is neither default-
    // constructible nor movable (its publisher holds references to the two
    // models beside it), so a plain std::array<Layer, N> can't be formed;
    // these are allocated once in the constructor, never on the audio thread.
    std::array<std::unique_ptr<Layer>, LayerAudibility::kNumLayers> layers;

    // Per-layer canvas view settings, saved and restored alongside each layer's envelopes. Plain
    // values rather than parameters: they change nothing audible, so exposing them for host
    // automation would only clutter the parameter list.
    std::array<LayerViewPrefs, LayerAudibility::kNumLayers> layerViewPrefs;

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

        // Phase 5's click controls. Cached for the same reason as the four above — these are read
        // once per block to configure the layer's ClickVoice, and a string lookup per layer per
        // block would be pure waste in the audio callback.
        std::atomic<float>* clickType  = nullptr;
        std::atomic<float>* clickTone  = nullptr;
        std::atomic<float>* clickDecay = nullptr;

        // Phase 6's Sub controls, likewise read once per block and latched by SubVoice at note-on.
        std::atomic<float>* subSustain    = nullptr;
        std::atomic<float>* subHarmGain   = nullptr;
        std::atomic<float>* subHarmRatio  = nullptr;
        std::atomic<float>* subHarmDecay  = nullptr;
        std::atomic<float>* subHarmPreset = nullptr;
        std::atomic<float>* subPhase      = nullptr;

        // Phase 7: which FX bus the layer feeds.
        std::atomic<float>* route = nullptr;
    };

    std::array<LayerParams, LayerAudibility::kNumLayers> layerParams;
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* masterTuneParam = nullptr; // master.tune, cents (Phase 6)
    std::atomic<float>* keyRootParam    = nullptr; // master.keyRoot, MIDI note (Phase 6)

    // Phase 7's FX parameters, cached for the same reason.
    struct SlotParamPointers
    {
        std::atomic<float>* type = nullptr;
        std::atomic<float>* on   = nullptr;
        std::atomic<float>* mix  = nullptr;
        std::array<std::atomic<float>*, FX::kNumSlotParams> values {};
    };
    std::array<SlotParamPointers, FX::kNumSlots> slotParams;
    std::array<std::atomic<float>*, 2> insertLevelParams {};
    std::array<std::atomic<float>*, FX::kNumMacros> macroParams {};
    std::atomic<float>* oversamplingParam = nullptr;

    // The FX section, the macros' assignments, and this block's FX parameters - a member so the
    // audio thread never builds it on its stack.
    FXEngine fx;
    MacroAssignments macroAssignments;
    FX::EngineParams fxParams;

    // Kept current by updateFXModules, read by getTailLengthSeconds from any thread.
    std::atomic<double> fxTailSeconds { 0.0 };

    // The host's tempo as the audio thread last read it, for the message thread's tail estimate.
    std::atomic<double> hostBpm { 120.0 };

    // Each layer's route for the current block, resolved once per block like layerSource.
    std::array<FX::Route, LayerAudibility::kNumLayers> layerRoute {};
    // Which of a layer's two voices its output is taken from for the current block.
    //
    // `silent` is not the same as an Off layer. It means a Click layer pointed at one of the four
    // reserved sample slots with no content behind it — audible as far as §3.3's mute/solo rule is
    // concerned, but with nothing to play. Step 4 replaces that case with real sample playback;
    // until then, and whenever a slot is genuinely empty, an empty slot is simply silent rather
    // than falling back to some other sound.
    enum class LayerSource : unsigned char { sub, click, silent };

    // Resolved once per block in processBlock, never per sample: reading the type parameter inside
    // the render loop would both cost an atomic load per layer per sample and let the answer
    // change halfway through a block.
    std::array<LayerSource, LayerAudibility::kNumLayers> layerSource;

    // Reads one layer's click parameters into its ClickVoice and returns which voice the layer
    // should be mixed from. Called once per layer per block, on the audio thread.
    LayerSource configureLayerSource(int index);

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

    // DC blocker state variables, one pair per output channel.
    // Implements y[n] = x[n] - x[n-1] + R * y[n-1] — a one-pole high-pass (~10 Hz) that
    // removes any DC offset introduced by the synthesis or FX chain. The very last stage, so DC an
    // asymmetric distortion generates is removed too.
    std::array<float, 2> dcBlockerX {}; // previous input sample
    std::array<float, 2> dcBlockerY {}; // previous output sample
    float dcBlockerR = 0.0f; // coefficient, computed from sample rate in prepareToPlay

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessor)
};
