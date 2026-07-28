#pragma once
#include "EnvelopeEvaluator.h"
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
    EnvelopeSnapshot(std::vector<float> pitch, std::vector<float> amp)
        : pitchTable(std::move(pitch)), ampTable(std::move(amp))
    {
    }

    const std::vector<float> pitchTable;
    const std::vector<float> ampTable;

    // Both tables share this domain/resolution (EnvelopeEvaluator::kTableSize /
    // kTableDomainSeconds at construction time). Stored here so audio-thread
    // code can index the tables without depending on EnvelopeEvaluator.h.
    const int    tableSize          = EnvelopeEvaluator::kTableSize;
    const double tableDomainSeconds = EnvelopeEvaluator::kTableDomainSeconds;
};
