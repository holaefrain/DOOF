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
// Step 3 (3a) adds click-drag node movement, wrapped in a single undo gesture
// per drag, with a value/frequency readout that follows the cursor. Step 3
// (3b) adds double-click-to-add/delete and right-click-to-delete. Further
// Step 3 bullets (reshape/context menu) build on this.
class EnvelopeCanvas : public juce::Component
{
public:
    // Which curve double-click-on-empty-space adds a new node to. Chosen
    // explicitly rather than by cursor proximity to a curve, since proximity
    // is ambiguous near crossings and undefined when a model has zero nodes.
    enum class ActiveCurve { pitch, amp };
    void setActiveCurve(ActiveCurve curve) { activeCurve = curve; }
    // pitchModel/ampModel must outlive this component and are edited directly
    // by this component's drag interaction. pitchRange/ampRange are each
    // model's vertical axis extent in its own units, used only to normalize
    // into the shared display space. visibleSeconds is the shared horizontal
    // axis extent (Step 5's Length control will make this adjustable; fixed
    // here for the initial render). processorForPlayHead must outlive this
    // component; its play head (if any) drives the beat grid's tempo, read
    // fresh on every paint so host tempo changes show up without this
    // component needing to know when they happen.
    EnvelopeCanvas(EnvelopeModel& pitchModel, juce::Range<double> pitchRange,
                   EnvelopeModel& ampModel,   juce::Range<double> ampRange,
                   double visibleSeconds, juce::AudioProcessor& processorForPlayHead);

    void paint(juce::Graphics&) override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

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

    // Inverse of timeToX: pixel x back to a time, NOT clamped to [0, visibleSeconds].
    double xToTime(float x) const;

    // Inverse of a value's position within toPixel's y mapping (log-scaled if
    // useLog), NOT clamped to valueRange.
    double yToValue(float y, juce::Range<double> valueRange, bool useLog) const;

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

    // Identifies a node handle under a pixel position, checking both curves
    // and returning whichever node handle is nearest (within a fixed hit
    // radius). index == -1 means nothing was hit; isPitch is meaningless
    // in that case.
    struct NodeHit { bool isPitch = false; int index = -1; };
    NodeHit findNodeAt(juce::Point<float> pos) const;

    // Deletes the node identified by hit. One undo step (deleteNode's own).
    void deleteNodeAt(NodeHit hit);

    // Draws the drag readout (§3.4: "Frequency readout while dragging
    // nodes") near the dragged node's current position.
    void drawDragReadout(juce::Graphics& g) const;

    EnvelopeModel& pitchModel;
    juce::Range<double> pitchRange;
    EnvelopeModel& ampModel;
    juce::Range<double> ampRange;
    double visibleSeconds;
    juce::AudioProcessor& processor;
    bool pitchLogScale = false;
    ActiveCurve activeCurve = ActiveCurve::pitch;

    // Drag state: which node (if any) is currently being dragged, and which
    // model it belongs to. index == -1 means no active drag.
    bool draggingPitch = false;
    int draggedNodeIndex = -1;
    juce::Point<float> readoutPos;
    juce::String readoutText;
};
