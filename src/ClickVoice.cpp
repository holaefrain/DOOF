#include "ClickVoice.h"

// Reset every slot and store the sample rate. Must be called before any processSample() or
// noteOn(), same contract as SubVoice::prepare.
void ClickVoice::prepare(double sr)
{
    sampleRate = sr;

    for (auto& slot : slots)
        slot = Slot{};
}

// Start a new hit, ramping out whatever was already sounding rather than waiting for it to finish
// — see the header for why this differs from SubVoice's serial choke.
void ClickVoice::noteOn(int midiNote)
{
    juce::ignoreUnused(midiNote); // keytracking is Phase 6, same as SubVoice

    // Every hit still sounding starts ramping out. Already-fading hits keep the ramp they are on:
    // restarting it on each retrigger would let a fast pattern hold a hit at a fixed level
    // indefinitely, which is the opposite of what the ramp is for.
    for (auto& slot : slots)
        if (slot.running && ! slot.fading)
        {
            slot.fading   = true;
            slot.fadeTime = 0.0;
        }

    auto& target = slots[(size_t) chooseSlot()];
    target.gen.start(pendingType, pendingTone, pendingDecaySeconds, sampleRate);
    target.running  = true;
    target.fading   = false;
    target.fadeTime = 0.0;
}

// Sum the live hit and any ramping-out hits. Slots that finish are marked free here, so an idle
// voice costs one bool test per slot and nothing else.
float ClickVoice::processSample()
{
    const double dt = 1.0 / sampleRate;
    double out = 0.0;

    for (auto& slot : slots)
    {
        if (! slot.running)
            continue;

        // Tested before rendering rather than after. A hit's amplitude at decaySeconds is exactly
        // kSilenceLevel — still non-zero — so rendering the sample and only then noticing would
        // put one audible sample beyond the stated duration. Checking first makes the decay
        // control an exact duration: the first silent sample is ceil(decaySeconds * sampleRate).
        if (slot.gen.isFinished())
        {
            slot.running = false;
            continue;
        }

        double sample = (double) slot.gen.render();

        if (slot.fading)
        {
            // Clamped to [0, 1] so the last sample of the ramp always reaches exactly silence,
            // the same guarantee SubVoice's choke fade makes.
            const double progress = std::min(slot.fadeTime / kFadeOutSeconds, 1.0);
            sample *= (1.0 - progress);
            slot.fadeTime += dt;

            if (slot.fadeTime >= kFadeOutSeconds)
                slot.running = false;
        }

        out += sample;
    }

    return (float) out;
}

bool ClickVoice::isIdle() const
{
    for (const auto& slot : slots)
        if (slot.running)
            return false;

    return true;
}

// Free slot if there is one; otherwise the hit furthest through its ramp-down, which is the
// quietest thing to displace. A non-fading hit is never chosen over a fading one, since it is the
// loudest thing in the pool.
int ClickVoice::chooseSlot() const
{
    int best = 0;
    double bestProgress = -1.0;

    for (int i = 0; i < kNumSlots; ++i)
    {
        const auto& slot = slots[(size_t) i];

        if (! slot.running)
            return i;

        const double progress = slot.fading ? slot.fadeTime : 0.0;
        if (progress > bestProgress)
        {
            bestProgress = progress;
            best = i;
        }
    }

    return best;
}

// ── Generator ─────────────────────────────────────────────────────────────────

// Latch the controls into a fresh hit. Everything derived from tone and decay is computed once
// here rather than per sample, so render() stays a handful of multiplies.
void ClickVoice::Generator::start(Type typeToUse, float toneNormalised, double decay, double sr)
{
    type       = typeToUse;
    sampleRate = sr;
    samplesElapsed = 0;
    lpState    = 0.0;
    bandState  = 0.0;
    impulseSpent = false;

    // Thump starts a quarter cycle in, at the sine's peak rather than at its zero crossing. From
    // zero it would fade in over the first quarter cycle — 0.8 ms at the bottom of the tone range,
    // a large fraction of the whole hit — which is a swell, not a transient. Starting at the peak
    // makes it a click, and its level then stays consistent across the tone sweep instead of
    // depending on how much of a cycle fits inside the decay.
    phase = (typeToUse == Type::thump) ? juce::MathConstants<double>::halfPi : 0.0;

    // Re-seeded to the same value on every hit, which is what makes two identical triggers render
    // identically — the property the null tests depend on.
    rng.setSeed(kNoiseSeed);

    // Clamped rather than trusted: a zero decay would divide by zero below, and a huge one would
    // leave the slot rendering long past anything a click should.
    const double clampedDecay = juce::jlimit(kMinDecaySeconds, kMaxDecaySeconds, decay);

    // amp = exp(-elapsed * decayK), chosen so amp is exactly kSilenceLevel at clampedDecay. That
    // makes the control a real duration instead of a time constant with an open-ended tail.
    decayK = -std::log(kSilenceLevel) / clampedDecay;

    // Rounded up, so a duration that falls between two samples still renders the one just before
    // it rather than stopping early.
    decaySamples = (int) std::ceil(clampedDecay * sampleRate);

    const double tone = (double) juce::jlimit(0.0f, 1.0f, toneNormalised);

    // Exponential sweep, so the control feels even across its travel instead of crowding the
    // whole useful range into the top of the knob.
    const double cutoff = kMinCutoffHz * std::pow(kMaxCutoffHz / kMinCutoffHz, tone);

    // Standard one-pole: y += a * (x - y), with a set from the cutoff and sample rate so the same
    // tone setting means the same corner frequency at 44.1, 48 or 96 kHz.
    lpCoeff = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * cutoff / sampleRate);

    // Snap subtracts a second one-pole two octaves below the first, leaving the band between them.
    bandCoeff = 1.0 - std::exp(-juce::MathConstants<double>::twoPi * (cutoff * 0.25) / sampleRate);

    sineFreq = kMinThumpHz * std::pow(kMaxThumpHz / kMinThumpHz, tone);

    switch (type)
    {
        case Type::tick:
            // The banded impulse response peaks at roughly lpCoeff on its first sample, which
            // would make a dark tick far quieter than a bright one. Dividing by it holds the peak
            // near 1.0 across the whole tone sweep.
            levelScale = 1.0 / lpCoeff;
            break;

        case Type::noise:
        case Type::snap:
            // White noise through a one-pole comes out with variance scaled by a / (2 - a), so the
            // inverse square root restores it and tone changes brightness rather than volume.
            // Capped because a very dark setting would otherwise ask for enormous gain.
            levelScale = std::min(std::sqrt((2.0 - lpCoeff) / lpCoeff), kMaxToneCompensation);
            break;

        case Type::thump:
            // A unit sine needs no correction; tone moves its frequency, not its level.
            levelScale = 1.0;
            break;
    }
}

// One sample of this hit: a source shaped by the tone filter, times the exponential fall.
float ClickVoice::Generator::render()
{
    // Derived from the sample count rather than accumulated, so it cannot drift away from the
    // duration isFinished() is measuring against.
    const double elapsedSeconds = (double) samplesElapsed / sampleRate;
    const double amp = std::exp(-elapsedSeconds * decayK);

    double source = 0.0;

    switch (type)
    {
        case Type::tick:
        {
            // One sample of impulse, then silence — the filter's own ringing is the click.
            const double impulse = impulseSpent ? 0.0 : 1.0;
            impulseSpent = true;

            lpState += lpCoeff * (impulse - lpState);

            // Banded the same way snap is, and for the same reason. A bare one-pole's impulse
            // response never changes sign, so it is a DC lump rather than a transient: it would
            // arrive at the DC blocker as something to remove rather than as a click. Subtracting
            // the lower corner leaves a bipolar tick with actual edge to it.
            bandState += bandCoeff * (lpState - bandState);
            source = lpState - bandState;
            break;
        }

        case Type::noise:
        {
            // nextFloat() is [0, 1); shifted to [-1, 1) so the burst has no DC component of its
            // own for the processor's DC blocker to have to remove.
            const double white = (double) rng.nextFloat() * 2.0 - 1.0;
            lpState += lpCoeff * (white - lpState);
            source = lpState;
            break;
        }

        case Type::snap:
        {
            const double white = (double) rng.nextFloat() * 2.0 - 1.0;
            lpState += lpCoeff * (white - lpState);

            // Everything below the second, lower corner is subtracted away, leaving the band
            // between the two — narrower and more focused than the plain noise burst.
            bandState += bandCoeff * (lpState - bandState);
            source = lpState - bandState;
            break;
        }

        case Type::thump:
        {
            source = std::sin(phase);

            phase += juce::MathConstants<double>::twoPi * sineFreq / sampleRate;
            if (phase >= juce::MathConstants<double>::twoPi)
                phase -= juce::MathConstants<double>::twoPi;
            break;
        }
    }

    ++samplesElapsed;

    return (float) (source * levelScale * amp);
}
