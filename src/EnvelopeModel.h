#pragma once
#include <juce_core/juce_core.h>

// EnvelopeIDs — ValueTree schema for a node-based envelope (§2, §3.1 of project-architecture.md).
//
// Shape:
//   ENVELOPE
//     NODE { time, value, cpOutTime, cpOutValue, cpInTime, cpInValue }
//     NODE { ... }
//     ...
//
// Nodes are ordered by `time` (ascending) within the parent ENVELOPE tree.
// Each node carries a full cubic-Bezier control pair: `cpOut` shapes the curve
// leaving this node toward the next one; `cpIn` shapes the curve arriving at
// this node from the previous one. The first node's cpIn and the last node's
// cpOut are unused (no segment on that side) but stored for a uniform schema.
//
// These IDs are the stable contract between EnvelopeModel (edits), the Bezier
// evaluator (reads), and the Phase 3 canvas (edits via the same model). Once
// any session/preset has been saved against this schema, property names are
// never renamed — same rule as APVTS parameter IDs (§2).
namespace EnvelopeIDs
{
    static const juce::Identifier envelopeType { "ENVELOPE" };
    static const juce::Identifier nodeType     { "NODE" };

    static const juce::Identifier time        { "time" };        // node's own time (seconds)
    static const juce::Identifier value       { "value" };       // node's own value (envelope units)
    static const juce::Identifier cpOutTime   { "cpOutTime" };    // outgoing control point, time
    static const juce::Identifier cpOutValue  { "cpOutValue" };   // outgoing control point, value
    static const juce::Identifier cpInTime    { "cpInTime" };     // incoming control point, time
    static const juce::Identifier cpInValue   { "cpInValue" };    // incoming control point, value
}
