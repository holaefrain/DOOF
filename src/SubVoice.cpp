#include "SubVoice.h"

// Reset all runtime state and store the sample rate.
// Must be called before audio processing begins (e.g. in prepareToPlay).
void SubVoice::prepare(double sr)
{
    sampleRate  = sr;
    state       = State::Idle;
    phase       = 0.0;
    envTime     = 0.0;
    hasExceededIdleThreshold = false;
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
        startBody(midiNote);
    }
    else if (state == State::Playing)
    {
        // Capture the voice's current amplitude and frequency so the choke
        // fade starts seamlessly from the same level the oscillator was at.
        chokeAmp   = ampAt(envTime);
        chokeFreq  = pitchAt(envTime);
        chokeTimer = 0.0;
        pendingNote = midiNote;
        state = State::Choking;
    }
    else // Choking — update pending note; let the in-progress fade finish
    {
        pendingNote = midiNote;
    }
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
        output = (float)(std::sin(phase) * chokeAmp * (1.0 - fadeProgress));

        // Continue advancing the oscillator at the captured choke frequency so
        // there is no phase discontinuity when the new body starts.
        phase += juce::MathConstants<double>::twoPi * chokeFreq / sampleRate;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;

        chokeTimer += dt;

        // Fade complete — start the pending note on the next processSample call.
        if (chokeTimer >= kChokeFadeSec)
            startBody(pendingNote);
    }
    else // Playing
    {
        double amp   = ampAt(envTime);
        double pitch = pitchAt(envTime);

        output = (float)(std::sin(phase) * amp);

        phase += juce::MathConstants<double>::twoPi * pitch / sampleRate;
        if (phase >= juce::MathConstants<double>::twoPi)
            phase -= juce::MathConstants<double>::twoPi;

        envTime += dt;

        if (amp >= kIdleThreshold)
            hasExceededIdleThreshold = true;

        // Transition to Idle once the tail has decayed to inaudible levels.
        // hasExceededIdleThreshold guards against an envelope that starts at
        // (or near) zero from satisfying the threshold and killing the voice
        // before it's ever actually sounded.
        if (hasExceededIdleThreshold && amp < kIdleThreshold)
            state = State::Idle;
    }

    return output;
}

// ── Private helpers ────────────────────────────────────────────────────────────

// Reads the pitch envelope at time t from currentSnapshot's pitch table.
double SubVoice::pitchAt(double t) const
{
    if (currentSnapshot == nullptr)
        return 0.0;
    return lookupTable(currentSnapshot->pitchTable, t);
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

// Reset the oscillator, envelope timer, and idle-threshold flag, then enter
// Playing state. Phase is set to 0 so sin(0)=0; combined with an envelope
// whose amp starts near zero, this gives a smooth (click-free) start even if
// the caller skips the choke.
// midiNote is stored for future keytracking (Phase 6); not used for pitch yet.
void SubVoice::startBody(int midiNote)
{
    juce::ignoreUnused(midiNote); // keytracking added in Phase 6
    phase   = 0.0;
    envTime = 0.0;
    hasExceededIdleThreshold = false;
    state   = State::Playing;
}
