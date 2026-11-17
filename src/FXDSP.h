#pragma once
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

// FXDSP — the small building blocks the FX modules share: an integer delay, one-pole and
// state-variable filters, and RBJ biquads. Plain structs with no allocation outside prepare(),
// so every process call is real-time safe (§2's cardinal rule).
namespace FXDSP
{
    constexpr double kPi = juce::MathConstants<double>::pi;

    // A whole-sample delay, for latency compensation. Exact: a delay of zero is a no-op, and any
    // other delay outputs the input `delay` samples late, bit for bit - which is what lets a
    // bypassed oversampled module, or a route aligned against another, null exactly.
    class IntegerDelay
    {
    public:
        void prepare(int maxDelaySamples)
        {
            maxDelay = juce::jmax(0, maxDelaySamples);
            buffer.assign((size_t) maxDelay + 1, 0.0f);
            writeIndex = 0;
            delay = 0;
        }

        void reset() noexcept { std::fill(buffer.begin(), buffer.end(), 0.0f); }

        void setDelay(int samples) noexcept { delay = juce::jlimit(0, maxDelay, samples); }
        int  getDelay() const noexcept      { return delay; }

        // Always writes, even at zero delay, so that lengthening the delay later reads real history
        // rather than whatever was left from before.
        void process(float* data, int numSamples) noexcept
        {
            if (buffer.empty())
                return;

            const int size = (int) buffer.size();
            for (int i = 0; i < numSamples; ++i)
            {
                buffer[(size_t) writeIndex] = data[i];
                int readIndex = writeIndex - delay;
                if (readIndex < 0)
                    readIndex += size;
                data[i] = buffer[(size_t) readIndex];
                if (++writeIndex == size)
                    writeIndex = 0;
            }
        }

    private:
        std::vector<float> buffer;
        int maxDelay = 0, writeIndex = 0, delay = 0;
    };

    // One-pole low-pass, unity gain at DC: y += a (x - y).
    struct OnePoleLowpass
    {
        void setCutoff(double hz, double sampleRate) noexcept
        {
            a = (float) (1.0 - std::exp(-2.0 * kPi * juce::jmin(hz, 0.49 * sampleRate) / sampleRate));
        }
        void reset() noexcept { y = 0.0f; }
        float process(float x) noexcept { y += a * (x - y); return y; }

        float a = 1.0f, y = 0.0f;
    };

    // Zavalishin's topology-preserving-transform state-variable filter (the form Andrew Simper
    // derives as the "trapezoidal SVF"). Stays stable and well-behaved under per-sample cutoff
    // modulation, which is what lets the Filter module and the Drive crossover move smoothly
    // instead of zippering. The cutoff is pre-warped exactly, so a Butterworth Q puts the -3 dB
    // point precisely on the set frequency.
    struct TptSvf
    {
        struct Outputs { float low, band, high; };

        void setCoefficients(double cutoffHz, double q, double sampleRate) noexcept
        {
            const double fc = juce::jlimit(1.0, 0.49 * sampleRate, cutoffHz);
            const double g  = std::tan(kPi * fc / sampleRate);
            k  = (float) (1.0 / q);
            a1 = (float) (1.0 / (1.0 + g * (g + k)));
            a2 = (float) (g * a1);
            a3 = (float) (g * a2);
        }

        void reset() noexcept { ic1 = ic2 = 0.0f; }

        Outputs process(float x) noexcept
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            return { v2, v1, x - k * v1 - v2 };
        }

        float k = 1.4142135f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        float ic1 = 0.0f, ic2 = 0.0f;
    };

    // Transposed direct form II biquad, coefficients from Robert Bristow-Johnson's Audio EQ
    // Cookbook. Used where the shape is a shelf or a bell, which the SVF does not give directly.
    struct Biquad
    {
        void reset() noexcept { s1 = s2 = 0.0f; }

        float process(float x) noexcept
        {
            const float y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }

        void setPeak(double hz, double q, double gainDb, double sampleRate) noexcept
        {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w = 2.0 * kPi * clampFrequency(hz, sampleRate) / sampleRate;
            const double alpha = std::sin(w) / (2.0 * q);
            const double c = std::cos(w);
            set(1.0 + alpha * A, -2.0 * c, 1.0 - alpha * A,
                1.0 + alpha / A, -2.0 * c, 1.0 - alpha / A);
        }

        // Shelf slope S = 1, the steepest without overshoot. At hz the gain is exactly half the
        // shelf's, in dB - the cookbook's definition of a shelf's corner.
        void setLowShelf(double hz, double gainDb, double sampleRate) noexcept  { setShelf(hz, gainDb, sampleRate, false); }
        void setHighShelf(double hz, double gainDb, double sampleRate) noexcept { setShelf(hz, gainDb, sampleRate, true); }

        void setLowpass(double hz, double q, double sampleRate) noexcept
        {
            const double w = 2.0 * kPi * clampFrequency(hz, sampleRate) / sampleRate;
            const double alpha = std::sin(w) / (2.0 * q);
            const double c = std::cos(w);
            set((1.0 - c) / 2.0, 1.0 - c, (1.0 - c) / 2.0,
                1.0 + alpha, -2.0 * c, 1.0 - alpha);
        }

        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float s1 = 0.0f, s2 = 0.0f;

    private:
        static double clampFrequency(double hz, double sampleRate) { return juce::jlimit(1.0, 0.49 * sampleRate, hz); }

        void setShelf(double hz, double gainDb, double sampleRate, bool high) noexcept
        {
            const double A = std::pow(10.0, gainDb / 40.0);
            const double w = 2.0 * kPi * clampFrequency(hz, sampleRate) / sampleRate;
            const double c = std::cos(w);
            const double alpha = std::sin(w) / 2.0 * std::sqrt(2.0); // S = 1
            const double rootA = std::sqrt(A);
            const double sign = high ? -1.0 : 1.0;

            set(A * ((A + 1.0) - sign * (A - 1.0) * c + 2.0 * rootA * alpha),
                sign * 2.0 * A * ((A - 1.0) - sign * (A + 1.0) * c),
                A * ((A + 1.0) - sign * (A - 1.0) * c - 2.0 * rootA * alpha),
                (A + 1.0) + sign * (A - 1.0) * c + 2.0 * rootA * alpha,
                -sign * 2.0 * ((A - 1.0) + sign * (A + 1.0) * c),
                (A + 1.0) + sign * (A - 1.0) * c - 2.0 * rootA * alpha);
        }

        void set(double nb0, double nb1, double nb2, double na0, double na1, double na2) noexcept
        {
            b0 = (float) (nb0 / na0);
            b1 = (float) (nb1 / na0);
            b2 = (float) (nb2 / na0);
            a1 = (float) (na1 / na0);
            a2 = (float) (na2 / na0);
        }
    };
}
