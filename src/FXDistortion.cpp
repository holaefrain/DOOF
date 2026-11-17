#include "FXModuleBase.h"
#include <cmath>

// The nonlinear modules (§3.7): Clip, Tube, Wave, Tape and Drive distortion, and the Ring
// Modulator. All six oversample (FXModuleBase's Oversampled), since all six create frequencies the
// input did not have and would alias them back down without it.
//
// Static offsets are removed analytically rather than with a DC-blocking filter: each shaper's
// output for a silent input is subtracted, so silence stays silence and a symmetric setting adds no
// phase shift at all to the sub. What DC a heavily asymmetric setting still generates is removed
// by the DC blocker at the end of the whole chain.
namespace FXModules
{
namespace
{
    // Sets each smoother's ramp for the rate it is advanced at - the oversampled rate inside
    // shape(), the session rate outside it.
    void prepareSmoothers(double rate, std::initializer_list<Linear*> smoothers)
    {
        for (auto* smoothed : smoothers)
            smoothed->reset(rate, kSmoothingSeconds);
    }

    // ── Clip ─────────────────────────────────────────────────────────────────
    // Drive into a clipper whose ceiling is the output peak at a symmetric setting. Softness blends
    // the hard clip toward a tanh curve with the same ceiling; Bias shifts the input against the
    // ceiling, so one side clips first (asymmetric, adding even harmonics).
    class Clip final : public Oversampled
    {
    public:
        explicit Clip(int oversamplingLog2) : Oversampled(FX::ModuleType::clip, oversamplingLog2) {}

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(drive,    FX::dbToGain(valueOf(n, 0)), jump);
            moveTo(ceiling,  FX::dbToGain(valueOf(n, 1)), jump);
            moveTo(softness, valueOf(n, 2) / 100.0f, jump);
            moveTo(bias,     valueOf(n, 3) / 100.0f, jump);
            moveTo(output,   FX::dbToGain(valueOf(n, 4)), jump);
        }

    private:
        static float clip(float v, float c, float soft) noexcept
        {
            const float hard = juce::jlimit(-c, c, v);
            const float smooth = c * std::tanh(v / c);
            return hard + soft * (smooth - hard);
        }

        void prepareModule(double sessionRate, double shapingRate, int) override
        {
            prepareSmoothers(shapingRate, { &drive, &ceiling, &softness, &bias });
            prepareSmoothers(sessionRate, { &output });
        }

        void resetModule() noexcept override {}

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = drive.getNextValue();
                const float c = ceiling.getNextValue();
                const float s = softness.getNextValue();
                const float offset = bias.getNextValue() * c;
                const float rest = clip(offset, c, s);

                left[i]  = clip(g * left[i]  + offset, c, s) - rest;
                right[i] = clip(g * right[i] + offset, c, s) - rest;
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float o = output.getNextValue();
                left[i]  *= o;
                right[i] *= o;
            }
        }

        Linear drive, ceiling, softness, bias, output;
    };

    // ── Tube ─────────────────────────────────────────────────────────────────
    // A biased tanh, tanh(gx + b) - tanh(b): the bias moves the operating point off the curve's
    // centre the way a valve's grid bias does, so the two half-waves saturate differently and even
    // harmonics appear. Tone is a gentle one-pole roll-off after the stage.
    class Tube final : public Oversampled
    {
    public:
        explicit Tube(int oversamplingLog2) : Oversampled(FX::ModuleType::tube, oversamplingLog2) {}

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(drive,  FX::dbToGain(valueOf(n, 0)), jump);
            moveTo(bias,   valueOf(n, 1) / 100.0f, jump);
            moveTo(tone,   valueOf(n, 2), jump);
            moveTo(output, FX::dbToGain(valueOf(n, 3)), jump);
        }

    private:
        void prepareModule(double sessionRate, double shapingRate, int) override
        {
            prepareSmoothers(shapingRate, { &drive, &bias });
            prepareSmoothers(sessionRate, { &output });
            tone.reset(sessionRate, kSmoothingSeconds);
        }

        void resetModule() noexcept override
        {
            for (auto& filter : toneFilter)
                filter.reset();
        }

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = drive.getNextValue();
                const float b = bias.getNextValue();
                const float rest = std::tanh(b);

                left[i]  = std::tanh(g * left[i]  + b) - rest;
                right[i] = std::tanh(g * right[i] + b) - rest;
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (tone.isSmoothing() || i == 0)
                {
                    const double hz = tone.getNextValue();
                    toneFilter[0].setCutoff(hz, baseRate);
                    toneFilter[1].setCutoff(hz, baseRate);
                }

                const float o = output.getNextValue();
                left[i]  = toneFilter[0].process(left[i])  * o;
                right[i] = toneFilter[1].process(right[i]) * o;
            }
        }

        Linear drive, bias, output;
        Multiplicative tone;
        FXDSP::OnePoleLowpass toneFilter[2];
    };

    // ── Wave ─────────────────────────────────────────────────────────────────
    // A wavefolder: past its threshold the waveform folds back on itself instead of flattening,
    // which keeps adding harmonics as the drive rises rather than saturating to a square. Shape
    // morphs a smooth sine fold into a sharper triangle fold; Symmetry offsets the input so the two
    // half-waves fold at different points.
    class Wave final : public Oversampled
    {
    public:
        explicit Wave(int oversamplingLog2) : Oversampled(FX::ModuleType::wave, oversamplingLog2) {}

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(fold,     FX::dbToGain(valueOf(n, 0)), jump);
            moveTo(symmetry, valueOf(n, 1) / 100.0f, jump);
            moveTo(shapeMix, valueOf(n, 2) / 100.0f, jump);
            moveTo(tone,     valueOf(n, 3), jump);
            moveTo(output,   FX::dbToGain(valueOf(n, 4)), jump);
        }

        // Folds to within [-1, 1] for any input. Both curves have unit slope at zero.
        static float folder(float u, float triangleAmount) noexcept
        {
            float t = std::fmod(u + 1.0f, 4.0f);
            if (t < 0.0f)
                t += 4.0f;
            const float triangle = t < 2.0f ? t - 1.0f : 3.0f - t;
            const float sine = std::sin(u);
            return sine + triangleAmount * (triangle - sine);
        }

    private:
        void prepareModule(double sessionRate, double shapingRate, int) override
        {
            prepareSmoothers(shapingRate, { &fold, &symmetry, &shapeMix });
            prepareSmoothers(sessionRate, { &output });
            tone.reset(sessionRate, kSmoothingSeconds);
        }

        void resetModule() noexcept override
        {
            for (auto& filter : toneFilter)
                filter.reset();
        }

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = fold.getNextValue();
                const float s = symmetry.getNextValue();
                const float m = shapeMix.getNextValue();
                const float rest = folder(s, m);

                left[i]  = folder(g * left[i]  + s, m) - rest;
                right[i] = folder(g * right[i] + s, m) - rest;
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            // The fold's top end is bright by nature; at the default 20 kHz the tone filter is open.
            for (int i = 0; i < numSamples; ++i)
            {
                if (tone.isSmoothing() || i == 0)
                {
                    const double hz = tone.getNextValue();
                    toneFilter[0].setCutoff(hz, baseRate);
                    toneFilter[1].setCutoff(hz, baseRate);
                }

                const float o = output.getNextValue();
                left[i]  = toneFilter[0].process(left[i])  * o;
                right[i] = toneFilter[1].process(right[i]) * o;
            }
        }

        Linear fold, symmetry, shapeMix, output;
        Multiplicative tone;
        FXDSP::OnePoleLowpass toneFilter[2];
    };

    // ── Tape ─────────────────────────────────────────────────────────────────
    // Soft saturation x / sqrt(1 + x^2) - gentler than tanh, never quite flat - then the playback
    // head's low "bump" (a bell at 80 Hz) and the high-frequency loss of tape (a 2-pole low-pass).
    class Tape final : public Oversampled
    {
    public:
        explicit Tape(int oversamplingLog2) : Oversampled(FX::ModuleType::tape, oversamplingLog2) {}

        static constexpr double kBumpHz = 80.0;
        static constexpr double kBumpQ  = 0.9;

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(drive,  FX::dbToGain(valueOf(n, 0)), jump);
            moveTo(bump,   valueOf(n, 1), jump);
            moveTo(tone,   valueOf(n, 2), jump);
            moveTo(output, FX::dbToGain(valueOf(n, 3)), jump);
            coefficientsDirty = coefficientsDirty || jump;
        }

    private:
        void prepareModule(double sessionRate, double shapingRate, int) override
        {
            prepareSmoothers(shapingRate, { &drive });
            prepareSmoothers(sessionRate, { &output, &bump });
            tone.reset(sessionRate, kSmoothingSeconds);
        }

        void resetModule() noexcept override
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                bumpFilter[ch].reset();
                toneFilter[ch].reset();
            }
            coefficientsDirty = true;
        }

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = drive.getNextValue();
                const float l = g * left[i];
                const float r = g * right[i];
                left[i]  = l / std::sqrt(1.0f + l * l);
                right[i] = r / std::sqrt(1.0f + r * r);
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            for (int start = 0; start < numSamples; start += kCoefficientStep)
            {
                const int count = juce::jmin(kCoefficientStep, numSamples - start);

                if (coefficientsDirty || bump.isSmoothing() || tone.isSmoothing())
                {
                    const double bumpDb = bump.skip(count);
                    const double toneHz = tone.skip(count);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        bumpFilter[ch].setPeak(kBumpHz, kBumpQ, bumpDb, baseRate);
                        toneFilter[ch].setLowpass(toneHz, juce::MathConstants<double>::sqrt2 / 2.0, baseRate);
                    }
                    coefficientsDirty = false;
                }

                for (int i = start; i < start + count; ++i)
                {
                    const float o = output.getNextValue();
                    left[i]  = toneFilter[0].process(bumpFilter[0].process(left[i]))  * o;
                    right[i] = toneFilter[1].process(bumpFilter[1].process(right[i])) * o;
                }
            }
        }

        Linear drive, bump, output;
        Multiplicative tone;
        FXDSP::Biquad bumpFilter[2], toneFilter[2];
        bool coefficientsDirty = true;
    };

    // ── Drive ────────────────────────────────────────────────────────────────
    // An overdrive that can leave the sub alone: a Linkwitz-Riley crossover splits the signal, only
    // the band above the crossover is driven, and the clean low band is added back. The low band is
    // delayed by exactly the oversampling latency the driven band picks up, so the two recombine
    // phase-aligned. At the lowest crossover (20 Hz) it is a full-range drive in practice.
    class Drive final : public Oversampled
    {
    public:
        explicit Drive(int oversamplingLog2) : Oversampled(FX::ModuleType::drive, oversamplingLog2) {}

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(drive,     FX::dbToGain(valueOf(n, 0)), jump);
            moveTo(crossover, valueOf(n, 1), jump);
            moveTo(tone,      valueOf(n, 2), jump);
            moveTo(output,    FX::dbToGain(valueOf(n, 3)), jump);
        }

    private:
        static constexpr double kButterworthQ = 0.70710678118654752;

        void prepareModule(double sessionRate, double shapingRate, int maxBlockSize) override
        {
            prepareSmoothers(shapingRate, { &drive });
            prepareSmoothers(sessionRate, { &output });
            crossover.reset(sessionRate, kSmoothingSeconds);
            tone.reset(sessionRate, kSmoothingSeconds);

            for (int ch = 0; ch < 2; ++ch)
            {
                lowBand[ch].assign((size_t) maxBlockSize, 0.0f);
                lowDelay[ch].prepare(getLatencySamples());
                lowDelay[ch].setDelay(getLatencySamples());
            }
        }

        void resetModule() noexcept override
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                for (auto& svf : split[ch])
                    svf.reset();
                toneFilter[ch].reset();
                lowDelay[ch].reset();
            }
        }

        void beforeShaping(float* left, float* right, int numSamples) noexcept override
        {
            float* channels[] { left, right };

            for (int i = 0; i < numSamples; ++i)
            {
                if (crossover.isSmoothing() || i == 0)
                {
                    const double hz = crossover.getNextValue();
                    for (auto& channel : split)
                        for (auto& svf : channel)
                            svf.setCoefficients(hz, kButterworthQ, baseRate);
                }

                // LR4: two Butterworth sections in series on each side. Their sum is an all-pass,
                // so the split itself colours nothing.
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float x = channels[ch][i];
                    lowBand[ch][(size_t) i] = split[ch][1].process(split[ch][0].process(x).low).low;
                    channels[ch][i]         = split[ch][3].process(split[ch][2].process(x).high).high;
                }
            }

            lowDelay[0].process(lowBand[0].data(), numSamples);
            lowDelay[1].process(lowBand[1].data(), numSamples);
        }

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = drive.getNextValue();
                left[i]  = std::tanh(g * left[i]);
                right[i] = std::tanh(g * right[i]);
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (tone.isSmoothing() || i == 0)
                {
                    const double hz = tone.getNextValue();
                    toneFilter[0].setCutoff(hz, baseRate);
                    toneFilter[1].setCutoff(hz, baseRate);
                }

                const float o = output.getNextValue();
                left[i]  = (toneFilter[0].process(left[i])  + lowBand[0][(size_t) i]) * o;
                right[i] = (toneFilter[1].process(right[i]) + lowBand[1][(size_t) i]) * o;
            }
        }

        Linear drive, output;
        Multiplicative crossover, tone;
        FXDSP::TptSvf split[2][4];
        FXDSP::OnePoleLowpass toneFilter[2];
        std::vector<float> lowBand[2];
        FXDSP::IntegerDelay lowDelay[2];
    };

    // ── Ring Mod ─────────────────────────────────────────────────────────────
    // Multiplies the signal by a carrier, replacing each input frequency with its sum and
    // difference against the carrier - metallic, inharmonic. Shape morphs the carrier from a sine
    // toward a (softened) square; Spread detunes the right channel's carrier up to 10% above the
    // left's, which is where this module's stereo comes from.
    class RingMod final : public Oversampled
    {
    public:
        explicit RingMod(int oversamplingLog2) : Oversampled(FX::ModuleType::ringMod, oversamplingLog2) {}

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(frequency, valueOf(n, 0), jump);
            moveTo(spread,    valueOf(n, 1) / 100.0f, jump);
            moveTo(shapeMix,  valueOf(n, 2) / 100.0f, jump);
            moveTo(output,    FX::dbToGain(valueOf(n, 3)), jump);
        }

    private:
        static constexpr float kMaxSpread = 0.1f;

        static float carrier(double phase, float squareAmount) noexcept
        {
            const float s = (float) std::sin(phase);
            static const float norm = 1.0f / std::tanh(5.0f);
            return s + squareAmount * (std::tanh(5.0f * s) * norm - s);
        }

        void prepareModule(double sessionRate, double shapingRate, int) override
        {
            rate = shapingRate;
            frequency.reset(shapingRate, kSmoothingSeconds);
            prepareSmoothers(shapingRate, { &spread, &shapeMix });
            prepareSmoothers(sessionRate, { &output });
        }

        void resetModule() noexcept override { phaseL = phaseR = 0.0; }

        void shape(float* left, float* right, int numSamples) noexcept override
        {
            constexpr double twoPi = juce::MathConstants<double>::twoPi;

            for (int i = 0; i < numSamples; ++i)
            {
                const double f = frequency.getNextValue();
                const float m = shapeMix.getNextValue();

                left[i]  *= carrier(phaseL, m);
                right[i] *= carrier(phaseR, m);

                phaseL += twoPi * f / rate;
                phaseR += twoPi * f * (1.0 + kMaxSpread * spread.getNextValue()) / rate;
                if (phaseL >= twoPi) phaseL -= twoPi;
                if (phaseR >= twoPi) phaseR -= twoPi;
            }
        }

        void afterShaping(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float o = output.getNextValue();
                left[i]  *= o;
                right[i] *= o;
            }
        }

        double rate = 44100.0, phaseL = 0.0, phaseR = 0.0;
        Multiplicative frequency;
        Linear spread, shapeMix, output;
    };
}

std::unique_ptr<FXModule> createDistortion(FX::ModuleType type, int oversamplingLog2)
{
    if (type == FX::ModuleType::clip) return std::make_unique<Clip>(oversamplingLog2);
    if (type == FX::ModuleType::tube) return std::make_unique<Tube>(oversamplingLog2);
    if (type == FX::ModuleType::wave) return std::make_unique<Wave>(oversamplingLog2);
    if (type == FX::ModuleType::tape) return std::make_unique<Tape>(oversamplingLog2);
    if (type == FX::ModuleType::drive) return std::make_unique<Drive>(oversamplingLog2);
    if (type == FX::ModuleType::ringMod) return std::make_unique<RingMod>(oversamplingLog2);
    return nullptr;
}
}
