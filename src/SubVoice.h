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
    // Which harmonics the series uses (§3.1: "Sub harmonics ... plus presets"). The numbering
    // becomes the layerN.sub.harmPreset choice list, so like ClickVoice::Type it is append-only.
    enum class HarmonicSet
    {
        all = 0, // 2nd through 8th
        odd,     // 3rd, 5th, 7th - hollow, square-ish
        even,    // 2nd, 4th, 6th, 8th - fuller, octave-heavy
        octaves  // 2nd, 4th, 8th only - the cleanest reinforcement
    };
    static constexpr int kNumHarmonicSets = 4;

    // Highest harmonic the series reaches. Eight keeps a kick's harmonics in the range where they
    // add presence rather than fizz, and bounds the per-sample cost.
    static constexpr int kMaxHarmonic = 8;

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

    // Harmonics for the next note (§3.1). gain is the level of the first harmonic in the set,
    // relative to the fundamental; each further one in the set is ratio times the one before;
    // decaySeconds is how long they take to die away, after which the sound is the clean sine
    // alone - a real duration, the same semantics as ClickVoice's decay. A gain of 0 skips the
    // harmonics entirely. Latched at note-on, like setTuning.
    void setHarmonics(float gain, float ratio, double decaySeconds, HarmonicSet set)
    {
        pendingHarmGain = gain;
        pendingHarmRatio = ratio;
        pendingHarmDecay = decaySeconds;
        pendingHarmSet = set;
    }

    // Phase rotation for the next note (§3.6), in degrees: the phase the sine has at its reference
    // point. With no phase-locked node the reference is the note's start; with one, it is the lock
    // node, and the note starts from zero phase instead (see the lock state below). Latched at
    // note-on, like setTuning.
    void setPhaseRotation(float degrees) { pendingRotateDegrees = degrees; }

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

    // ── Harmonics (Phase 6) ───────────────────────────────────────────────────
    float       pendingHarmGain  = 0.0f;
    float       pendingHarmRatio = 0.5f;
    double      pendingHarmDecay = 0.15;
    HarmonicSet pendingHarmSet   = HarmonicSet::all;

    // Latched by startBody: each harmonic's level (index = harmonic number; 0 and 1 unused, and 0
    // for any harmonic outside the set), the decay rate, and how many samples the harmonics last.
    double      harmLevel[kMaxHarmonic + 1] {};
    bool        harmonicsOn      = false;
    double      harmDecayK       = 0.0;
    juce::int64 harmDecaySamples = 0;

    // Samples since this note's body began. Unlike envSamples it never jumps back at a sustain
    // wrap, so the harmonics keep fading through a held loop rather than restarting each pass.
    juce::int64 noteSamples = 0;

    // Level at which the harmonics are considered gone, matching kIdleThreshold (~-80 dB).
    static constexpr double kHarmSilenceLevel = 1e-4;

    // Where a harmonic starts fading out, and where it is gone, as fractions of the sample rate.
    // Above the ceiling a harmonic would alias back down as an unrelated tone; the fade keeps a
    // pitch sweep from switching one off with a step.
    static constexpr double kHarmFadeStartRatio = 0.40;
    static constexpr double kHarmCeilingRatio   = 0.45;

    // The oscillator's output at `phase` before the amp envelope: the sine, plus the harmonic
    // series for as long as it lasts. `frequency` is the fundamental, for the aliasing guard.
    double waveform(double phase, double frequency) const;

    // ── Phase rotate + lock (Phase 6) ─────────────────────────────────────────
    float  pendingRotateDegrees = 0.0f;
    double rotateRadians        = 0.0; // latched for the note now playing

    // Phase lock (§3.6: "lock phase from a chosen node onward; edits before it don't disturb
    // downstream phase alignment"). The phase is made to arrive at the lock node at exactly
    // rotateRadians, so everything after the node depends only on the envelope after it.
    //
    // Smooth correction, by user decision: over a window ending at the lock node, every phase
    // step is scaled by 1 + c * hann(position in window). At the window's start the voice adds up
    // exactly the steps it is about to take - the same per-sample arithmetic, not an estimate -
    // works out how far short of or past the target they would land, and picks c to close the
    // gap. The Hann shape means the pitch eases away and back rather than stepping, and the gap
    // is never more than half a cycle, since the target only has to match modulo a full turn.
    bool        lockPending     = false; // this note has a lock still to reach
    bool        lockCorrecting  = false; // inside the correction window now
    juce::int64 lockSample      = 0;     // envelope position where the phase must equal the target
    juce::int64 lockWindowStart = 0;     // where the correction window begins
    double      lockCorrection  = 0.0;   // c above

    // Longest correction window. Longer spreads the correction thinner (a smaller pitch bend);
    // 100 ms holds at least five cycles of anything a kick sweeps through, so a half-cycle
    // correction is at most a ~10% bend at its peak and far less on average. A lock node earlier
    // than this uses everything before it.
    static constexpr double kLockWindowSec = 0.1;

    // Bounds on c. In a window too short or too low to hold a cycle, closing the gap exactly could
    // take the pitch to zero or negative; it is clamped instead, and the lock lands short.
    static constexpr double kMinLockCorrection = -0.75;
    static constexpr double kMaxLockCorrection = 3.0;

    // Works out lockCorrection at the start of the window and enters it.
    void beginLockCorrection();

    // The correction's shape at an envelope position inside the window, in [0, 1].
    double lockWindowShape(juce::int64 position) const;

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
