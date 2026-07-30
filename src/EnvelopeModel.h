#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <memory>

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

    static const juce::Identifier length      { "length" };       // work-area length, seconds (Phase 3 §3.4)
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
    // externalUndoManager, if given, is used instead of an owned UndoManager
    // so several EnvelopeModels can share one undo history (e.g. pitch and
    // amp on the same Sub layer, so a single "undo" reverts the most recent
    // edit regardless of which envelope it touched). Must outlive this model.
    // Defaults to null: owns its own UndoManager, exactly Phase 2's behaviour.
    explicit EnvelopeModel(juce::UndoManager* externalUndoManager = nullptr);

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

    // Work-area length in seconds (§3.4's "Length control"): the editable
    // range's own duration, distinct from the fixed 4 s table domain (nodes
    // may still exist beyond it — Length is a work-area boundary, not a
    // hard data limit). Defaults to 0.5 s if never set. One undo step.
    static constexpr double kDefaultLength = 0.5;
    double getLength() const { return tree.getProperty(EnvelopeIDs::length, kDefaultLength); }
    void setLength(double newLength);

    void undo() { undoManager.undo(); }
    void redo() { undoManager.redo(); }

    // Begins a gesture: starts one undo transaction that every mutating call
    // (addNode/moveNode/deleteNode/setControlPoints) made before the matching
    // endGesture() coalesces into, instead of each getting its own step. Used
    // to wrap a whole continuous edit — e.g. a mouse-down-to-mouse-up node
    // drag — into a single undo step. Gestures do not nest.
    void beginGesture()
    {
        jassert(!inGesture);
        undoManager.beginNewTransaction();
        inGesture = true;
    }

    // Ends the current gesture. Mutating calls after this each resume
    // starting their own transaction, as normal.
    void endGesture()
    {
        jassert(inGesture);
        inGesture = false;
    }

    // Exposes the underlying tree for serialisation and for the Step 3
    // publisher to attach a ValueTree::Listener.
    juce::ValueTree& getValueTree() { return tree; }
    const juce::ValueTree& getValueTree() const { return tree; }

    // Replaces the entire model state (e.g. on preset load). Clears undo
    // history since a loaded state isn't a user edit to undo back from.
    void setState(const juce::ValueTree& newState);

private:
    // Declaration order matters: ownedUndoManager must be constructed before
    // undoManager, since undoManager's initializer reads it.
    std::unique_ptr<juce::UndoManager> ownedUndoManager; // null when using an external UndoManager
    juce::UndoManager& undoManager;                       // always valid: *ownedUndoManager, or the external one
    juce::ValueTree tree { EnvelopeIDs::envelopeType };

    // True between beginGesture() and endGesture(); mutating methods skip
    // their usual beginNewTransaction() call while this is set.
    bool inGesture = false;

    // Index at which a node with the given time should be inserted to keep
    // tree children ordered ascending by time.
    int findInsertIndex(double time) const;
};
