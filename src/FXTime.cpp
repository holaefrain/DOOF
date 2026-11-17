#include "FXModuleBase.h"
#include <cmath>

// Delay and Reverb. Their output is the wet signal only; the slot's Mix blends it with the dry
// kick, and a Mix of 100% makes the slot a pure return - what an insert fed by the "Both" route
// wants for a parallel reverb.
namespace FXModules
{
namespace
{
    // A circular buffer read at fractional positions behind the newest sample, with linear
    // interpolation - exact at whole-sample delays, which is what lets the delay's echo land on a
    // known sample.
    struct DelayLine
    {
        void prepare(int maxDelaySamples)
        {
            buffer.assign((size_t) juce::jmax(4, maxDelaySamples + 2), 0.0f);
            write = 0;
        }

        void reset() noexcept { std::fill(buffer.begin(), buffer.end(), 0.0f); }

        void push(float x) noexcept
        {
            buffer[(size_t) write] = x;
            if (++write == (int) buffer.size())
                write = 0;
        }

        // `delay` samples behind the newest pushed sample; 0 reads that sample itself.
        float read(double delay) const noexcept
        {
            const int size = (int) buffer.size();
            double position = (double) (write - 1) - juce::jlimit(0.0, (double) (size - 2), delay);
            if (position < 0.0)
                position += size;

            const int older = (int) position;
            const int newer = older + 1 == size ? 0 : older + 1;
            const float fraction = (float) (position - older);
            return buffer[(size_t) older] + fraction * (buffer[(size_t) newer] - buffer[(size_t) older]);
        }

        std::vector<float> buffer;
        int write = 0;
    };

    // ── Delay ────────────────────────────────────────────────────────────────
    // A stereo delay with filtered feedback. Ping-Pong crosses the feedback between channels and
    // feeds the input into the left side only, so at 100% the repeats alternate left, right, left;
    // in between it blends toward two independent delays. Sync replaces Time with a note division
    // of the host tempo. Changing the time glides the read head (a tape-style pitch bend) rather
    // than jumping it, which would click.
    class Delay final : public ModuleBase
    {
    public:
        Delay() : ModuleBase(FX::ModuleType::delay) {}

        static constexpr double kMaxSeconds  = 4.0; // a synced 1/1 at 60 BPM
        static constexpr double kGlideSeconds = 0.1;

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            for (auto& line : lines)
                line.prepare((int) std::ceil(kMaxSeconds * sampleRate) + 2);
            delaySamples.reset(sampleRate, kGlideSeconds);
            feedback.reset(sampleRate, kSmoothingSeconds);
            pingPong.reset(sampleRate, kSmoothingSeconds);
            tone.reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                lines[ch].reset();
                toneFilter[ch].reset();
            }
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext& context) noexcept override
        {
            const bool jump = consumeJump();

            const bool synced = juce::roundToInt(valueOf(n, 4)) == 1;
            const double bpm = context.bpm > 0.0 ? context.bpm : 120.0;
            const double seconds = synced ? FX::divisionBeats(juce::roundToInt(valueOf(n, 5))) * 60.0 / bpm
                                          : valueOf(n, 0) / 1000.0;

            // A whole number of samples once settled: a fractional read is a linear interpolation,
            // which is a gentle low-pass - harmless once, but compounded on every repeat.
            moveTo(delaySamples, (float) std::round(juce::jlimit(0.001, kMaxSeconds, seconds) * rate), jump);
            moveTo(feedback,     valueOf(n, 1) / 100.0f, jump);
            moveTo(tone,         valueOf(n, 2), jump);
            moveTo(pingPong,     valueOf(n, 3) / 100.0f, jump);
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                if (tone.isSmoothing() || i == 0)
                {
                    const double hz = tone.getNextValue();
                    toneFilter[0].setCutoff(hz, rate);
                    toneFilter[1].setCutoff(hz, rate);
                }

                // One sample less than the delay, read before this sample is pushed: the echo of
                // an input lands exactly delaySamples later.
                const double d = delaySamples.getNextValue() - 1.0;
                const float heardL = lines[0].read(d);
                const float heardR = lines[1].read(d);

                const float p  = pingPong.getNextValue();
                const float fb = feedback.getNextValue();

                const float backL = toneFilter[0].process((1.0f - p) * heardL + p * heardR);
                const float backR = toneFilter[1].process((1.0f - p) * heardR + p * heardL);

                const float inL = left[i], inR = right[i];
                lines[0].push((1.0f - p) * inL + p * 0.5f * (inL + inR) + fb * backL);
                lines[1].push((1.0f - p) * inR + fb * backR);

                left[i]  = heardL;
                right[i] = heardR;
            }
        }

    private:
        double rate = 44100.0;
        DelayLine lines[2];
        Linear delaySamples, feedback, pingPong;
        Multiplicative tone;
        FXDSP::OnePoleLowpass toneFilter[2];
    };

    // ── Reverb ───────────────────────────────────────────────────────────────
    // An eight-line feedback delay network (Jot's FDN) with a Householder feedback matrix, which
    // is orthogonal - lossless on its own - so every bit of decay comes from the per-line gains.
    // Each line's gain is set from its own length so that every path loses 60 dB in exactly the
    // Decay time: that is what makes the measured RT60 match the control.
    //
    // Signal path: mono sum -> Low Cut (keeps the reverb off the sub) -> Pre-Delay -> four
    // series all-pass diffusers (so the onset is dense rather than a flutter) -> the network.
    // Damping is a one-pole low-pass inside each line, so highs die faster than lows, as in a
    // real room; at its top setting it is out of the loop entirely. Size scales every line's
    // length together; Width narrows the two decorrelated output taps toward mono.
    class Reverb final : public ModuleBase
    {
    public:
        Reverb() : ModuleBase(FX::ModuleType::reverb) {}

        static constexpr int kLines = 8;

        // Line lengths at full Size, ms. Spread out and mutually incommensurate, so the modes of
        // the network interleave instead of piling up on a few frequencies (metallic ringing).
        static constexpr double kLineMs[kLines] { 31.7, 37.9, 41.3, 47.1, 53.3, 59.9, 67.3, 73.7 };
        static constexpr double kDiffuserMs[4]  { 4.77, 3.59, 12.73, 9.31 };
        static constexpr float  kDiffuserGain   = 0.6f;
        static constexpr double kMaxPreDelayMs  = 200.0;

        static double sizeScale(double size) { return 0.35 + 0.65 * size; }

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;

            preDelayLine.prepare((int) std::ceil(kMaxPreDelayMs / 1000.0 * sampleRate) + 2);
            for (int i = 0; i < kLines; ++i)
                lines[i].prepare((int) std::ceil(kLineMs[i] / 1000.0 * sampleRate) + 2);

            for (int i = 0; i < 4; ++i)
            {
                diffusers[i].buffer.assign((size_t) juce::jmax(1, (int) std::lround(kDiffuserMs[i] / 1000.0 * sampleRate)), 0.0f);
                diffusers[i].index = 0;
            }

            preDelay.reset(sampleRate, Delay::kGlideSeconds);
            size.reset(sampleRate, 0.2);
            width.reset(sampleRate, kSmoothingSeconds);
            lowCut.reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            preDelayLine.reset();
            for (auto& line : lines)
                line.reset();
            for (auto& filter : damping)
                filter.reset();
            for (auto& diffuser : diffusers)
                std::fill(diffuser.buffer.begin(), diffuser.buffer.end(), 0.0f);
            lowCutFilter.reset();
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            decaySeconds = valueOf(n, 0);
            moveTo(preDelay, (float) std::round(valueOf(n, 1) / 1000.0 * rate), jump);
            dampingHz = valueOf(n, 2);
            moveTo(size,   valueOf(n, 3) / 100.0f, jump);
            moveTo(width,  valueOf(n, 4) / 100.0f, jump);
            moveTo(lowCut, valueOf(n, 5), jump);
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            // Loop gains and damping are set once per block, from the size at its start: RT60 is a
            // property of the whole decay, and a block-rate step in a gain this close to 1 is
            // inaudible.
            const double blockScale = sizeScale(size.getCurrentValue());
            float gains[kLines];
            for (int l = 0; l < kLines; ++l)
            {
                const double lengthSamples = std::round(kLineMs[l] / 1000.0 * blockScale * rate);
                gains[l] = (float) std::pow(10.0, -3.0 * lengthSamples / (decaySeconds * rate));
            }

            const bool damped = dampingHz < 0.45 * rate;
            for (auto& filter : damping)
                filter.setCutoff(dampingHz, rate);

            // The input pattern and the two output taps are mutually orthogonal (three rows of an
            // 8x8 Hadamard matrix). Orthogonal input and output means the first pass through the
            // lines arrives as eight separate reflections rather than summing into one coherent
            // burst, and orthogonal taps mean left and right are decorrelated from the start.
            constexpr float householder = 2.0f / (float) kLines;
            static constexpr float inputSign[kLines] { 1, 1, 1, 1, -1, -1, -1, -1 };
            static constexpr float tapLeft[kLines]   { 1, -1, 1, -1, 1, -1, 1, -1 };
            static constexpr float tapRight[kLines]  { 1, 1, -1, -1, 1, 1, -1, -1 };
            const float injection = 1.0f / std::sqrt((float) kLines);

            for (int i = 0; i < numSamples; ++i)
            {
                if (lowCut.isSmoothing() || i == 0)
                    lowCutFilter.setCoefficients(lowCut.getNextValue(), 0.70710678, rate);

                float x = lowCutFilter.process(0.5f * (left[i] + right[i])).high;

                preDelayLine.push(x);
                x = preDelayLine.read(preDelay.getNextValue());

                for (auto& diffuser : diffusers)
                    x = diffuser.process(x, kDiffuserGain);

                const double scale = sizeScale(size.getNextValue());

                // Whole-sample line lengths. A fractional read inside the loop would be a linear
                // interpolation - a low-pass - applied again on every trip round it, taking about
                // 3 dB of broadband energy per pass at a half-sample fraction and cutting the decay
                // short of what Decay asks for. A Size change therefore steps the lengths by whole
                // samples, which the diffuse field hides.
                float heard[kLines], back[kLines];
                float sum = 0.0f;
                for (int l = 0; l < kLines; ++l)
                {
                    heard[l] = lines[l].read(std::round(kLineMs[l] / 1000.0 * scale * rate) - 1.0);
                    back[l] = (damped ? damping[l].process(heard[l]) : heard[l]) * gains[l];
                    sum += back[l];
                }

                float outL = 0.0f, outR = 0.0f;
                for (int l = 0; l < kLines; ++l)
                {
                    lines[l].push(back[l] - householder * sum + inputSign[l] * injection * x);
                    outL += tapLeft[l] * heard[l];
                    outR += tapRight[l] * heard[l];
                }

                outL *= injection;
                outR *= injection;

                const float mid  = 0.5f * (outL + outR);
                const float side = 0.5f * (outL - outR) * width.getNextValue();
                left[i]  = mid + side;
                right[i] = mid - side;
            }
        }

    private:
        // Schroeder all-pass: flat magnitude, smeared phase - density without colour.
        struct Allpass
        {
            float process(float x, float g) noexcept
            {
                const float delayed = buffer[(size_t) index];
                const float w = x + g * delayed;
                buffer[(size_t) index] = w;
                if (++index == (int) buffer.size())
                    index = 0;
                return delayed - g * w;
            }

            std::vector<float> buffer;
            int index = 0;
        };

        double rate = 44100.0, decaySeconds = 1.2, dampingHz = 6000.0;
        DelayLine preDelayLine, lines[kLines];
        FXDSP::OnePoleLowpass damping[kLines];
        Allpass diffusers[4];
        FXDSP::TptSvf lowCutFilter;
        Linear preDelay, size, width;
        Multiplicative lowCut;
    };
}

std::unique_ptr<FXModule> createTimeBased(FX::ModuleType type)
{
    if (type == FX::ModuleType::delay) return std::make_unique<Delay>();
    if (type == FX::ModuleType::reverb) return std::make_unique<Reverb>();
    return nullptr;
}
}
