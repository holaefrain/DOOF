#include "FXModuleBase.h"
#include <cmath>

// Compressor and Dual Comp (§3.7's "dual Compressors" - both forms, user decision 2026-11-17).
// Neither oversamples: gain riding at audio rate generates far less aliasing than a waveshaper,
// and the detector already runs per sample.
namespace FXModules
{
namespace
{
    // A feed-forward, stereo-linked compressor stage, after Giannoulis, Massberg & Reiss, "Digital
    // Dynamic Range Compressor Design - A Tutorial and Analysis" (JAES 2012): a soft-knee gain
    // computer in the log domain, then a branching peak detector on the gain reduction itself.
    // Stereo-linked so the image never shifts: both channels take the larger side's reduction.
    struct CompressorStage
    {
        // The static curve's gain reduction in dB (zero or negative) for a level in dBFS.
        static float gainReductionDb(float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept
        {
            const float over = levelDb - thresholdDb;
            float outDb = levelDb;

            if (2.0f * over < -kneeDb)
                outDb = levelDb;
            else if (2.0f * std::abs(over) <= kneeDb && kneeDb > 0.0f)
            {
                const float inKnee = over + kneeDb * 0.5f;
                outDb = levelDb + (1.0f / ratio - 1.0f) * inKnee * inKnee / (2.0f * kneeDb);
            }
            else
                outDb = thresholdDb + over / ratio;

            return outDb - levelDb;
        }

        static float levelDb(float left, float right) noexcept
        {
            const float peak = juce::jmax(std::abs(left), std::abs(right));
            return peak > 1.0e-6f ? 20.0f * std::log10(peak) : -120.0f;
        }

        // Attack while the reduction is deepening, release while it recovers.
        float process(float targetDb) noexcept
        {
            const float coefficient = targetDb < stateDb ? attack : release;
            stateDb = coefficient * stateDb + (1.0f - coefficient) * targetDb;
            return stateDb;
        }

        void setTimes(double attackSeconds, double releaseSeconds, double sampleRate) noexcept
        {
            attack  = (float) std::exp(-1.0 / (attackSeconds  * sampleRate));
            release = (float) std::exp(-1.0 / (releaseSeconds * sampleRate));
        }

        void reset() noexcept { stateDb = 0.0f; }

        float stateDb = 0.0f, attack = 0.0f, release = 0.0f;
    };

    // ── Compressor ───────────────────────────────────────────────────────────
    class Compressor final : public ModuleBase
    {
    public:
        Compressor() : ModuleBase(FX::ModuleType::compressor) {}

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            for (auto* smoothed : { &threshold, &ratio, &knee, &makeup })
                smoothed->reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            stage.reset();
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(threshold, valueOf(n, 0), jump);
            moveTo(ratio,     valueOf(n, 1), jump);
            moveTo(knee,      valueOf(n, 4), jump);
            moveTo(makeup,    valueOf(n, 5), jump);
            stage.setTimes(valueOf(n, 2) / 1000.0, valueOf(n, 3) / 1000.0, rate);
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float target = CompressorStage::gainReductionDb(CompressorStage::levelDb(left[i], right[i]),
                                                                      threshold.getNextValue(), ratio.getNextValue(),
                                                                      knee.getNextValue());
                const float gain = FX::dbToGain(stage.process(target) + makeup.getNextValue());
                left[i]  *= gain;
                right[i] *= gain;
            }
        }

    private:
        double rate = 44100.0;
        Linear threshold, ratio, knee, makeup;
        CompressorStage stage;
    };

    // ── Dual Comp ────────────────────────────────────────────────────────────
    // Two compressors in series in one slot: a fast Peak stage that catches the transient, into a
    // slow Glue stage that rides the body. Each stage has its own threshold and ratio; Speed sets
    // both stages' timing together (fast to slow), a fixed 6 dB knee keeps both smooth, and one
    // Makeup follows them. Six controls, which is what a slot has.
    class DualComp final : public ModuleBase
    {
    public:
        DualComp() : ModuleBase(FX::ModuleType::dualComp) {}

        static constexpr float kKneeDb = 6.0f;

        // Each stage's attack and release at Speed 0% and 100%, in seconds. Speed sweeps between
        // them on a log scale, the way time constants are heard.
        struct Timing { double attackFast, attackSlow, releaseFast, releaseSlow; };
        static constexpr Timing kPeak { 0.0001, 0.002, 0.020, 0.200 };
        static constexpr Timing kGlue { 0.005,  0.050, 0.100, 1.000 };

        static double sweep(double fast, double slow, double speed) { return fast * std::pow(slow / fast, speed); }

        void prepare(double sampleRate, int) override
        {
            rate = sampleRate;
            for (auto* smoothed : { &peakThreshold, &peakRatio, &glueThreshold, &glueRatio, &makeup })
                smoothed->reset(sampleRate, kSmoothingSeconds);
            reset();
        }

        void reset() noexcept override
        {
            peak.reset();
            glue.reset();
            markJump();
        }

        void setParameters(const FX::SlotValues& n, const FX::BlockContext&) noexcept override
        {
            const bool jump = consumeJump();
            moveTo(peakThreshold, valueOf(n, 0), jump);
            moveTo(peakRatio,     valueOf(n, 1), jump);
            moveTo(glueThreshold, valueOf(n, 2), jump);
            moveTo(glueRatio,     valueOf(n, 3), jump);
            moveTo(makeup,        valueOf(n, 5), jump);

            const double speed = valueOf(n, 4) / 100.0;
            peak.setTimes(sweep(kPeak.attackFast, kPeak.attackSlow, speed), sweep(kPeak.releaseFast, kPeak.releaseSlow, speed), rate);
            glue.setTimes(sweep(kGlue.attackFast, kGlue.attackSlow, speed), sweep(kGlue.releaseFast, kGlue.releaseSlow, speed), rate);
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float peakDb = peak.process(CompressorStage::gainReductionDb(
                    CompressorStage::levelDb(left[i], right[i]), peakThreshold.getNextValue(),
                    peakRatio.getNextValue(), kKneeDb));
                const float afterPeak = FX::dbToGain(peakDb);

                // The glue stage hears the peak stage's output, as two units in series would.
                const float glueDb = glue.process(CompressorStage::gainReductionDb(
                    CompressorStage::levelDb(left[i] * afterPeak, right[i] * afterPeak),
                    glueThreshold.getNextValue(), glueRatio.getNextValue(), kKneeDb));

                const float gain = FX::dbToGain(peakDb + glueDb + makeup.getNextValue());
                left[i]  *= gain;
                right[i] *= gain;
            }
        }

    private:
        double rate = 44100.0;
        Linear peakThreshold, peakRatio, glueThreshold, glueRatio, makeup;
        CompressorStage peak, glue;
    };
}

std::unique_ptr<FXModule> createDynamics(FX::ModuleType type)
{
    if (type == FX::ModuleType::compressor) return std::make_unique<Compressor>();
    if (type == FX::ModuleType::dualComp) return std::make_unique<DualComp>();
    return nullptr;
}
}
