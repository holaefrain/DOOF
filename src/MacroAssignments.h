#pragma once
#include "FXMacros.h"
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <atomic>
#include <memory>
#include <vector>

// MacroAssignments — which slot controls the four FX macros drive, and how far (§3.7, §7.1).
//
// Structured state, not parameters: an assignment list has no fixed size, and hosts expect a fixed
// parameter list (§2). It lives in a ValueTree saved with the preset (state version 5), and every
// edit publishes an immutable FX::MacroSnapshot to the audio thread through one atomic-pointer swap,
// the same pattern as the envelope snapshots - with the same delayed, double-buffered freeing of
// the outgoing snapshot, so an audio callback still holding it can never see it freed.
//
// Message thread only, except getSnapshot().
class MacroAssignments : private juce::Timer
{
public:
    MacroAssignments();
    ~MacroAssignments() override;

    // Adds an assignment, or updates its depth if the macro already drives that control. Depth is
    // clamped to -1..+1. Ignored once the macro has FX::kMaxAssignmentsPerMacro assignments.
    void assign(int macro, int slot, int target, float depth);
    void remove(int macro, int slot, int target);

    bool  isAssigned(int macro, int slot, int target) const;
    float getDepth(int macro, int slot, int target) const; // 0 when not assigned

    // One macro's assignments, in the order they were made - what its list in the GUI shows.
    std::vector<FX::MacroAssignment> getAssignments(int macro) const;

    // Moves every assignment on slot a to slot b and vice versa: a drag-swap's assignments travel
    // with their modules (user decision, 2026-11-17).
    void swapSlots(int a, int b);

    // Drops every assignment on a slot - used when its module is replaced, since the old module's
    // controls are not the new one's.
    void clearSlot(int slot);
    void clear();

    // For the preset. setState validates every entry: out-of-range indices are dropped, depths
    // clamped, duplicates merged, and each macro capped, so a damaged or hostile file cannot put
    // anything unbounded in front of the audio thread.
    juce::ValueTree getState() const;
    void setState(const juce::ValueTree& state);

    // Audio thread: wait-free. Never null.
    const FX::MacroSnapshot* getSnapshot() const noexcept { return current.load(std::memory_order_acquire); }

    static const juce::Identifier stateType;

private:
    juce::ValueTree findAssignment(int macro, int slot, int target) const;
    int countFor(int macro) const;
    void publish();
    void timerCallback() override;

    juce::ValueTree tree;

    std::atomic<const FX::MacroSnapshot*> current { nullptr };
    std::vector<std::unique_ptr<const FX::MacroSnapshot>> retiringNow, retiringPrev;
    static constexpr int kRetirementTimerMs = 50;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MacroAssignments)
};
