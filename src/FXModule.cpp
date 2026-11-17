#include "FXModuleBase.h"

namespace
{
    // What an emptied slot is handed: passes audio through untouched and costs nothing. A real
    // object rather than a null pointer so that "switch this slot to nothing" travels through the
    // same lock-free handoff as every other module change.
    class PassThrough final : public FXModule
    {
    public:
        PassThrough() : FXModule(FX::ModuleType::none) {}
        void prepare(double, int) override {}
        void reset() noexcept override {}
        void setParameters(const FX::SlotValues&, const FX::BlockContext&) noexcept override {}
        void process(float*, float*, int) noexcept override {}
    };
}

namespace FX
{
std::unique_ptr<FXModule> createModule(ModuleType type, int oversamplingLog2)
{
    if (type == ModuleType::none)
        return std::make_unique<PassThrough>();

    if (auto module = FXModules::createDistortion(type, oversamplingLog2))
        return module;
    if (auto module = FXModules::createFilterFamily(type))
        return module;
    if (auto module = FXModules::createDynamics(type))
        return module;
    if (auto module = FXModules::createTimeBased(type))
        return module;

    jassertfalse; // a ModuleType with no implementation
    return std::make_unique<PassThrough>();
}

int oversamplingLatency(int oversamplingLog2, int maxBlockSize)
{
    if (oversamplingLog2 <= 0)
        return 0;

    juce::dsp::Oversampling<float> probe(2, (size_t) juce::jmin(oversamplingLog2, kMaxOversamplingLog2),
                                         juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true);
    probe.initProcessing((size_t) juce::jmax(1, maxBlockSize));
    return (int) std::lround(probe.getLatencyInSamples());
}
}
