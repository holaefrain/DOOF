#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "EnvelopeModel.h"

// EnvelopeCanvas — the central canvas (project-architecture.md §3.4): renders
// the pitch and amp envelope curves overlaid on one shared time axis. Each
// curve is normalized into its own value range so both share the same
// vertical space despite having different units (pitch in Hz, amp unitless
// [0,1]).
//
// Step 3 (3a) adds click-drag node movement, wrapped in a single undo gesture
// per drag, with a value/frequency readout that follows the cursor. Step 3
// (3b) adds double-click-to-add a node. Step 3 (3c) adds dragging the curve
// itself (between nodes) to reshape its Bezier control points, with Shift
// held for fine-drag. Step 3 (3d) adds a right-click context menu (Delete
// Node, Reset Curve, Copy Curve, Paste Curve) — right-click no longer deletes
// instantly as it did in 3b; that's now a menu item alongside the others.
// Step 4 adds mouse-wheel zoom and empty-space-drag pan on the time axis —
// a rendering-only transform of the view window, never the model. Step 5
// adds a per-model "Length" (work-area duration) handle at the bottom of the
// canvas for each curve, draggable to bound where nodes can be added/moved.
// Step 6 adds a ~30 fps Timer that repaints when the host's tempo changes
// (the only state this component reads without an edit driving a repaint)
// and makes the component opaque with its own background fill, so redraws
// no longer require the parent editor to repaint behind it.
class EnvelopeCanvas : public juce::Component, private juce::Timer
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
    // into the shared display space. initialViewDuration is the shared
    // horizontal axis extent shown at startup, before any zoom/pan (Step 5's
    // Length control will add a separate notion of the model's own working
    // duration; this is purely the initial camera position). processorForPlayHead
    // must outlive this component; its play head (if any) drives the beat
    // grid's tempo. Repaints are otherwise only triggered by edits, so the
    // Step 6 timer polls the tempo and repaints when it changes — the one
    // piece of state this component displays that isn't driven by a
    // user-initiated call into this class.
    EnvelopeCanvas(EnvelopeModel& pitchModel, juce::Range<double> pitchRange,
                   EnvelopeModel& ampModel,   juce::Range<double> ampRange,
                   double initialViewDuration, juce::AudioProcessor& processorForPlayHead);

    void paint(juce::Graphics&) override;

    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Switches the pitch curve's vertical mapping between linear and log
    // scale (§3.4: "Log / Linear vertical-axis toggle"). Log applies to
    // pitch only — amp's range includes an exact 0.0 default node, where
    // log is undefined, and amp here is a normalized [0,1] value rather than
    // a dB gain, so linear is already its natural display. This is a pure
    // rendering-coordinate change: it never reads or writes the model, so it
    // cannot affect audio (drawEnvelope's model parameter is a const&).
    void setPitchLogScale(bool shouldUseLog);

    // Called after either model's Length changes via the canvas handle drag,
    // so external UI (e.g. the editor's numeric fields) can refresh. Not
    // called for changes made some other way (e.g. directly via the model).
    std::function<void()> onLengthChanged;

private:
    // The model for one curve. Almost every interaction here already knows which curve it is
    // acting on as a bool, so this replaces the pitch-or-amp ternary that was otherwise repeated
    // at fourteen call sites - one place to be wrong instead of fourteen. The const overload
    // exists because pointer members, unlike reference members, do not hand out a const referent
    // inside a const method.
    EnvelopeModel&       modelFor(bool isPitch)       { return isPitch ? *pitchModel : *ampModel; }
    const EnvelopeModel& modelFor(bool isPitch) const { return isPitch ? *pitchModel : *ampModel; }

    // Points sampled per Bezier segment when drawing a curve — fine enough
    // that individual line segments aren't visible at typical canvas sizes.
    static constexpr int kSamplesPerSegment = 32;

    // Maps a time to an x pixel coordinate across the current view window
    // [viewStartTime, viewStartTime + viewDuration].
    float timeToX(double time) const;

    // Inverse of timeToX: pixel x back to a time, NOT clamped to the view window.
    double xToTime(float x) const;

    // Clamps viewStartTime so the view window never goes before time 0 or
    // past the end of the valid domain (EnvelopeEvaluator::kTableDomainSeconds).
    void clampView();

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

    // Reads the host's current BPM, same as drawBeatGrid; a free function
    // would duplicate that logic, so this is the shared lookup both use.
    // Returns 0.0 if no play head or tempo is available.
    double getCurrentBpm() const;

    // Polls the host tempo at ~30 fps (kTimerHz) and repaints only when it
    // has actually changed since the last tick — most ticks do nothing.
    void timerCallback() override;
    static constexpr int kTimerHz = 30;
    double lastKnownBpm = 0.0;

    // Identifies a node handle under a pixel position, checking both curves
    // and returning whichever node handle is nearest (within a fixed hit
    // radius). index == -1 means nothing was hit; isPitch is meaningless
    // in that case.
    struct NodeHit { bool isPitch = false; int index = -1; };
    NodeHit findNodeAt(juce::Point<float> pos) const;

    // Deletes the node identified by hit. One undo step (deleteNode's own).
    void deleteNodeAt(NodeHit hit);

    // Right-click context menu (3d). If hit names a node, its curve is used
    // and "Delete Node" is offered; otherwise the currently active curve
    // (the "Editing" selector) is used and there's nothing to delete.
    void showContextMenu(NodeHit hit);

    // Replaces all of a curve's nodes with its out-of-the-box DefaultEnvelopes
    // shape. One undo step (wrapped in a gesture), so it can be undone like
    // any other edit.
    void resetCurve(bool isPitch);

    // Serializes a curve's node data to XML and puts it on the system
    // clipboard. Building block for copying an envelope shape between layers
    // in a later phase, and pasted cross-curve today via pasteCurve().
    void copyCurve(bool isPitch) const;

    // Replaces a curve's nodes with whatever envelope XML is on the system
    // clipboard (as put there by copyCurve, possibly from the other curve).
    // Does nothing if the clipboard doesn't hold valid envelope XML. One
    // undo step (wrapped in a gesture).
    void pasteCurve(bool isPitch);

    // Draws the drag readout (§3.4: "Frequency readout while dragging
    // nodes") near the dragged node's current position.
    void drawDragReadout(juce::Graphics& g) const;

    // Identifies a point on a curve's drawn path (not a node handle) under a
    // pixel position — the segment between node[segmentIndex] and
    // node[segmentIndex+1], and the Bezier parameter t nearest that pixel.
    // segmentIndex == -1 means nothing was hit.
    struct CurveHit { bool isPitch = false; int segmentIndex = -1; double t = 0.0; };
    CurveHit findCurveAt(juce::Point<float> pos) const;

    // Draws a curve's Length handle: a small triangle at the bottom of the
    // canvas at its work-area boundary, plus a thin dashed line marking it.
    void drawLengthHandle(juce::Graphics& g, const EnvelopeModel& model, juce::Colour colour) const;

    // Hit-tests both curves' Length handles near the bottom edge. Neither
    // isPitch is meaningful when hit is false.
    struct LengthHandleHit { bool isPitch = false; bool hit = false; };
    LengthHandleHit findLengthHandleAt(juce::Point<float> pos) const;

    // Minimum Length, so the work area can never collapse to nothing.
    static constexpr double kMinLength = 0.01;

    // Fraction applied to the drag delta while Shift is held, for precise
    // reshaping (3c: "modifier key for fine-drag").
    static constexpr double kFineDragScale = 0.2;

    // Zoom bounds (Step 4): can't zoom in below 10 ms, can't zoom out past
    // the full valid domain (there's nothing beyond it to show).
    static constexpr double kMinViewDuration = 0.01;

    // Fraction of the current view duration zoomed per wheel notch.
    static constexpr double kZoomStep = 0.15;

    // Pointers rather than references so a later step can point the canvas at a different layer's
    // models without rebuilding the component. Never null: set at construction and only ever
    // reseated to another valid pair.
    EnvelopeModel* pitchModel;
    juce::Range<double> pitchRange;
    EnvelopeModel* ampModel;
    juce::Range<double> ampRange;

    // The current view window: [viewStartTime, viewStartTime + viewDuration]
    // is what's mapped across the canvas's full width. Rendering-only state —
    // never read by anything outside this component, never touches the model.
    double viewStartTime = 0.0;
    double viewDuration;

    juce::AudioProcessor& processor;
    bool pitchLogScale = false;
    ActiveCurve activeCurve = ActiveCurve::pitch;

    // Drag state: which node (if any) is currently being dragged, and which
    // model it belongs to. index == -1 means no active drag.
    bool draggingPitch = false;
    int draggedNodeIndex = -1;
    juce::Point<float> readoutPos;
    juce::String readoutText;

    // Reshape state (3c): dragging the curve between two nodes adjusts both
    // control points of that segment together. segmentIndex == -1 means no
    // active reshape. The "orig" values are captured at mouseDown and held
    // fixed for the whole gesture; every mouseDrag recomputes the new control
    // points from these plus the total mouse delta so far, rather than
    // incrementally, to avoid compounding error across many small deltas.
    bool reshapingPitch = false;
    int reshapeSegmentIndex = -1;
    double reshapeT = 0.0;
    double reshapeOrigCpOutTime = 0.0, reshapeOrigCpOutValue = 0.0;
    double reshapeOrigCpInTime  = 0.0, reshapeOrigCpInValue  = 0.0;
    double reshapeOrigMouseTime = 0.0, reshapeOrigMouseValue = 0.0;

    // Pan state (Step 4): dragging empty space (not a node or curve) pans
    // the view. Captured at mouseDown and held fixed for the whole gesture,
    // same reasoning as the reshape state above.
    bool panning = false;
    double panOrigViewStart = 0.0;
    float panOrigMouseX = 0.0f;

    // Length-handle drag state (Step 5). draggingLength == false means no
    // active Length drag; lengthDragIsPitch says which curve's handle.
    bool draggingLength = false;
    bool lengthDragIsPitch = false;
};
