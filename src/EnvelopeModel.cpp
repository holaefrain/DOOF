#include "EnvelopeModel.h"

EnvelopeModel::EnvelopeModel(juce::UndoManager* externalUndoManager)
    : ownedUndoManager(externalUndoManager == nullptr ? std::make_unique<juce::UndoManager>() : nullptr),
      undoManager(externalUndoManager != nullptr ? *externalUndoManager : *ownedUndoManager)
{
}

// Index at which a node with the given time should be inserted to keep
// tree children ordered ascending by time (first index whose time exceeds it).
int EnvelopeModel::findInsertIndex(double time) const
{
    int i = 0;
    for (; i < tree.getNumChildren(); ++i)
        if ((double) tree.getChild(i)[EnvelopeIDs::time] > time)
            break;
    return i;
}

// Builds a new NODE tree with control points coincident with the node itself,
// inserts it in time order, and wraps the whole thing in one undo transaction.
int EnvelopeModel::addNode(double time, double value)
{
    undoManager.beginNewTransaction();

    juce::ValueTree node { EnvelopeIDs::nodeType };
    node.setProperty(EnvelopeIDs::time,       time,  nullptr);
    node.setProperty(EnvelopeIDs::value,      value, nullptr);
    node.setProperty(EnvelopeIDs::cpOutTime,  time,  nullptr);
    node.setProperty(EnvelopeIDs::cpOutValue, value, nullptr);
    node.setProperty(EnvelopeIDs::cpInTime,   time,  nullptr);
    node.setProperty(EnvelopeIDs::cpInValue,  value, nullptr);

    const int index = findInsertIndex(time);
    tree.addChild(node, index, &undoManager);
    return index;
}

// Shifts the node's own position and both control points by the same delta,
// then re-sorts (remove + re-insert) in case the move crossed a neighbour.
int EnvelopeModel::moveNode(int index, double newTime, double newValue)
{
    jassert(juce::isPositiveAndBelow(index, tree.getNumChildren()));
    undoManager.beginNewTransaction();

    auto node = tree.getChild(index);
    const double dTime  = newTime  - (double) node[EnvelopeIDs::time];
    const double dValue = newValue - (double) node[EnvelopeIDs::value];

    node.setProperty(EnvelopeIDs::time,  newTime,  &undoManager);
    node.setProperty(EnvelopeIDs::value, newValue, &undoManager);
    node.setProperty(EnvelopeIDs::cpOutTime,  (double) node[EnvelopeIDs::cpOutTime]  + dTime,  &undoManager);
    node.setProperty(EnvelopeIDs::cpOutValue, (double) node[EnvelopeIDs::cpOutValue] + dValue, &undoManager);
    node.setProperty(EnvelopeIDs::cpInTime,   (double) node[EnvelopeIDs::cpInTime]   + dTime,  &undoManager);
    node.setProperty(EnvelopeIDs::cpInValue,  (double) node[EnvelopeIDs::cpInValue]  + dValue, &undoManager);

    tree.removeChild(node, &undoManager);
    const int newIndex = findInsertIndex(newTime);
    tree.addChild(node, newIndex, &undoManager);
    return newIndex;
}

void EnvelopeModel::deleteNode(int index)
{
    jassert(juce::isPositiveAndBelow(index, tree.getNumChildren()));
    undoManager.beginNewTransaction();
    tree.removeChild(index, &undoManager);
}

void EnvelopeModel::setControlPoints(int index, double cpOutTime, double cpOutValue,
                                                  double cpInTime,  double cpInValue)
{
    jassert(juce::isPositiveAndBelow(index, tree.getNumChildren()));
    undoManager.beginNewTransaction();

    auto node = tree.getChild(index);
    node.setProperty(EnvelopeIDs::cpOutTime,  cpOutTime,  &undoManager);
    node.setProperty(EnvelopeIDs::cpOutValue, cpOutValue, &undoManager);
    node.setProperty(EnvelopeIDs::cpInTime,   cpInTime,   &undoManager);
    node.setProperty(EnvelopeIDs::cpInValue,  cpInValue,  &undoManager);
}

EnvelopeModel::Node EnvelopeModel::getNode(int index) const
{
    jassert(juce::isPositiveAndBelow(index, tree.getNumChildren()));
    auto node = tree.getChild(index);

    Node n;
    n.time       = node[EnvelopeIDs::time];
    n.value      = node[EnvelopeIDs::value];
    n.cpOutTime  = node[EnvelopeIDs::cpOutTime];
    n.cpOutValue = node[EnvelopeIDs::cpOutValue];
    n.cpInTime   = node[EnvelopeIDs::cpInTime];
    n.cpInValue  = node[EnvelopeIDs::cpInValue];
    return n;
}

// Replaces the model state wholesale (e.g. preset load) and clears undo
// history — a loaded state is a fresh starting point, not a user edit.
void EnvelopeModel::setState(const juce::ValueTree& newState)
{
    jassert(newState.hasType(EnvelopeIDs::envelopeType));
    tree = newState;
    undoManager.clearUndoHistory();
}
