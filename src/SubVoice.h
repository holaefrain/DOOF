#pragma once
#include <juce_core/juce_core.h>
#include <cmath>

// SubVoice — monophonic sine oscillator driven by hardcoded pitch and amp envelopes.
//
// Implements the Phase 1 voice: one sine oscillator that sweeps pitch from
// kPitchStartHz down to kPitchEndHz, with a short linear attack and an
// exponential amp decay.  A new note-on while the voice is active triggers
// a short linear choke fade before the new body begins (anti-click retrigger).
//
// Phase 2 replaces the hardcoded envelopes with the node-based lookup-table
// engine, but the state machine and choke logic here persist unchanged.
class SubVoice
{
public:
    // Prepare the voice for playback at the given sample rate.
    // Must be called before any processSample() or noteOn() calls.
    void prepare(double sampleRate);

    // Trigger a new note.  If the voice is already playing, initiates the choke
    // fade and queues midiNote to start after the fade completes.
    // midiNote is stored but not used for pitch in Phase 1; keytracking is Phase 6.
    void noteOn(int midiNote);

    // Advance the voice by one sample and return the mono output.
    // Real-time safe: no allocations, no locks, no branches on heap size.
    float processSample();

    // Returns true when the voice has fully decayed and is producing silence.
    bool isIdle() const { return state == State::Idle; }

private:
    // ── Voice state machine ───────────────────────────────────────────────────
    // Idle:    silent; no CPU work done in processSample.
    // Playing: running pitch + amp envelopes; transitions to Idle when amp falls
    //          below kIdleThreshold, or to Choking on a new noteOn.
    // Choking: linear fade from the amplitude captured at choke-start down to 0
    //          over kChokeFadeSec; then transitions to Playing with the pending note.
    enum class State { Idle, Playing, Choking };

    // ── Hardcoded Phase 1 envelope constants ─────────────────────────────────
    // Pitch envelope: exponential sweep from kPitchStartHz to kPitchEndHz.
    static constexpr double kPitchStartHz  = 150.0;  // frequency at note-on (Hz)
    static constexpr double kPitchEndHz    =  50.0;  // asymptotic frequency after sweep (Hz)
    static constexpr double kPitchDecaySec =  0.08;  // pitch sweep time constant (seconds)

    // Amp envelope: brief linear attack then exponential decay.
    static constexpr double kAmpAttackSec  = 0.002;  // linear ramp-up duration (seconds)
    static constexpr double kAmpDecaySec   = 0.35;   // exponential decay time constant (seconds)

    // Choke fade: anti-click linear ramp-down on retrigger.
    static constexpr double kChokeFadeSec  = 0.005;  // choke fade duration (seconds)

    // Amplitude below which the voice is considered silent and goes Idle.
    static constexpr double kIdleThreshold = 1e-4;   // ~-80 dB

    // ── Runtime state ─────────────────────────────────────────────────────────
    double sampleRate  = 44100.0;
    State  state       = State::Idle;

    // Sine oscillator: phase accumulator in [0, 2π).
    double phase = 0.0;

    // Elapsed time in the current Playing body; drives both pitch and amp envelopes.
    double envTime = 0.0;

    // ── Choke state ───────────────────────────────────────────────────────────
    double chokeTimer = 0.0;   // time elapsed inside the choke fade (seconds)
    double chokeAmp   = 0.0;   // amplitude captured at the moment the choke started
    double chokeFreq  = 0.0;   // frequency held during the choke fade (Hz)
    int    pendingNote = -1;   // MIDI note queued to start once the choke completes

    // ── Private helpers ───────────────────────────────────────────────────────

    // Resets phase and envTime, stores the triggering note, enters Playing state.
    void startBody(int midiNote);

    // Evaluates the pitch envelope at time t (seconds): exponential sweep.
    double pitchAt(double t) const;

    // Evaluates the amp envelope at time t (seconds): linear attack then exp decay.
    double ampAt(double t) const;
};
