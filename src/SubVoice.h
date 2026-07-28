#pragma once
#include <juce_core/juce_core.h>
#include "EnvelopeSnapshot.h"
#include <cmath>

// SubVoice — monophonic sine oscillator driven by table-based pitch and amp
// envelopes (§2, §3.1 of project-architecture.md).
//
// The pitch and amp envelopes are read from an EnvelopeSnapshot handed in via
// setSnapshot() — never a live EnvelopeModel/ValueTree (§2's cardinal rule:
// the audio thread never touches the live tree). The snapshot pointer is
// expected to be loaded once per block by the caller (PluginProcessor) and
// held for the whole block; SubVoice itself just reads whatever pointer it
// was last given.
//
// A new note-on while the voice is active triggers a short linear choke fade
// before the new body begins (anti-click retrigger).
class SubVoice
{
public:
    // Prepare the voice for playback at the given sample rate.
    // Must be called before any processSample() or noteOn() calls.
    void prepare(double sampleRate);

    // Sets the envelope snapshot this voice reads from. Real-time safe: just
    // stores a pointer, no allocation, no locking. Call once per block, before
    // any processSample() calls for that block (§2: "loads that pointer once
    // per block").
    void setSnapshot(const EnvelopeSnapshot* snapshot) { currentSnapshot = snapshot; }

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
    // Playing: running pitch + amp envelopes; transitions to Idle once amp has
    //          risen above kIdleThreshold at least once and then falls back
    //          below it, or to Choking on a new noteOn.
    // Choking: linear fade from the amplitude captured at choke-start down to 0
    //          over kChokeFadeSec; then transitions to Playing with the pending note.
    enum class State { Idle, Playing, Choking };

    // Choke fade: anti-click linear ramp-down on retrigger.
    static constexpr double kChokeFadeSec  = 0.005;  // choke fade duration (seconds)

    // Amplitude below which the voice is considered silent and goes Idle.
    static constexpr double kIdleThreshold = 1e-4;   // ~-80 dB

    // ── Runtime state ─────────────────────────────────────────────────────────
    double sampleRate  = 44100.0;
    State  state       = State::Idle;

    const EnvelopeSnapshot* currentSnapshot = nullptr;

    // Sine oscillator: phase accumulator in [0, 2π).
    double phase = 0.0;

    // Elapsed time in the current Playing body; drives both pitch and amp envelopes.
    double envTime = 0.0;

    // Set once amp has risen above kIdleThreshold since the current body
    // started. Guards against an envelope whose amp starts at (or near) zero
    // — e.g. a rising attack — from tripping the idle check before it's ever
    // actually sounded, without assuming any fixed attack duration.
    bool hasExceededIdleThreshold = false;

    // ── Choke state ───────────────────────────────────────────────────────────
    double chokeTimer = 0.0;   // time elapsed inside the choke fade (seconds)
    double chokeAmp   = 0.0;   // amplitude captured at the moment the choke started
    double chokeFreq  = 0.0;   // frequency held during the choke fade (Hz)
    int    pendingNote = -1;   // MIDI note queued to start once the choke completes

    // ── Private helpers ───────────────────────────────────────────────────────

    // Resets phase, envTime, and the idle-threshold flag, stores the
    // triggering note, enters Playing state.
    void startBody(int midiNote);

    // Evaluates the pitch envelope at time t (seconds) via currentSnapshot's
    // pitch table. Returns 0.0 if no snapshot has been set yet.
    double pitchAt(double t) const;

    // Evaluates the amp envelope at time t (seconds) via currentSnapshot's
    // amp table. Returns 0.0 if no snapshot has been set yet.
    double ampAt(double t) const;

    // Linearly interpolated lookup into one of currentSnapshot's tables at
    // absolute time t; clamps to the table's first/last entry outside its
    // domain. currentSnapshot must be non-null when this is called.
    double lookupTable(const std::vector<float>& table, double t) const;
};
