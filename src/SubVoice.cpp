#include "SubVoice.h"

// Reset all runtime state and store the sample rate.
// Must be called before audio processing begins (e.g. in prepareToPlay).
void SubVoice::prepare(double sr)
{
    sampleRate  = sr;
    state       = State::Idle;
    phase       = 0.0;
    envTime     = 0.0;
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

        // Transition to Idle once the tail has decayed to inaudible levels.
        // The envTime guard prevents the attack phase (where amp starts at 0.0)
        // from satisfying the threshold and killing the voice before it starts.
        if (envTime >= kAmpAttackSec && amp < kIdleThreshold)
            state = State::Idle;
    }

    return output;
}

// ── Private helpers ────────────────────────────────────────────────────────────

// Exponential frequency sweep: starts at kPitchStartHz and decays toward kPitchEndHz.
// The decay rate is set by kPitchDecaySec (one time constant).
double SubVoice::pitchAt(double t) const
{
    return kPitchEndHz + (kPitchStartHz - kPitchEndHz) * std::exp(-t / kPitchDecaySec);
}

// Amp envelope: linear ramp over kAmpAttackSec (prevents a hard click at note-on),
// followed by an exponential decay toward silence.
double SubVoice::ampAt(double t) const
{
    if (t < kAmpAttackSec)
        return t / kAmpAttackSec;                                   // linear attack
    return std::exp(-(t - kAmpAttackSec) / kAmpDecaySec);          // exponential decay
}

// Reset the oscillator and envelope timer, then enter Playing state.
// Phase is set to 0 so sin(0)=0; combined with the linear attack this
// gives a smooth (click-free) start even if the caller skips the choke.
// midiNote is stored for future keytracking (Phase 6); not used for pitch yet.
void SubVoice::startBody(int midiNote)
{
    juce::ignoreUnused(midiNote); // keytracking added in Phase 6
    phase   = 0.0;
    envTime = 0.0;
    state   = State::Playing;
}
