#include "FXEngine.h"
#include <cmath>

// ── Tail ─────────────────────────────────────────────────────────────────────

double FX::tailSeconds(ModuleType type, const SlotValues& values, double bpm)
{
    const auto& info = getInfo(type);
    auto value = [&](int index) { return (double) toValue(info.params[(size_t) index], values[(size_t) index]); };

    if (type == ModuleType::reverb)
        return value(0) + value(1) / 1000.0;

    if (type == ModuleType::delay)
    {
        const bool synced = juce::roundToInt(value(4)) == 1;
        const double seconds = synced ? divisionBeats(juce::roundToInt(value(5))) * 60.0 / juce::jmax(1.0, bpm)
                                      : value(0) / 1000.0;
        const double feedback = value(1) / 100.0;

        // Repeats until they fall 60 dB, capped where a "tail" stops being one.
        const double repeats = feedback > 0.001 ? std::log(0.001) / std::log(feedback) : 0.0;
        return juce::jmin(30.0, seconds * (1.0 + repeats));
    }

    return 0.0;
}

// ── Retired modules ──────────────────────────────────────────────────────────

void FXRetiredModules::push(FXModule* module) noexcept
{
    if (module == nullptr)
        return;

    int start1, size1, start2, size2;
    fifo.prepareToWrite(1, start1, size1, start2, size2);

    if (size1 > 0)
    {
        items[(size_t) start1] = module;
        fifo.finishedWrite(1);
        return;
    }

    jassertfalse; // the message thread has not drained in a very long time; leaked, not freed here
}

void FXRetiredModules::deleteAll()
{
    while (fifo.getNumReady() > 0)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead(1, start1, size1, start2, size2);
        delete items[(size_t) start1];
        items[(size_t) start1] = nullptr;
        fifo.finishedRead(1);
    }
}

// ── Slot ─────────────────────────────────────────────────────────────────────

FXSlot::~FXSlot()
{
    deleteEverything();
}

void FXSlot::deleteEverything()
{
    delete incoming.exchange(nullptr);
    delete active;
    delete next;
    active = next = nullptr;
}

void FXSlot::prepare(double sampleRate, int maxBlockSize, int maxLatency)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        dry[ch].assign((size_t) maxBlockSize, 0.0f);
        dryDelay[ch].prepare(maxLatency);
    }

    onAmount.reset(sampleRate, 0.01);
    mixAmount.reset(sampleRate, 0.02);
}

void FXSlot::installNow(std::unique_ptr<FXModule> module)
{
    deleteEverything();
    active = module.release();

    for (auto& delay : dryDelay)
    {
        delay.reset();
        delay.setDelay(getLatencySamples());
    }

    moduleNeedsReset = false;
    jumpControls = true;
}

std::unique_ptr<FXModule> FXSlot::offer(std::unique_ptr<FXModule> module)
{
    return std::unique_ptr<FXModule>(incoming.exchange(module.release(), std::memory_order_acq_rel));
}

int FXSlot::getLatencySamples() const noexcept
{
    return isAudible(active) ? active->getLatencySamples() : 0;
}

FX::ModuleType FXSlot::getActiveType() const noexcept
{
    return active != nullptr ? active->getType() : FX::ModuleType::none;
}

void FXSlot::process(float* left, float* right, int numSamples, const FX::SlotParams& params,
                     const FX::BlockContext& context, FXRetiredModules& retired) noexcept
{
    // A module offered since the last block. If an earlier one was still waiting, it never played:
    // it goes straight back to be deleted.
    if (auto* offered = incoming.exchange(nullptr, std::memory_order_acq_rel))
    {
        retired.push(next);
        next = offered;
    }

    if (next != nullptr)
    {
        if (! isAudible(active) || (onAmount.getCurrentValue() <= 0.0f && ! onAmount.isSmoothing()))
        {
            // Nothing (left) to fade out: swap now, and fade the newcomer up from the dry signal.
            retired.push(active);
            active = next;
            next = nullptr;

            for (auto& delay : dryDelay)
            {
                delay.reset();
                delay.setDelay(getLatencySamples());
            }

            onAmount.setCurrentAndTargetValue(0.0f);
            mixAmount.setCurrentAndTargetValue(params.mix);
            moduleNeedsReset = false; // freshly prepared, so already clean
            jumpControls = false;
        }
        else
        {
            onAmount.setTargetValue(0.0f);
        }
    }

    if (! isAudible(active))
        return; // empty: an exact passthrough

    if (jumpControls)
    {
        onAmount.setCurrentAndTargetValue(params.on ? 1.0f : 0.0f);
        mixAmount.setCurrentAndTargetValue(params.mix);
        jumpControls = false;
    }
    else
    {
        if (next == nullptr)
            onAmount.setTargetValue(params.on ? 1.0f : 0.0f);
        mixAmount.setTargetValue(params.mix);
    }

    // The dry signal, delayed to line up with the module's output. Fed every block, so that
    // re-enabling the module or turning the mix down mid-note blends against the right audio.
    float* channels[] { left, right };
    for (int ch = 0; ch < 2; ++ch)
    {
        std::copy(channels[ch], channels[ch] + numSamples, dry[ch].data());
        dryDelay[ch].process(dry[ch].data(), numSamples);
    }

    if (! onAmount.isSmoothing() && onAmount.getCurrentValue() <= 0.0f)
    {
        // Bypassed: the delayed input, exactly, and the module not run at all.
        for (int ch = 0; ch < 2; ++ch)
            std::copy(dry[ch].data(), dry[ch].data() + numSamples, channels[ch]);

        moduleNeedsReset = true;
        return;
    }

    if (moduleNeedsReset)
    {
        active->reset();
        moduleNeedsReset = false;
    }

    active->setParameters(params.values, context);
    active->process(left, right, numSamples);

    // Fully wet: the module's output untouched.
    if (! onAmount.isSmoothing() && ! mixAmount.isSmoothing()
        && onAmount.getCurrentValue() >= 1.0f && mixAmount.getCurrentValue() >= 1.0f)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        const float wet = onAmount.getNextValue() * mixAmount.getNextValue();
        left[i]  = dry[0][(size_t) i] * (1.0f - wet) + left[i]  * wet;
        right[i] = dry[1][(size_t) i] * (1.0f - wet) + right[i] * wet;
    }
}

// ── Engine ───────────────────────────────────────────────────────────────────

FXEngine::FXEngine()
{
    for (auto& value : effectiveForDisplay)
        value.store(0.5f, std::memory_order_relaxed);
}

void FXEngine::prepare(double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;
    maxBlock = juce::jmax(1, maxBlockSize);

    for (int log2 = 0; log2 <= FX::kMaxOversamplingLog2; ++log2)
        moduleLatency[(size_t) log2] = FX::oversamplingLatency(log2, maxBlock);

    // No module can add more than the steepest oversampling does, and no route has more than one
    // channel's worth of slots, so these bound every delay the engine will ever be asked for.
    const int maxModuleLatency = moduleLatency[(size_t) FX::kMaxOversamplingLog2];
    const int maxRouteLatency  = maxModuleLatency * FX::kSlotsPerChannel;

    for (auto& bus : buses)
        bus.assign((size_t) maxBlock, 0.0f);

    for (int c = 0; c < 2; ++c)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            insert[c][ch].assign((size_t) maxBlock, 0.0f);
            insertAlign[c][ch].prepare(maxRouteLatency);
        }
        insertLevel[c].reset(sampleRate, 0.02);
    }
    jumpLevels = true;
    dryAlign.prepare(maxRouteLatency);

    for (auto& slot : slots)
        slot.prepare(sampleRate, maxBlock, maxModuleLatency);

    // Every module is rebuilt at the new rate, directly - the audio is stopped.
    for (size_t s = 0; s < slots.size(); ++s)
    {
        if (requested[s].type < 0)
            continue;

        auto module = FX::createModule((FX::ModuleType) requested[s].type, requested[s].oversampling);
        module->prepare(sampleRate, maxBlock);
        slots[s].installNow(std::move(module));
    }

    retired.deleteAll();
    latency.store(juce::jmax(chainLatency(0), chainLatency(1)) + chainLatency(2), std::memory_order_relaxed);
}

void FXEngine::setModuleTypes(const std::array<int, FX::kNumSlots>& types, int oversamplingLog2, bool audioStopped)
{
    retired.deleteAll();
    requestedOversampling = juce::jlimit(0, FX::kMaxOversamplingLog2, oversamplingLog2);

    for (size_t s = 0; s < slots.size(); ++s)
    {
        const auto type = (FX::ModuleType) juce::jlimit(0, FX::kNumModuleTypes - 1, types[s]);
        const Request wanted { (int) type, effectiveOversampling(type, requestedOversampling) };

        if (wanted.type == requested[s].type && wanted.oversampling == requested[s].oversampling)
            continue;

        requested[s] = wanted;

        if (sampleRate <= 0.0)
            continue; // built by prepare()

        auto module = FX::createModule(type, wanted.oversampling);
        module->prepare(sampleRate, maxBlock);

        if (audioStopped)
            slots[s].installNow(std::move(module));
        else
            slots[s].offer(std::move(module)); // a superseded, never-played offer is deleted here
    }

    if (audioStopped)
        latency.store(juce::jmax(chainLatency(0), chainLatency(1)) + chainLatency(2), std::memory_order_relaxed);
}

void FXEngine::clearBuses(int numSamples) noexcept
{
    for (auto& bus : buses)
        std::fill(bus.begin(), bus.begin() + numSamples, 0.0f);
}

int FXEngine::chainLatency(int channel) const noexcept
{
    int total = 0;
    for (int s = 0; s < FX::kSlotsPerChannel; ++s)
        total += slots[(size_t) FX::slotIndex((FX::Channel) channel, s)].getLatencySamples();
    return total;
}

int FXEngine::getModuleLatency(int oversamplingLog2) const noexcept
{
    return moduleLatency[(size_t) juce::jlimit(0, FX::kMaxOversamplingLog2, oversamplingLog2)];
}

float FXEngine::getEffectiveValue(int slot, int target) const noexcept
{
    jassert(juce::isPositiveAndBelow(slot, FX::kNumSlots) && juce::isPositiveAndBelow(target, FX::kNumTargets));
    return effectiveForDisplay[(size_t) (slot * FX::kNumTargets + target)].load(std::memory_order_relaxed);
}

void FXEngine::processChain(int channel, float* left, float* right, int numSamples,
                            const FX::BlockContext& context) noexcept
{
    for (int s = 0; s < FX::kSlotsPerChannel; ++s)
    {
        const auto index = (size_t) FX::slotIndex((FX::Channel) channel, s);
        slots[index].process(left, right, numSamples, effective[index], context, retired);
    }
}

void FXEngine::process(float* left, float* right, int numSamples, const FX::EngineParams& params) noexcept
{
    jassert(numSamples <= maxBlock);

    // 1. Macros (§7.1): each assignment moves its target by depth x macro, from where the control
    // itself is set. Applied to a copy, so the host parameters are never written from here.
    effective = params.slots;

    if (params.macroSnapshot != nullptr)
    {
        for (const auto& a : params.macroSnapshot->assignments)
        {
            if (! juce::isPositiveAndBelow(a.slot, FX::kNumSlots) || ! juce::isPositiveAndBelow(a.macro, FX::kNumMacros))
                continue;

            auto& slot = effective[(size_t) a.slot];
            const float offset = a.depth * params.macros[(size_t) a.macro];

            if (a.target == FX::kMixTarget)
                slot.mix += offset;
            else if (juce::isPositiveAndBelow(a.target, FX::kNumSlotParams))
                slot.values[(size_t) a.target] += offset;
        }

        // Clamped after every assignment is summed, so two macros pushing a control past its end
        // and one pulling it back meet where the arithmetic says, not where the first clamp left it.
        for (auto& slot : effective)
        {
            slot.mix = juce::jlimit(0.0f, 1.0f, slot.mix);
            for (auto& value : slot.values)
                value = juce::jlimit(0.0f, 1.0f, value);
        }
    }

    for (size_t s = 0; s < effective.size(); ++s)
    {
        for (int t = 0; t < FX::kNumSlotParams; ++t)
            effectiveForDisplay[s * FX::kNumTargets + (size_t) t].store(effective[s].values[(size_t) t], std::memory_order_relaxed);
        effectiveForDisplay[s * FX::kNumTargets + FX::kMixTarget].store(effective[s].mix, std::memory_order_relaxed);
    }

    // 2. The inserts: each mono bus becomes stereo here (§7.1), runs its chain, then its level.
    for (int c = 0; c < 2; ++c)
    {
        float* l = insert[c][0].data();
        float* r = insert[c][1].data();
        const float* bus = buses[(size_t) (c == 0 ? FX::Bus::insertA : FX::Bus::insertB)].data();

        std::copy(bus, bus + numSamples, l);
        std::copy(bus, bus + numSamples, r);

        processChain(c, l, r, numSamples, params.context);

        if (jumpLevels)
            insertLevel[c].setCurrentAndTargetValue(params.insertLevel[(size_t) c]);
        else
            insertLevel[c].setTargetValue(params.insertLevel[(size_t) c]);
        if (insertLevel[c].isSmoothing() || insertLevel[c].getCurrentValue() < 1.0f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float g = insertLevel[c].getNextValue();
                l[i] *= g;
                r[i] *= g;
            }
        }
    }

    jumpLevels = false;

    // 3. Alignment: the dry bus and the quicker insert wait for the slower one.
    const int latencyA = chainLatency(0);
    const int latencyB = chainLatency(1);
    const int routeLatency = juce::jmax(latencyA, latencyB);

    float* dry = buses[(size_t) FX::Bus::dry].data();
    dryAlign.setDelay(routeLatency);
    dryAlign.process(dry, numSamples);

    const int lag[] { routeLatency - latencyA, routeLatency - latencyB };
    for (int c = 0; c < 2; ++c)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            insertAlign[c][ch].setDelay(lag[c]);
            insertAlign[c][ch].process(insert[c][ch].data(), numSamples);
        }
    }

    // 4. Master: the three routes summed, then its chain.
    for (int i = 0; i < numSamples; ++i)
    {
        left[i]  = dry[i] + insert[0][0][(size_t) i] + insert[1][0][(size_t) i];
        right[i] = dry[i] + insert[0][1][(size_t) i] + insert[1][1][(size_t) i];
    }

    processChain(2, left, right, numSamples, params.context);

    latency.store(routeLatency + chainLatency(2), std::memory_order_relaxed);
}
