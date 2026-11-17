#include "FXModuleBase.h"
#include <cmath>

// Filter, EQ and Bit Crusher. None of them oversample: the filters are linear, and the bit
// crusher's aliasing is the effect itself.
namespace FXModules
{
namespace
{
    // ── Filter ───────────────────────────────────────────────────────────────
    // Low pass, high pass, band pass or notch at 12 or 24 dB per octave, from TPT state-variable
    // filters. At zero resonance both slopes are exact Butterworth responses - one section at
    // Q 0.7071 for 12 dB, two at the fourth-order pair 0.5412 / 1.3066 for 24 dB - so the low and
    // high pass are exactly -3 dB at the cutoff, which §6's verify measures. Resonance raises the
    // (second) section's Q exponentially toward 12. Band pass is normalised to unity at its peak.
    class Filter final : public ModuleBase
    {
    public:
        Filter() : ModuleBase(FX::ModuleType::filter) {}

        enum Mode { lowPass = 0, highPass = 1, bandPass = 2, notch = 3 };

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            cutoff.reset(sampleRate, kSmoothingSeconds);
            resonance.reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            for (auto& channel : sections)
                for (auto& svf : channel)
                    svf.reset();
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            mode     = juce::roundToInt(valueOf(n, 0));
            steep    = juce::roundToInt(valueOf(n, 3)) == 1;
            moveTo(cutoff,    valueOf(n, 1), jump);
            moveTo(resonance, valueOf(n, 2) / 100.0f, jump);
            coefficientsDirty = true;
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            float* channels[] { left, right };

            for (int i = 0; i < numSamples; ++i)
            {
                if (coefficientsDirty || cutoff.isSmoothing() || resonance.isSmoothing())
                {
                    updateCoefficients(cutoff.getNextValue(), resonance.getNextValue());
                    coefficientsDirty = false;
                }

                for (int ch = 0; ch < 2; ++ch)
                {
                    float x = channels[ch][i];
                    x = section(sections[ch][0], x);
                    if (steep)
                        x = section(sections[ch][1], x);
                    channels[ch][i] = x;
                }
            }
        }

    private:
        static constexpr double kMaxQ = 12.0;

        // One section's output for the current mode.
        float section(FXDSP::TptSvf& svf, float x) const noexcept
        {
            const auto out = svf.process(x);
            switch (mode)
            {
                case highPass: return out.high;
                case bandPass: return svf.k * out.band;
                case notch:    return out.low + out.high;
                default:       return out.low;
            }
        }

        void updateCoefficients(double hz, double r) noexcept
        {
            // The Q of the section that carries the resonance rises from its Butterworth value.
            auto resonant = [r](double butterworthQ) { return butterworthQ * std::pow(kMaxQ / butterworthQ, r); };

            const double q1 = steep ? 0.54119610014619698 : resonant(0.70710678118654752);
            const double q2 = resonant(1.3065629648763766);

            for (auto& channel : sections)
            {
                channel[0].setCoefficients(hz, q1, rate);
                channel[1].setCoefficients(hz, q2, rate);
            }
        }

        double rate = 44100.0;
        Multiplicative cutoff;
        Linear resonance;
        int mode = lowPass;
        bool steep = false, coefficientsDirty = true;
        FXDSP::TptSvf sections[2][2];
    };

    // ── EQ ───────────────────────────────────────────────────────────────────
    // Three bands - a low shelf, a bell, a high shelf - which is exactly the slot's six controls.
    // RBJ cookbook biquads; the shelves' corners are where they reach half their gain, the bell
    // (Q 1) reaches its full gain at its frequency.
    class EQ final : public ModuleBase
    {
    public:
        EQ() : ModuleBase(FX::ModuleType::eq) {}

        static constexpr double kBellQ = 1.0;

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            for (auto* gain : { &lowGain, &midGain, &highGain })
                gain->reset(sampleRate, kSmoothingSeconds);
            for (auto* freq : { &lowFreq, &midFreq, &highFreq })
                freq->reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            for (auto& channel : bands)
                for (auto& band : channel)
                    band.reset();
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(lowGain,  valueOf(n, 0), jump);
            moveTo(lowFreq,  valueOf(n, 1), jump);
            moveTo(midGain,  valueOf(n, 2), jump);
            moveTo(midFreq,  valueOf(n, 3), jump);
            moveTo(highGain, valueOf(n, 4), jump);
            moveTo(highFreq, valueOf(n, 5), jump);
            coefficientsDirty = coefficientsDirty || jump;
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            for (int start = 0; start < numSamples; start += kCoefficientStep)
            {
                const int count = juce::jmin(kCoefficientStep, numSamples - start);

                if (coefficientsDirty || isGliding())
                {
                    const double lg = lowGain.skip(count),  lf = lowFreq.skip(count);
                    const double mg = midGain.skip(count),  mf = midFreq.skip(count);
                    const double hg = highGain.skip(count), hf = highFreq.skip(count);

                    for (auto& channel : bands)
                    {
                        channel[0].setLowShelf(lf, lg, rate);
                        channel[1].setPeak(mf, kBellQ, mg, rate);
                        channel[2].setHighShelf(hf, hg, rate);
                    }
                    coefficientsDirty = false;
                }

                for (int i = start; i < start + count; ++i)
                {
                    left[i]  = bands[0][2].process(bands[0][1].process(bands[0][0].process(left[i])));
                    right[i] = bands[1][2].process(bands[1][1].process(bands[1][0].process(right[i])));
                }
            }
        }

    private:
        bool isGliding() const noexcept
        {
            return lowGain.isSmoothing() || midGain.isSmoothing() || highGain.isSmoothing()
                || lowFreq.isSmoothing() || midFreq.isSmoothing() || highFreq.isSmoothing();
        }

        double rate = 44100.0;
        Linear lowGain, midGain, highGain;
        Multiplicative lowFreq, midFreq, highFreq;
        FXDSP::Biquad bands[2][3];
        bool coefficientsDirty = true;
    };

    // ── Bit Crusher ──────────────────────────────────────────────────────────
    // Sample-rate reduction (hold each input sample for 1/Rate seconds) into a mid-tread quantiser:
    // with B bits every output sample is a whole multiple of 2^-(B-1), clamped to [-1, 1]. That
    // grid is exact, which is what lets §6's "quantization steps visible in the waveform" be
    // checked sample by sample.
    class BitCrusher final : public ModuleBase
    {
    public:
        BitCrusher() : ModuleBase(FX::ModuleType::bitCrusher) {}

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            holdRate.reset(sampleRate, kSmoothingSeconds);
            output.reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            phase = 1.0; // takes the very first sample
            held[0] = held[1] = 0.0f;
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            const int bits = juce::jlimit(1, 16, juce::roundToInt(valueOf(n, 0)));
            levels = (float) (1 << (bits - 1));
            moveTo(holdRate, valueOf(n, 1), jump);
            moveTo(output,   FX::dbToGain(valueOf(n, 2)), jump);
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                phase += holdRate.getNextValue() / rate;
                if (phase >= 1.0)
                {
                    phase -= std::floor(phase);
                    held[0] = left[i];
                    held[1] = right[i];
                }

                const float o = output.getNextValue();
                left[i]  = quantise(held[0]) * o;
                right[i] = quantise(held[1]) * o;
            }
        }

    private:
        float quantise(float x) const noexcept
        {
            return juce::jlimit(-1.0f, 1.0f, std::round(x * levels) / levels);
        }

        double rate = 44100.0, phase = 1.0;
        float levels = 128.0f;
        float held[2] {};
        Multiplicative holdRate;
        Linear output;
    };
}

std::unique_ptr<FXModule> createFilterFamily(FX::ModuleType type)
{
    if (type == FX::ModuleType::filter) return std::make_unique<Filter>();
    if (type == FX::ModuleType::eq) return std::make_unique<EQ>();
    if (type == FX::ModuleType::bitCrusher) return std::make_unique<BitCrusher>();
    return nullptr;
}
}
