#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cmath>

// ClickVoice — the transient half of a kick (§6 Phase 5: "Click playback — built-in clicks,
// noise, basic texture; per-click level/tone").
//
// Deliberately juce_core only, exactly the constraint SubVoice.h holds, so it compiles into the
// lean DOOFTests target for fast day-to-day iteration. That rules out juce_dsp, so the tone
// control is a hand-written one-pole rather than a StateVariableFilter — ample for a 1-20 ms
// transient, and one fewer dependency in the hottest code path.
//
// Every hit is reproducible: the noise generator is re-seeded to the same fixed value on each
// note-on, and type/tone/decay are latched at note-on rather than read live per sample. A real
// click sample is identical every trigger, so a synthesised one should be too — and determinism
// is what makes null-testing the engine possible at all.
//
// Retrigger differs from SubVoice's choke on purpose; see noteOn().
class ClickVoice
{
public:
    // The four synthesised click flavours, covering §6's "built-in clicks, noise, basic texture".
    //
    // The numbering is load-bearing from Step 3a onward: these become the first four entries of
    // the layerN.click.type Choice parameter, and a Choice's entry list is part of its range, so
    // it can never be reordered or renumbered once a preset exists — §2's never-rename rule
    // applied to ranges. Append only.
    enum class Type
    {
        tick = 0,   // filtered impulse: the sharpest of the four, a beater on a hard head
        noise,      // white burst through the tone filter: broadband air
        snap,       // band-passed noise: narrower and more focused than noise
        thump       // short sine blip well above the sub: body rather than air
    };

    // How many entries Type has. Step 3a's parameter list needs it, and tests iterate on it, so
    // neither has to restate the count and drift from this enum.
    static constexpr int kNumTypes = 4;

    // Prepare the voice for playback at the given sample rate and silence it.
    // Must be called before any processSample() or noteOn(), same contract as SubVoice::prepare.
    void prepare(double sampleRate);

    // ── Per-hit controls ──────────────────────────────────────────────────────
    // All three are latched into the hit by noteOn() rather than read per sample, so changing one
    // mid-click cannot warp a transient already in flight. Safe to call from the audio thread
    // (plain stores, no allocation); PluginProcessor sets them once per block from the APVTS.

    // Which of the four flavours the next hit uses.
    void setType(Type type) { pendingType = type; }

    // Brightness, normalised 0 (dark) to 1 (bright). Drives the one-pole cutoff for tick/noise/
    // snap, and the blip frequency for thump. Clamped on latch, so an out-of-range value from a
    // misconfigured parameter cannot produce an unstable filter.
    void setTone(float toneNormalised) { pendingTone = toneNormalised; }

    // How long the hit takes to fall to silence, in seconds. The amplitude falls exponentially and
    // reaches kSilenceLevel exactly at this time, at which point the hit stops rendering — so this
    // is a real duration, not a time constant with an open-ended tail.
    void setDecaySeconds(double seconds) { pendingDecaySeconds = seconds; }

    // Trigger a new hit, latching the current type/tone/decay.
    //
    // Retriggering does NOT work like SubVoice's choke, and the difference matters. SubVoice fades
    // the outgoing note to silence and only then starts the new one, which delays the retrigger by
    // kChokeFadeSec. On a sub's slow attack that is inaudible; on a click it would be fatal — 5 ms
    // is longer than the entire transient, and it would throw away the sample-accurate onset that
    // Phase 5 Step 1 exists to deliver. So the fade runs in parallel instead: the new hit starts
    // immediately at its correct sample while the outgoing one ramps down beside it. Same
    // anti-click guarantee, no timing cost.
    //
    // midiNote is accepted and ignored, matching SubVoice — keytracking is Phase 6.
    void noteOn(int midiNote);

    // Advance the voice by one sample and return the mono output: the live hit plus any hits still
    // ramping out. Real-time safe — no allocations, no locks, fixed-size state.
    float processSample();

    // True once every hit has finished and the voice is producing silence. Mirrors
    // SubVoice::isIdle so the two voices can be reasoned about the same way.
    bool isIdle() const;

private:
    // ── Tuning constants ──────────────────────────────────────────────────────

    // Ramp-down applied to an outgoing hit on retrigger. Matches SubVoice's kChokeFadeSec, which
    // is already known to be short enough not to soften a transient and long enough to avoid a
    // click. Unlike SubVoice's, this one overlaps the new hit rather than preceding it.
    static constexpr double kFadeOutSeconds = 0.005;

    // Amplitude at which a hit is considered finished. Matches SubVoice::kIdleThreshold (~-80 dB)
    // so both voices agree on what silence means.
    static constexpr double kSilenceLevel = 1e-4;

    // Cutoff range the tone control sweeps, for tick/noise/snap. Mapped exponentially, so the
    // control feels even across its travel rather than crowding everything into the top end.
    static constexpr double kMinCutoffHz = 200.0;
    static constexpr double kMaxCutoffHz = 12000.0;

    // Frequency range the tone control sweeps for thump. Lower than the cutoff range above
    // because this is an audible pitch rather than a filter corner, but still well clear of the
    // sub layer it sits on top of.
    static constexpr double kMinThumpHz = 300.0;
    static constexpr double kMaxThumpHz = 4000.0;

    // Ceiling on the make-up gain that keeps noise level roughly constant across the tone sweep
    // (see Generator::start). Without a cap, a very dark setting would ask for enormous gain on a
    // heavily filtered signal.
    static constexpr double kMaxToneCompensation = 8.0;

    // Guard rails on the latched decay, so a zero or negative value cannot divide by zero and an
    // absurd one cannot leave a hit rendering forever.
    static constexpr double kMinDecaySeconds = 0.0002;
    static constexpr double kMaxDecaySeconds = 1.0;

    // Fixed seed the noise generator is reset to at the start of every hit. Any value works; what
    // matters is that it never changes, since that is what makes two identical triggers render
    // identically.
    static constexpr juce::int64 kNoiseSeed = 0x0D00F0C11CB1;

    // How many hits can be ramping out at once, on top of the live one. Four covers retriggering
    // up to roughly 800 Hz before the quietest ramp has to be dropped early — far past anything a
    // kick pattern produces, and still only a handful of small structs, allocated once.
    static constexpr int kNumSlots = 5;

    // ── One hit ───────────────────────────────────────────────────────────────
    // All the state a single click needs, so an outgoing hit can be left running untouched in its
    // own slot while a new one starts. Copyable by design (juce::Random is a bare int64 seed),
    // which is what lets a slot be snapshotted rather than re-derived.
    struct Generator
    {
        // Latch the controls and reset every piece of per-hit state. Called once per trigger.
        void start(Type typeToUse, float toneNormalised, double decaySeconds, double sr);

        // Advance one sample and return this hit's contribution, envelope included.
        float render();

        // True once the hit has rendered its full duration. Counted in samples rather than
        // compared against elapsed seconds: accumulating 1/sampleRate drifts, and at 20 ms /
        // 44.1 kHz the 882 additions land just short of 0.02, leaving the hit sounding one sample
        // past where it was asked to stop.
        bool isFinished() const { return samplesElapsed >= decaySamples; }

        Type   type       = Type::tick;  // flavour latched at trigger
        double sampleRate = 44100.0;     // cached at trigger so render() needs no outside lookup
        int    samplesElapsed = 0;       // samples rendered in this hit; the exact clock
        int    decaySamples   = 0;       // total samples this hit lasts, ceil(decay * sampleRate)
        double decayK     = 0.0;         // fall rate, amp = exp(-elapsed seconds * decayK)

        double lpCoeff    = 0.0;         // one-pole coefficient at the tone cutoff
        double lpState    = 0.0;         // that filter's running output
        double bandCoeff  = 0.0;         // second, lower one-pole; snap subtracts it to make a band
        double bandState  = 0.0;         // that filter's running output

        double phase      = 0.0;         // thump's sine phase, in [0, 2pi)
        double sineFreq   = 0.0;         // thump's frequency in Hz, from tone
        double levelScale = 1.0;         // per-type make-up so tone changes brightness, not volume
        bool   impulseSpent = false;     // tick's impulse is one sample wide; this fires it once

        juce::Random rng;                // re-seeded to kNoiseSeed by start(), never elsewhere
    };

    // One hit per slot: slot 0 is not special, the live hit is wherever noteOn last put it.
    struct Slot
    {
        Generator gen;              // the hit itself
        bool   running = false;     // false means the slot is free
        bool   fading  = false;     // true once a later noteOn has started ramping this one out
        double fadeTime = 0.0;      // seconds elapsed in that ramp
    };

    // Picks where a new hit goes: a free slot if there is one, otherwise the hit furthest through
    // its ramp-down, which is the quietest thing to displace.
    int chooseSlot() const;

    std::array<Slot, kNumSlots> slots;   // fixed pool, sized at compile time — never allocated
    double sampleRate = 44100.0;         // cached from prepare() for the fade timing

    // Controls awaiting the next trigger. Held separately from the running hits so setting one
    // mid-click cannot reach into a transient already in flight.
    Type   pendingType = Type::tick;
    float  pendingTone = 0.5f;
    double pendingDecaySeconds = 0.005;
};
