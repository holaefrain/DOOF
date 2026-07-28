#pragma once
#include <vector>
#include "EnvelopeModel.h"

// EnvelopeEvaluator — turns an EnvelopeModel's node/Bezier data into a fixed-
// resolution lookup table of envelope values sampled evenly across a fixed
// time domain. This is the "precompute each envelope into a lookup table"
// step from §2 of project-architecture.md — Bezier curves are evaluated once
// here, at edit time, never per-sample at audio rate.
//
// Message-thread only (reads the live EnvelopeModel/ValueTree). The result is
// a plain std::vector<float>, safe to hand off to the audio thread as part of
// an immutable EnvelopeSnapshot (Step 3).
namespace EnvelopeEvaluator
{
    // Table resolution and time domain. 4096 samples over 4 seconds covers the
    // longest kick tail exercised so far (§6 Phase 1 verify renders out to 3 s)
    // with room to spare; revisit if §3.4's user-editable Length control ends
    // up needing a longer domain.
    constexpr int    kTableSize          = 4096;
    constexpr double kTableDomainSeconds = 4.0;

    // Builds a lookup table of kTableSize samples spanning [0, kTableDomainSeconds)
    // seconds. Evaluates each node-to-node cubic-Bezier segment at the sample
    // times that fall within it. Times before the first node or after the last
    // node hold at that node's value. An empty model produces an all-zero
    // (silent) table.
    std::vector<float> buildTable(const EnvelopeModel& model);
}
