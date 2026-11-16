#include "SubVoice.h"
#include <algorithm>
#include <iterator>

// Reset all runtime state and store the sample rate.
// Must be called before audio processing begins (e.g. in prepareToPlay).
void SubVoice::prepare(double sr)
{
    sampleRate  = sr;
    state       = State::Idle;
    phase       = 0.0;
    envSamples  = 0;
    hasExceededIdleThreshold = false;
    semitoneShift = 0.0;
    tuneRatio     = 1.0;
    sustainOn     = false;
    currentNote   = -1;
    held          = false;
    pendingHeld   = false;
    wrapCommitted = false;
    rotateRadians  = 0.0;
    lockPending    = false;
    lockCorrecting = false;
    chokeTimer  = 0.0;
    chokeAmp    = 0.0;
    chokeFreq   = 0.0;
    pendingNote = -1;
}

// Handle an incoming note-on event.
// - Idle → directly start the new body.
// - Playing → capture current amp/freq, start choke fade, queue the new note.
// - Choking → update the queued note; the current fade continues uninterrupted.
void SubVoice::noteOn(int midiNote)
{
    if (state == State::Idle)
    {
        startBody(midiNote, true);
    }
    else if (state == State::Playing)
    {
        // Capture the voice's current amplitude and frequency so the choke
        // fade starts seamlessly from the same level the oscillator was at.
        currentEnvelope(chokeAmp, chokeFreq);
        chokeTimer = 0.0;
        pendingNote = midiNote;
        pendingHeld = true;
        state = State::Choking;
    }
    else // Choking — update pending note; let the in-progress fade finish
    {
        pendingNote = midiNote;
        pendingHeld = true;
    }
}

void SubVoice::noteOff(int midiNote)
{
    if (state == State::Choking && midiNote == pendingNote)
        pendingHeld = false; // released before its body even began
    else if (state == State::Playing && midiNote == currentNote)
        held = false;
}

// The current snapshot's sustain loop converted to samples, with its crossfade length. Recomputed
// per sample rather than cached at note-on, so editing the loop while a note is held takes effect
// on the next pass - the same as every other envelope edit.
SubVoice::LoopSamples SubVoice::loopSamples() const
{
    LoopSamples loop;
    if (currentSnapshot == nullptr || ! currentSnapshot->sustainLoop.isValid())
        return loop;

    loop.start = (juce::int64) std::llround(currentSnapshot->sustainLoop.start * sampleRate);
    loop.end   = (juce::int64) std::llround(currentSnapshot->sustainLoop.end   * sampleRate);
    loop.crossfade = std::min((juce::int64) std::llround(kLoopCrossfadeSec * sampleRate),
                              (loop.end - loop.start) / 2);

    // A loop too short to crossfade at all is treated as no loop rather than wrapped hard.
    if (loop.crossfade < 1)
        return {};

    return loop;
}

// Within the last `crossfade` samples before a committed wrap, each value is blended toward where
// the wrap will land - the same distance past the loop start - so that by the jump itself the
// envelope is already reading the loop start's values and nothing steps.
void SubVoice::currentEnvelope(double& amp, double& pitch) const
{
    const double t = envTime();
    amp   = ampAt(t);
    pitch = pitchAt(t);

    if (! wrapCommitted)
        return;

    const auto loop = loopSamples();
    if (loop.end <= loop.start)
        return; // the loop was edited away mid-wrap; play on unblended

    const double into = (double) (envSamples - (loop.end - loop.crossfade)) / (double) loop.crossfade;
    if (into < 0.0)
        return;

    const double landing = (double) (envSamples - (loop.end - loop.start)) / sampleRate;
    const double blend = std::min(into, 1.0);

    amp   += (ampAt(landing)   - amp)   * blend;
    pitch += (pitchAt(landing) - pitch) * blend;
}

// Advance the voice by exactly one sample and return the mono output.
// Choking: linear fade from chokeAmp → 0; transitions to the pending note when done.
// Playing: evaluate envelopes, update oscillator, check for end-of-tail.
// Idle: returns 0 immediately with no computation.
float SubVoice::processSample()
{
    if (state == State::Idle)
        return 0.0f;

    const double dt = 1.0 / sampleRate;
    float output;

    if (state == State::Choking)
    {
        // Clamp fadeProgress to [0, 1] so the last sample always reaches silence.
        double fadeProgress = std::min(chokeTimer / kChokeFadeSec, 1.0);
        output = (float)(waveform(phase, chokeFreq) * chokeAmp * (1.0 - fadeProgress));
        ++noteSamples;

        // Continue advancing the oscillator at the captured choke frequency so
        // there is no phase discontinuity when the new body starts.
        phase += juce::MathConstants<double>::twoPi * chokeFreq / sampleRate;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;

        chokeTimer += dt;

        // Fade complete — start the pending note on the next processSample call.
        if (chokeTimer >= kChokeFadeSec)
            startBody(pendingNote, pendingHeld);
    }
    else // Playing
    {
        const auto loop = loopSamples();
        const bool canLoop = sustainOn && held && loop.end > loop.start;

        // A held note entering the crossfade zone commits to this pass's wrap (see wrapCommitted).
        if (canLoop && ! wrapCommitted
            && envSamples >= loop.end - loop.crossfade && envSamples < loop.end)
            wrapCommitted = true;

        if (lockPending && envSamples == lockWindowStart)
            beginLockCorrection();

        double amp, pitch;
        currentEnvelope(amp, pitch);

        output = (float)(waveform(phase, pitch) * amp);

        double step = juce::MathConstants<double>::twoPi * pitch / sampleRate;
        if (lockCorrecting)
            step *= 1.0 + lockCorrection * lockWindowShape(envSamples);

        // A loop rather than one subtraction: a corrected step can exceed a full turn.
        phase += step;
        while (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;

        ++envSamples;
        ++noteSamples;

        if (lockCorrecting && envSamples >= lockSample)
            lockCorrecting = false; // arrived: from here on the phase runs free from the target

        // The phase accumulator carries straight on across the jump, so the waveform stays
        // continuous; only the envelope position moves.
        if (wrapCommitted && envSamples >= loop.end)
        {
            envSamples -= loop.end - loop.start;
            wrapCommitted = false;

            // A wrap inside the correction window invalidates the steps it was computed from.
            // Re-arm, so the correction is worked out afresh when the envelope next reaches it.
            if (lockCorrecting)
            {
                lockCorrecting = false;
                lockPending = true;
            }
        }

        if (amp >= kIdleThreshold)
            hasExceededIdleThreshold = true;

        // Transition to Idle once the tail has decayed to inaudible levels.
        // hasExceededIdleThreshold guards against an envelope that starts at
        // (or near) zero from satisfying the threshold and killing the voice
        // before it's ever actually sounded. Never while a held note is still
        // looping: a quiet stretch inside the loop is not the end of the tail.
        const bool stillLooping = canLoop && envSamples <= loop.end;
        if (hasExceededIdleThreshold && amp < kIdleThreshold && ! stillLooping)
            state = State::Idle;
    }

    return output;
}

// ── Private helpers ────────────────────────────────────────────────────────────

// Adds up the phase steps the window is about to take, with the same pitchAt and the same
// arithmetic the render loop will use, and solves for the correction that lands the phase on the
// target at the lock node.
void SubVoice::beginLockCorrection()
{
    lockPending = false;

    double natural = 0.0;  // total phase the window advances uncorrected
    double weighted = 0.0; // the same, weighted by the correction's shape
    for (juce::int64 n = lockWindowStart; n < lockSample; ++n)
    {
        const double step = juce::MathConstants<double>::twoPi * pitchAt((double) n / sampleRate) / sampleRate;
        natural  += step;
        weighted += step * lockWindowShape(n);
    }

    // The gap to close, taken the short way round: never more than half a turn either side.
    const double gap = std::remainder(rotateRadians - (phase + natural), juce::MathConstants<double>::twoPi);

    lockCorrection = weighted > 0.0 ? juce::jlimit(kMinLockCorrection, kMaxLockCorrection, gap / weighted)
                                    : 0.0;
    lockCorrecting = true;
}

double SubVoice::lockWindowShape(juce::int64 position) const
{
    const double length = (double) (lockSample - lockWindowStart);
    const double along = (double) (position - lockWindowStart) / length;
    return 0.5 * (1.0 - std::cos(juce::MathConstants<double>::twoPi * along));
}

// sin(phase), plus the harmonic series while it lasts. sin(h * phase) comes from the Chebyshev
// recurrence sin((h+1)x) = 2cos(x)sin(hx) - sin((h-1)x), so the whole series costs one sin and one
// cos per sample instead of one sin per harmonic. Each harmonic is integer-related to the
// fundamental's phase, which keeps the series locked to it through any pitch sweep - and makes a
// phase rotation rotate the whole waveform rather than just its fundamental.
double SubVoice::waveform(double phaseNow, double frequency) const
{
    const double fundamental = std::sin(phaseNow);

    // Checked before anything else so that once the harmonics are over - or were never on - the
    // output is exactly the plain sine, bit for bit.
    if (! harmonicsOn || noteSamples >= harmDecaySamples)
        return fundamental;

    const double envelope = std::exp(-((double) noteSamples / sampleRate) * harmDecayK);

    const double twoCos = 2.0 * std::cos(phaseNow);
    const double fadeStart = sampleRate * kHarmFadeStartRatio;
    const double ceiling   = sampleRate * kHarmCeilingRatio;

    double previous = 0.0;         // sin(0 * phase)
    double current  = fundamental; // sin(1 * phase)
    double series   = 0.0;

    for (int h = 2; h <= kMaxHarmonic; ++h)
    {
        const double next = twoCos * current - previous;
        previous = current;
        current  = next;

        if (harmLevel[h] == 0.0)
            continue;

        const double partial = h * frequency;
        if (partial >= ceiling)
            break; // every higher harmonic is above it too

        const double guard = partial <= fadeStart ? 1.0 : (ceiling - partial) / (ceiling - fadeStart);
        series += harmLevel[h] * guard * current;
    }

    return fundamental + envelope * series;
}

// The drawn pitch at t, moved by the keytracked share of the note's offset and by master tuning.
double SubVoice::pitchAt(double t) const
{
    if (currentSnapshot == nullptr)
        return 0.0;

    double pitch = lookupTable(currentSnapshot->pitchTable, t) * tuneRatio;

    // Skipped entirely when the note is the key root or nothing is flagged, so an untracked patch
    // costs nothing extra per sample.
    if (semitoneShift != 0.0 && ! currentSnapshot->keytrackTable.empty())
    {
        const double weight = lookupTable(currentSnapshot->keytrackTable, t);
        if (weight > 0.0)
            pitch *= std::exp2(weight * semitoneShift / 12.0);
    }

    return std::min(pitch, sampleRate * kMaxFrequencyRatio);
}

// Reads the amp envelope at time t from currentSnapshot's amp table.
double SubVoice::ampAt(double t) const
{
    if (currentSnapshot == nullptr)
        return 0.0;
    return lookupTable(currentSnapshot->ampTable, t);
}

// Linearly interpolated lookup into one of currentSnapshot's tables at
// absolute time t. Clamps to the table's first/last entry outside its domain
// (matches EnvelopeEvaluator::buildTable's own hold-flat-at-the-ends behaviour).
double SubVoice::lookupTable(const std::vector<float>& table, double t) const
{
    const double dt = currentSnapshot->tableDomainSeconds / (double) currentSnapshot->tableSize;
    const double index = t / dt;

    if (index <= 0.0)
        return (double) table[0];

    const int lastIndex = currentSnapshot->tableSize - 1;
    if (index >= (double) lastIndex)
        return (double) table[(size_t) lastIndex];

    const int i0 = (int) index;
    const double frac = index - (double) i0;
    return (double) table[(size_t) i0] * (1.0 - frac) + (double) table[(size_t) i0 + 1] * frac;
}

// Reset the oscillator, envelope clock, and idle-threshold flag, latch this note's tuning, then
// enter Playing state. Phase is set to 0 so sin(0)=0; combined with an envelope whose amp starts
// near zero, this gives a smooth (click-free) start even if the caller skips the choke.
void SubVoice::startBody(int midiNote, bool keyHeld)
{
    semitoneShift = (double) (midiNote - pendingKeyRoot);
    tuneRatio     = std::exp2((double) pendingTuneCents / 1200.0);
    sustainOn     = pendingSustain;
    currentNote   = midiNote;
    held          = keyHeld;
    wrapCommitted = false;

    // Harmonics: each one in the set gets gain x ratio^(its rank in the set), so the first
    // harmonic present is always at `gain` whichever preset is chosen.
    harmonicsOn = pendingHarmGain > 0.0f;
    std::fill(std::begin(harmLevel), std::end(harmLevel), 0.0);

    if (harmonicsOn)
    {
        double level = (double) pendingHarmGain;
        for (int h = 2; h <= kMaxHarmonic; ++h)
        {
            const bool inSet = (pendingHarmSet == HarmonicSet::all)
                            || (pendingHarmSet == HarmonicSet::odd     && (h % 2) == 1)
                            || (pendingHarmSet == HarmonicSet::even    && (h % 2) == 0)
                            || (pendingHarmSet == HarmonicSet::octaves && (h & (h - 1)) == 0);
            if (! inSet)
                continue;

            harmLevel[h] = level;
            level *= (double) pendingHarmRatio;
        }

        // Clamped like ClickVoice's decay: zero would divide by zero, and the ceiling is the
        // parameter's own maximum.
        const double decay = juce::jlimit(0.001, 4.0, pendingHarmDecay);
        harmDecayK = -std::log(kHarmSilenceLevel) / decay;
        harmDecaySamples = (juce::int64) std::ceil(decay * sampleRate);
    }

    noteSamples = 0;

    // Phase: rotated at the start, unless a lock moves the rotation to the lock node.
    rotateRadians = std::remainder((double) pendingRotateDegrees * juce::MathConstants<double>::pi / 180.0,
                                   juce::MathConstants<double>::twoPi);
    if (rotateRadians < 0.0)
        rotateRadians += juce::MathConstants<double>::twoPi;

    lockCorrecting = false;
    lockPending    = false;

    const double lockTime = (currentSnapshot != nullptr) ? currentSnapshot->phaseLockTime : -1.0;
    lockSample = (juce::int64) std::llround(juce::jmax(0.0, lockTime) * sampleRate);

    if (lockTime > 0.0 && lockSample > 0)
    {
        lockWindowStart = juce::jmax((juce::int64) 0,
                                     lockSample - (juce::int64) std::llround(kLockWindowSec * sampleRate));
        lockPending = true;
    }

    // With a lock the note starts from zero - sin(0) = 0, the click-free start the factory amp
    // envelope relies on - and the rotation is applied at the lock node instead.
    phase      = lockPending ? 0.0 : rotateRadians;
    envSamples = 0;
    hasExceededIdleThreshold = false;
    state   = State::Playing;
}
