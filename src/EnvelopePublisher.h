#pragma once
#include "EnvelopeModel.h"
#include "EnvelopeSnapshot.h"
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>
#include <vector>

// EnvelopePublisher — bridges a pitch EnvelopeModel and an amp EnvelopeModel
// to the audio thread. Listens for edits on both ValueTrees (message thread
// only); on any edit, rebuilds both lookup tables (EnvelopeEvaluator) and
// atomically publishes a new EnvelopeSnapshot via a single atomic-pointer
// exchange (§2: "publish an immutable snapshot to the audio thread via a
// single atomic-pointer swap").
//
// getSnapshot() is the only audio-thread-safe entry point: a wait-free load
// of the current pointer, no locks, no ValueTree access. Old snapshots are
// not freed inline — they're retired into a message-thread-only list and
// freed on a delayed juce::Timer tick, by which time any audio callback that
// might have been holding the old pointer has long since finished (a
// processBlock call is microseconds to low-milliseconds; the retirement
// delay is a full timer interval away, generously longer).
class EnvelopePublisher : private juce::ValueTree::Listener,
                           private juce::Timer
{
public:
    EnvelopePublisher(EnvelopeModel& pitchModel, EnvelopeModel& ampModel);
    ~EnvelopePublisher() override;

    // Wait-free load of the current snapshot pointer. Safe to call from any
    // thread, including the audio thread. Never null after construction.
    const EnvelopeSnapshot* getSnapshot() const noexcept
    {
        return current.load(std::memory_order_acquire);
    }

private:
    EnvelopeModel& pitchModel;
    EnvelopeModel& ampModel;

    std::atomic<const EnvelopeSnapshot*> current { nullptr };

    // Double-buffered retirement: entries move from retiringNow -> retiringPrev
    // on each timer tick, and retiringPrev is cleared (freeing its snapshots)
    // at the *start* of the following tick. Guarantees at least one full timer
    // interval between a snapshot being swapped out and it being freed.
    std::vector<std::unique_ptr<const EnvelopeSnapshot>> retiringNow;
    std::vector<std::unique_ptr<const EnvelopeSnapshot>> retiringPrev;

    static constexpr int kRetirementTimerMs = 50;

    // Rebuilds both tables from the live models and atomically republishes.
    void rebuildAndPublish();

    // juce::ValueTree::Listener — any change on either tree triggers a rebuild.
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void valueTreeParentChanged(juce::ValueTree&) override {}

    // juce::Timer — drains the retirement list on a fixed interval.
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EnvelopePublisher)
};
