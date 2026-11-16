#pragma once
#include "EnvelopeEvaluator.h"
#include <juce_core/juce_core.h> // jassert
#include <vector>
#include <utility>

// EnvelopeSnapshot — immutable, fully-baked pitch + amp envelope tables handed
// to the audio thread (§2: "publish an immutable snapshot to the audio thread
// via a single atomic-pointer swap"). Constructed once on the message thread
// and never mutated afterward; once published, every field is safe to read
// from any thread without synchronization. The audio thread never reads the
// live EnvelopeModel/ValueTree — only ever this type, via the atomic pointer
// EnvelopePublisher (Step 3b) maintains.
struct EnvelopeSnapshot
{
    // keytrack may be left empty, meaning no node is keytracked - which is what every snapshot
    // built before Phase 6 meant, and what tests that only care about pitch and amp still pass.
    EnvelopeSnapshot(std::vector<float> pitch, std::vector<float> amp,
                     std::vector<float> keytrack = {},
                     EnvelopeEvaluator::LoopSpan loop = {},
                     double lockTime = -1.0)
        : pitchTable(std::move(pitch)), ampTable(std::move(amp)), keytrackTable(std::move(keytrack)),
          sustainLoop(loop), phaseLockTime(lockTime)
    {
        jassert(keytrackTable.empty() || keytrackTable.size() == pitchTable.size());
    }

    const std::vector<float> pitchTable;
    const std::vector<float> ampTable;

    // Phase 6 §3.5: how much of the played note's offset from the key root applies at each point
    // of the pitch table, in [0, 1] (EnvelopeEvaluator::buildKeytrackTable). Empty = all zeros.
    const std::vector<float> keytrackTable;

    // Phase 6 §3.5: the stretch sustain mode loops over while a note is held - first to last
    // keytracked node. Invalid (the default) when fewer than two nodes are flagged.
    const EnvelopeEvaluator::LoopSpan sustainLoop;

    // Phase 6 §3.6: when the Sub's phase is locked, in seconds, or -1 for no lock.
    const double phaseLockTime;

    // Both tables share this domain/resolution (EnvelopeEvaluator::kTableSize /
    // kTableDomainSeconds at construction time). Stored here so audio-thread
    // code can index the tables without depending on EnvelopeEvaluator.h.
    const int    tableSize          = EnvelopeEvaluator::kTableSize;
    const double tableDomainSeconds = EnvelopeEvaluator::kTableDomainSeconds;
};
