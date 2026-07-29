#pragma once
#include <juce_data_structures/juce_data_structures.h>

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

// EnvelopeModel — owns one node-based envelope as a ValueTree (schema above)
// plus the UndoManager that makes every edit undoable.
//
// Pure data model: knows how to add/move/delete nodes and keep them ordered
// by time. Knows nothing about audio or Bezier math — the evaluator (Step 2)
// reads this model to build a lookup table, and the atomic-pointer publisher
// (Step 3) listens on getValueTree() to know when to rebuild and republish.
// Message-thread only; never touched from processBlock.
class EnvelopeModel
{
public:
    EnvelopeModel() = default;

    // Plain-value snapshot of one node, returned by value so callers (tests,
    // the evaluator) can inspect a node without touching the underlying tree.
    struct Node
    {
        double time = 0.0, value = 0.0;
        double cpOutTime = 0.0, cpOutValue = 0.0; // outgoing control point (toward next node)
        double cpInTime  = 0.0, cpInValue  = 0.0; // incoming control point (from previous node)
    };

    // Inserts a new node at (time, value), keeping nodes ordered by time.
    // Control points default to coincident with the node itself. When both a
    // segment's start-node cpOut and end-node cpIn are coincident like this,
    // time and value are both the same (nonlinear) function of the Bezier
    // parameter, and that function cancels out when eliminated between them —
    // so the segment is exactly linear in time until reshaped via
    // setControlPoints(). One undo step. Returns the node's index after insertion.
    int addNode(double time, double value);

    // Moves the node at index to (newTime, newValue). Its control points
    // shift by the same delta so the curve shape travels with the node
    // (reshape independently via setControlPoints). Re-sorts if the move
    // crosses a neighbouring node's time. One undo step. Returns the node's
    // index after the move (may differ from `index` if it was re-sorted).
    int moveNode(int index, double newTime, double newValue);

    // Removes the node at index. One undo step.
    void deleteNode(int index);

    // Repositions just the control points of the node at index, independent
    // of the node's own time/value. One undo step.
    void setControlPoints(int index, double cpOutTime, double cpOutValue,
                                       double cpInTime,  double cpInValue);

    int getNumNodes() const { return tree.getNumChildren(); }
    Node getNode(int index) const;

    void undo() { undoManager.undo(); }
    void redo() { undoManager.redo(); }

    // Exposes the underlying tree for serialisation and for the Step 3
    // publisher to attach a ValueTree::Listener.
    juce::ValueTree& getValueTree() { return tree; }
    const juce::ValueTree& getValueTree() const { return tree; }

    // Replaces the entire model state (e.g. on preset load). Clears undo
    // history since a loaded state isn't a user edit to undo back from.
    void setState(const juce::ValueTree& newState);

private:
    juce::UndoManager undoManager;
    juce::ValueTree tree { EnvelopeIDs::envelopeType };

    // Index at which a node with the given time should be inserted to keep
    // tree children ordered ascending by time.
    int findInsertIndex(double time) const;
};
