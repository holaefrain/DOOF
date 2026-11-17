#include "MacroAssignments.h"

// Never rename these once a preset has shipped - same rule as parameter IDs (§2).
const juce::Identifier MacroAssignments::stateType { "MACROS" };

namespace
{
    const juce::Identifier assignmentType { "ASSIGN" };
    const juce::Identifier macroProp  { "macro" };
    const juce::Identifier slotProp   { "slot" };
    const juce::Identifier targetProp { "target" };
    const juce::Identifier depthProp  { "depth" };

    bool isValid(int macro, int slot, int target)
    {
        return juce::isPositiveAndBelow(macro, FX::kNumMacros)
            && juce::isPositiveAndBelow(slot, FX::kNumSlots)
            && juce::isPositiveAndBelow(target, FX::kNumTargets);
    }
}

MacroAssignments::MacroAssignments()
    : tree(stateType)
{
    publish();
    startTimer(kRetirementTimerMs);
}

MacroAssignments::~MacroAssignments()
{
    stopTimer();

    // The owner (the processor) is being torn down, so the host has stopped calling processBlock.
    delete current.exchange(nullptr);
}

juce::ValueTree MacroAssignments::findAssignment(int macro, int slot, int target) const
{
    for (const auto child : tree)
        if ((int) child[macroProp] == macro && (int) child[slotProp] == slot && (int) child[targetProp] == target)
            return child;

    return {};
}

int MacroAssignments::countFor(int macro) const
{
    int count = 0;
    for (const auto child : tree)
        if ((int) child[macroProp] == macro)
            ++count;
    return count;
}

void MacroAssignments::assign(int macro, int slot, int target, float depth)
{
    if (! isValid(macro, slot, target))
        return;

    const float clamped = juce::jlimit(-1.0f, 1.0f, depth);

    if (auto existing = findAssignment(macro, slot, target); existing.isValid())
    {
        existing.setProperty(depthProp, clamped, nullptr);
    }
    else
    {
        if (countFor(macro) >= FX::kMaxAssignmentsPerMacro)
            return;

        juce::ValueTree child { assignmentType };
        child.setProperty(macroProp, macro, nullptr);
        child.setProperty(slotProp, slot, nullptr);
        child.setProperty(targetProp, target, nullptr);
        child.setProperty(depthProp, clamped, nullptr);
        tree.appendChild(child, nullptr);
    }

    publish();
}

void MacroAssignments::remove(int macro, int slot, int target)
{
    if (auto existing = findAssignment(macro, slot, target); existing.isValid())
    {
        tree.removeChild(existing, nullptr);
        publish();
    }
}

bool MacroAssignments::isAssigned(int macro, int slot, int target) const
{
    return findAssignment(macro, slot, target).isValid();
}

float MacroAssignments::getDepth(int macro, int slot, int target) const
{
    const auto existing = findAssignment(macro, slot, target);
    return existing.isValid() ? (float) existing[depthProp] : 0.0f;
}

std::vector<FX::MacroAssignment> MacroAssignments::getAssignments(int macro) const
{
    std::vector<FX::MacroAssignment> result;
    for (const auto child : tree)
        if ((int) child[macroProp] == macro)
            result.push_back({ macro, (int) child[slotProp], (int) child[targetProp], (float) child[depthProp] });
    return result;
}

void MacroAssignments::swapSlots(int a, int b)
{
    if (a == b || ! juce::isPositiveAndBelow(a, FX::kNumSlots) || ! juce::isPositiveAndBelow(b, FX::kNumSlots))
        return;

    for (auto child : tree)
    {
        const int slot = child[slotProp];
        if (slot == a)
            child.setProperty(slotProp, b, nullptr);
        else if (slot == b)
            child.setProperty(slotProp, a, nullptr);
    }

    publish();
}

void MacroAssignments::clearSlot(int slot)
{
    bool removed = false;
    for (int i = tree.getNumChildren(); --i >= 0;)
    {
        if ((int) tree.getChild(i)[slotProp] == slot)
        {
            tree.removeChild(i, nullptr);
            removed = true;
        }
    }

    if (removed)
        publish();
}

void MacroAssignments::clear()
{
    tree.removeAllChildren(nullptr);
    publish();
}

juce::ValueTree MacroAssignments::getState() const
{
    return tree.createCopy();
}

void MacroAssignments::setState(const juce::ValueTree& state)
{
    tree.removeAllChildren(nullptr);

    if (state.hasType(stateType))
    {
        for (const auto child : state)
        {
            if (! child.hasType(assignmentType))
                continue;

            const int macro = child.getProperty(macroProp, -1);
            const int slot = child.getProperty(slotProp, -1);
            const int target = child.getProperty(targetProp, -1);
            const float depth = child.getProperty(depthProp, 0.0f);

            if (! isValid(macro, slot, target) || ! std::isfinite(depth))
                continue;

            // Through assign() so a file can never do what the GUI could not: duplicates merge,
            // depths clamp, and each macro stops at its cap.
            if (auto existing = findAssignment(macro, slot, target); existing.isValid())
                existing.setProperty(depthProp, juce::jlimit(-1.0f, 1.0f, depth), nullptr);
            else if (countFor(macro) < FX::kMaxAssignmentsPerMacro)
            {
                juce::ValueTree copy { assignmentType };
                copy.setProperty(macroProp, macro, nullptr);
                copy.setProperty(slotProp, slot, nullptr);
                copy.setProperty(targetProp, target, nullptr);
                copy.setProperty(depthProp, juce::jlimit(-1.0f, 1.0f, depth), nullptr);
                tree.appendChild(copy, nullptr);
            }
        }
    }

    publish();
}

void MacroAssignments::publish()
{
    auto snapshot = std::make_unique<FX::MacroSnapshot>();
    snapshot->assignments.reserve((size_t) tree.getNumChildren());

    for (const auto child : tree)
        snapshot->assignments.push_back({ (int) child[macroProp], (int) child[slotProp],
                                          (int) child[targetProp], (float) child[depthProp] });

    const auto* old = current.exchange(snapshot.release(), std::memory_order_acq_rel);
    if (old != nullptr)
        retiringNow.emplace_back(old);
}

// Frees what was retired at least one full interval ago, then rotates this tick's retirements in.
void MacroAssignments::timerCallback()
{
    retiringPrev.clear();
    retiringPrev = std::move(retiringNow);
    retiringNow.clear();
}
