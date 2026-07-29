#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
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
    // adjustable; fixed here for the initial render). processorForPlayHead
    // must outlive this component; its play head (if any) drives the beat
    // grid's tempo, read fresh on every paint so host tempo changes show up
    // without this component needing to know when they happen.
    EnvelopeCanvas(const EnvelopeModel& pitchModel, juce::Range<double> pitchRange,
                   const EnvelopeModel& ampModel,   juce::Range<double> ampRange,
                   double visibleSeconds, juce::AudioProcessor& processorForPlayHead);

    void paint(juce::Graphics&) override;

    // Switches the pitch curve's vertical mapping between linear and log
    // scale (§3.4: "Log / Linear vertical-axis toggle"). Log applies to
    // pitch only — amp's range includes an exact 0.0 default node, where
    // log is undefined, and amp here is a normalized [0,1] value rather than
    // a dB gain, so linear is already its natural display. This is a pure
    // rendering-coordinate change: it never reads or writes the model, so it
    // cannot affect audio (drawEnvelope's model parameter is a const&).
    void setPitchLogScale(bool shouldUseLog);

private:
    // Points sampled per Bezier segment when drawing a curve — fine enough
    // that individual line segments aren't visible at typical canvas sizes.
    static constexpr int kSamplesPerSegment = 32;

    // Maps a time to an x pixel coordinate across the visible time range.
    float timeToX(double time) const;

    // Maps a (time, value) point into pixel space, normalizing value against
    // valueRange first (log-scaled if useLog) so different curves share the
    // canvas's full height. valueRange's lower bound must be > 0 when
    // useLog is true.
    juce::Point<float> toPixel(double time, double value, juce::Range<double> valueRange, bool useLog) const;

    // Draws one model's curve (reusing BezierSegment's De Casteljau evaluator)
    // plus a handle at each node, in the given colour.
    void drawEnvelope(juce::Graphics& g, const EnvelopeModel& model,
                       juce::Range<double> valueRange, juce::Colour colour, bool useLog) const;

    // Vertical gridlines at each beat boundary across the visible time range,
    // read from the host's current tempo (§3.4: "Beat grid ... synced to host
    // BPM"). Silently skipped if the host hasn't provided a play head or a
    // tempo yet.
    void drawBeatGrid(juce::Graphics& g) const;

    const EnvelopeModel& pitchModel;
    juce::Range<double> pitchRange;
    const EnvelopeModel& ampModel;
    juce::Range<double> ampRange;
    double visibleSeconds;
    juce::AudioProcessor& processor;
    bool pitchLogScale = false;
};
