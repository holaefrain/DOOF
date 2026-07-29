#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "EnvelopeModel.h"

// EnvelopeCanvas — the central canvas (project-architecture.md §3.4): renders
// the pitch and amp envelope curves overlaid on one shared time axis. Each
// curve is normalized into its own value range so both share the same
// vertical space despite having different units (pitch in Hz, amp unitless
// [0,1]).
//
// Read-only in Step 2 (2a): draws the current model state only. Step 3 adds
// drag/add/delete/reshape interaction directly on this component.
class EnvelopeCanvas : public juce::Component
{
public:
    // pitchModel/ampModel must outlive this component. pitchRange/ampRange
    // are each model's vertical axis extent in its own units, used only to
    // normalize into the shared display space. visibleSeconds is the shared
    // horizontal axis extent (Step 5's Length control will make this
    // adjustable; fixed here for the initial render).
    EnvelopeCanvas(const EnvelopeModel& pitchModel, juce::Range<double> pitchRange,
                   const EnvelopeModel& ampModel,   juce::Range<double> ampRange,
                   double visibleSeconds);

    void paint(juce::Graphics&) override;

private:
    // Points sampled per Bezier segment when drawing a curve — fine enough
    // that individual line segments aren't visible at typical canvas sizes.
    static constexpr int kSamplesPerSegment = 32;

    // Maps a (time, value) point into pixel space, normalizing value against
    // valueRange first so different curves share the canvas's full height.
    juce::Point<float> toPixel(double time, double value, juce::Range<double> valueRange) const;

    // Draws one model's curve (reusing BezierSegment's De Casteljau evaluator)
    // plus a handle at each node, in the given colour.
    void drawEnvelope(juce::Graphics& g, const EnvelopeModel& model,
                       juce::Range<double> valueRange, juce::Colour colour) const;

    const EnvelopeModel& pitchModel;
    juce::Range<double> pitchRange;
    const EnvelopeModel& ampModel;
    juce::Range<double> ampRange;
    double visibleSeconds;
};
