#pragma once
#include "FXModuleInfo.h"
#include <vector>

// The audio thread's view of the four FX macros' assignments (§3.7, §7.1: "4 macro params, each
// with a list of (target param, range, polarity) assignments"). Plain data, built on the message
// thread by MacroAssignments and published immutable through an atomic pointer, exactly like the
// envelope snapshots (§2) - the audio thread never reads the ValueTree the GUI edits.
namespace FX
{
    // One macro driving one slot control. Range and polarity are one signed depth: the target sits
    // at its own value with the macro at zero and moves by depth x macro, clamped to its range.
    struct MacroAssignment
    {
        int   macro  = 0;    // 0 to kNumMacros - 1
        int   slot   = 0;    // 0 to kNumSlots - 1
        int   target = 0;    // 0 to 5 = p1-p6, kMixTarget = the slot's mix
        float depth  = 0.0f; // -1 to +1, as a fraction of the control's full travel
    };

    struct MacroSnapshot
    {
        std::vector<MacroAssignment> assignments;
    };

    // Most assignments one macro can hold. Generous for four knobs over eighteen slots, and a
    // bound means a hostile preset can never make the per-block modulation pass unbounded.
    constexpr int kMaxAssignmentsPerMacro = 32;
}
