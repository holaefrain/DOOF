#pragma once
#include "FXModule.h"
#include "FXDSP.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// FXModuleBase — shared plumbing for the module implementations. Internal to the FX*.cpp files:
// everything outside the FX code reaches a module through FXModule and FX::createModule.
namespace FXModules
{
    using Linear         = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;
    using Multiplicative = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative>;

    // How long a control takes to reach a new value. Long enough that a block-rate step from the
    // host - or a macro swept by hand - never zippers; short enough to feel immediate.
    constexpr double kSmoothingSeconds = 0.02;

    // Coefficient recalculation interval for the biquad-based modules while a control is gliding.
    constexpr int kCoefficientStep = 16;

    // The common base: tracks whether the next setParameters() should jump rather than glide.
    class ModuleBase : public FXModule
    {
    public:
        using FXModule::FXModule;

    protected:
        // True once after every reset(): straight after a module is built, re-enabled from bypass
        // or cleared, its controls should be where they are set, not gliding there from defaults.
        bool consumeJump() noexcept
        {
            const bool jump = jumpNext;
            jumpNext = false;
            return jump;
        }

        void markJump() noexcept { jumpNext = true; }

        template <typename Smoothed>
        static void moveTo(Smoothed& smoothed, float value, bool jump) noexcept
        {
            if (jump)
                smoothed.setCurrentAndTargetValue(value);
            else
                smoothed.setTargetValue(value);
        }

    private:
        bool jumpNext = true;
    };

    // A nonlinear module. Its shaping runs at the global oversampling rate between JUCE's
    // linear-phase half-band FIR filters, with integer latency so that the engine can compensate
    // every other route exactly. Factor 1 ("Off") skips the filters entirely and adds no latency.
    //
    // A subclass splits its work three ways: anything linear before the nonlinearity (at the
    // session rate), the nonlinearity itself (oversampled), and anything linear after it.
    class Oversampled : public ModuleBase
    {
    public:
        Oversampled(FX::ModuleType type, int oversamplingLog2)
            : ModuleBase(type), factorLog2(juce::jlimit(0, FX::kMaxOversamplingLog2, oversamplingLog2))
        {
        }

        void prepare(double sampleRate, int maxBlockSize) override
        {
            baseRate = sampleRate;

            if (factorLog2 > 0)
            {
                oversampler = std::make_unique<juce::dsp::Oversampling<float>>(
                    2, (size_t) factorLog2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                    true, true);
                oversampler->initProcessing((size_t) maxBlockSize);
            }

            prepareModule(sampleRate, sampleRate * (double) (1 << factorLog2), maxBlockSize);
            reset();
        }

        void reset() noexcept override
        {
            if (oversampler != nullptr)
                oversampler->reset();

            resetModule();
            markJump();
        }

        int getLatencySamples() const noexcept override
        {
            return oversampler != nullptr ? (int) std::lround(oversampler->getLatencyInSamples()) : 0;
        }

        void process(float* left, float* right, int numSamples) noexcept override
        {
            beforeShaping(left, right, numSamples);

            if (oversampler != nullptr)
            {
                float* channels[] { left, right };
                juce::dsp::AudioBlock<float> block(channels, 2, (size_t) numSamples);
                auto up = oversampler->processSamplesUp(block);
                shape(up.getChannelPointer(0), up.getChannelPointer(1), (int) up.getNumSamples());
                oversampler->processSamplesDown(block);
            }
            else
            {
                shape(left, right, numSamples);
            }

            afterShaping(left, right, numSamples);
        }

    protected:
        virtual void prepareModule(double sessionRate, double shapingRate, int maxBlockSize) = 0;
        virtual void resetModule() noexcept = 0;
        virtual void beforeShaping(float*, float*, int) noexcept {}
        virtual void shape(float* left, float* right, int numSamples) noexcept = 0; // oversampled
        virtual void afterShaping(float*, float*, int) noexcept {}

        double baseRate = 44100.0;

    private:
        const int factorLog2;
        std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    };

    // One factory per family, dispatched by FX::createModule. Each returns nullptr for a type that
    // is not its own.
    std::unique_ptr<FXModule> createDistortion(FX::ModuleType type, int oversamplingLog2);
    std::unique_ptr<FXModule> createFilterFamily(FX::ModuleType type);
    std::unique_ptr<FXModule> createDynamics(FX::ModuleType type);
    std::unique_ptr<FXModule> createTimeBased(FX::ModuleType type);
}
