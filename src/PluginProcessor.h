#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "SubVoice.h"
#include "EnvelopeModel.h"
#include "EnvelopePublisher.h"

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

    // Read-only access for the editor's canvas (Phase 3 Step 2). Non-const
    // access for direct editing arrives with Step 3's node interaction.
    const EnvelopeModel& getPitchEnvelopeModel() const { return pitchEnvelopeModel; }
    const EnvelopeModel& getAmpEnvelopeModel()   const { return ampEnvelopeModel; }

private:
    // Builds the parameter layout passed to the APVTS constructor.
    // Parameter IDs follow the stable namespaced format defined in §2 of project-reference.md.
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // The single mono voice: one sine oscillator with pitch/amp envelopes and choke logic.
    SubVoice voice;

    // Shared undo history for both envelope models below (Phase 3 §3.4: "Undo
    // / redo, 30 steps"), so a single undo reverts the most recent edit
    // chronologically, regardless of whether it touched pitch or amp.
    // UndoManager(1, 30): the tiny 1-unit budget is exceeded almost
    // immediately by any transaction, which forces JUCE's trimming logic to
    // constantly drop the oldest transaction down to the 30-transaction
    // floor — a real hard cap, not just a hint (see juce_UndoManager.cpp's
    // dropOldTransactionsIfTooLarge()).
    //
    // Declaration order matters: this must be constructed before the two
    // models below, which take a pointer to it in their constructor calls —
    // C++ constructs members in declaration order regardless of initializer
    // list order.
    juce::UndoManager envelopeUndoManager { 1, 30 };

    // Node-based pitch and amp envelope data (§3.1). Message-thread only — the
    // audio thread never touches these directly, only the snapshots published
    // through envelopePublisher below.
    //
    // Declaration order matters: envelopePublisher stores references to these
    // two models, so they must be declared (and therefore constructed) after
    // envelopeUndoManager but before envelopePublisher — reversing either
    // would leave a dangling reference during construction.
    EnvelopeModel pitchEnvelopeModel { &envelopeUndoManager };
    EnvelopeModel ampEnvelopeModel   { &envelopeUndoManager };

    // Bridges the two models above to the audio thread: rebuilds and publishes
    // a new EnvelopeSnapshot via atomic-pointer swap on every edit (§2).
    EnvelopePublisher envelopePublisher { pitchEnvelopeModel, ampEnvelopeModel };

    // DC blocker state variables.
    // Implements y[n] = x[n] - x[n-1] + R * y[n-1] — a one-pole high-pass (~10 Hz) that
    // removes any DC offset introduced by the synthesis or FX chain.
    float dcBlockerX = 0.0f; // previous input sample
    float dcBlockerY = 0.0f; // previous output sample
    float dcBlockerR = 0.0f; // coefficient, computed from sample rate in prepareToPlay

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessor)
};
