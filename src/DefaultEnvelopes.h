#pragma once
#include "EnvelopeModel.h"

// DefaultEnvelopes — the out-of-the-box Sub-layer pitch and amp envelope
// shapes. Shared between PluginProcessor (production defaults) and the
// Phase 1 regression tests, so those tests exercise the same envelope shape
// production code actually ships with, rather than duplicating the node data
// in two places that could quietly drift apart.
//
// These are a monotonic-sweep approximation of the original Phase 1 hardcoded
// formulas (exponential pitch sweep, linear-attack/exponential-decay amp) —
// not a numerically exact reproduction. A 2-node cubic Bezier with default
// (coincident) control points is exactly linear in time (see EnvelopeModel's
// addNode doc comment), so this trades exact curve-shape fidelity for
// simplicity; the Phase 1 verify tests check qualitative envelope behaviour
// (monotonic decay, relative RMS/zero-crossing-rate ordering), which this
// shape satisfies. Real curve shaping arrives with the Phase 3 canvas.
namespace DefaultEnvelopes
{
    // Monotonic sweep 150 Hz -> 50 Hz over 300 ms.
    void seedPitch(EnvelopeModel& model);

    // Brief linear attack to full level, then decay to near-silence by 400 ms.
    void seedAmp(EnvelopeModel& model);
}
