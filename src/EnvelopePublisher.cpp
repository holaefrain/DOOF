#include "EnvelopePublisher.h"
#include "EnvelopeEvaluator.h"

EnvelopePublisher::EnvelopePublisher(EnvelopeModel& pitch, EnvelopeModel& amp)
    : pitchModel(pitch), ampModel(amp)
{
    pitchModel.getValueTree().addListener(this);
    ampModel.getValueTree().addListener(this);

    rebuildAndPublish(); // seed an initial snapshot so getSnapshot() is never null

    startTimer(kRetirementTimerMs);
}

EnvelopePublisher::~EnvelopePublisher()
{
    stopTimer();
    pitchModel.getValueTree().removeListener(this);
    ampModel.getValueTree().removeListener(this);

    // Safe to free immediately here: the publisher is only ever destroyed
    // once its owner (the processor) is being torn down, by which point the
    // host has already stopped calling processBlock.
    delete current.exchange(nullptr);
    retiringNow.clear();
    retiringPrev.clear();
}

// Rebuilds both tables from the live models and atomically swaps in a new
// snapshot. Runs on whichever thread triggered it — always the message
// thread, since EnvelopeModel edits (and therefore these ValueTree callbacks)
// are message-thread only.
void EnvelopePublisher::rebuildAndPublish()
{
    auto pitchTable = EnvelopeEvaluator::buildTable(pitchModel);
    auto ampTable   = EnvelopeEvaluator::buildTable(ampModel);

    auto* newSnapshot = new EnvelopeSnapshot(std::move(pitchTable), std::move(ampTable));
    const auto* oldSnapshot = current.exchange(newSnapshot, std::memory_order_acq_rel);

    if (oldSnapshot != nullptr)
        retiringNow.emplace_back(oldSnapshot);
}

void EnvelopePublisher::valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) { rebuildAndPublish(); }
void EnvelopePublisher::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&)              { rebuildAndPublish(); }
void EnvelopePublisher::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int)        { rebuildAndPublish(); }
void EnvelopePublisher::valueTreeChildOrderChanged(juce::ValueTree&, int, int)                { rebuildAndPublish(); }

// Frees whatever was retired at least one full timer interval ago, then
// rotates this tick's retirements into place for next time.
void EnvelopePublisher::timerCallback()
{
    retiringPrev.clear();
    retiringPrev = std::move(retiringNow);
    retiringNow.clear();
}
