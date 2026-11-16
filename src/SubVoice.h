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

    // Tuning for the next note (Phase 6 §3.5). keyRootNote is the MIDI note at which keytracked
    // nodes play exactly as drawn; every semitone the played note sits above or below it moves
    // them by a semitone. masterTuneCents shifts the whole sub - every node, keytracked or not -
    // by that many cents, which is what moving the A4 = 440 Hz reference means.
    //
    // Latched at the start of each note, never read mid-note, so a hit already sounding cannot be
    // retuned underneath itself - the same rule ClickVoice applies to its controls. Safe to call
    // from the audio thread (plain stores); PluginProcessor sets it once per block.
    void setTuning(int keyRootNote, float masterTuneCents)
    {
        pendingKeyRoot = keyRootNote;
        pendingTuneCents = masterTuneCents;
    }

    // Sustain mode for the next note (§3.5): while the key is held, loop the envelopes between the
    // first and last keytracked nodes, then finish the tail once it is released. Does nothing for a
    // patch with fewer than two keytracked nodes. Latched at note-on, like setTuning.
    void setSustain(bool enabled) { pendingSustain = enabled; }

    // Release a note. Only the note now sounding, or the one queued behind a choke, is released: a
    // note-off for an older key that has already been choked away is ignored, so in monophonic
    // playing, letting go of the previous key never cuts the new note short. Without sustain mode
    // this changes nothing - the envelope plays to its end whether or not the key is held.
    void noteOff(int midiNote);

    // Trigger a new note.  If the voice is already playing, initiates the choke
    // fade and queues midiNote to start after the fade completes. The note's
    // distance from the key root drives the keytracked nodes (see setTuning).
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

    // The envelope read position in samples. Advances one per sample and jumps back by the loop
    // length at a sustain wrap. Envelope time is derived from it rather than accumulated, since
    // summing 1/sampleRate drifts (the same fix ClickVoice needed for its decay).
    juce::int64 envSamples = 0;

    // ── Sustain (Phase 6) ─────────────────────────────────────────────────────
    bool pendingSustain = false; // applied at the next startBody
    bool sustainOn      = false; // latched for the note now playing
    int  currentNote    = -1;    // the note now playing, for matching its note-off
    bool held           = false; // its key is still down
    bool pendingHeld    = false; // the key queued behind a choke is still down

    // True once a held note has entered the crossfade at the end of the loop. The wrap is then
    // carried through even if the key is released mid-crossfade: abandoning it halfway would step
    // the envelope back from the blend to the raw value, which is the click the blend exists to
    // prevent. Release then plays on from the loop start, through the loop end, into the tail.
    bool wrapCommitted = false;

    // Length of the crossfade into each wrap. Long enough that a loop whose ends differ in level
    // does not click, short against any loop worth holding; capped at half the loop so a very short
    // loop still has a stretch that plays unblended.
    static constexpr double kLoopCrossfadeSec = 0.010;

    // The loop in samples at the current sample rate, or end <= start when there is none.
    struct LoopSamples { juce::int64 start = 0, end = 0, crossfade = 0; };
    LoopSamples loopSamples() const;

    // Amp and pitch at the current envelope position, blended toward the loop start while a
    // committed wrap is under way.
    void currentEnvelope(double& amp, double& pitch) const;

    // Envelope time for the sample about to be rendered, in seconds.
    double envTime() const { return (double) envSamples / sampleRate; }

    // ── Tuning (Phase 6) ──────────────────────────────────────────────────────
    int   pendingKeyRoot   = 60;   // applied at the next startBody
    float pendingTuneCents = 0.0f; // likewise

    // Latched by startBody for the note now playing.
    double semitoneShift = 0.0; // played note minus key root
    double tuneRatio     = 1.0; // 2^(cents / 1200)

    // Ceiling on the oscillator frequency, as a fraction of the sample rate. A note keytracked far
    // above the root can push a node past Nyquist, where a sine folds back as an unrelated pitch;
    // held just under it instead. Far above anything a kick sweep reaches at a sane root.
    static constexpr double kMaxFrequencyRatio = 0.45;

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

    // Resets phase, the envelope clock and the idle-threshold flag, latches the tuning for
    // midiNote, enters Playing state.
    void startBody(int midiNote, bool keyHeld);

    // The frequency the oscillator plays at time t (seconds): the drawn pitch envelope, with the
    // keytracked share of the note's offset from the key root and the master tuning applied.
    // Returns 0.0 if no snapshot has been set yet.
    double pitchAt(double t) const;

    // Evaluates the amp envelope at time t (seconds) via currentSnapshot's
    // amp table. Returns 0.0 if no snapshot has been set yet.
    double ampAt(double t) const;

    // Linearly interpolated lookup into one of currentSnapshot's tables at
    // absolute time t; clamps to the table's first/last entry outside its
    // domain. currentSnapshot must be non-null when this is called.
    double lookupTable(const std::vector<float>& table, double t) const;
};
