#include "EnvelopeCanvas.h"
#include "BezierSegment.h"
#include "DefaultEnvelopes.h"
#include "EnvelopeEvaluator.h"
#include <cmath>

EnvelopeCanvas::EnvelopeCanvas(EnvelopeModel& pitchModelIn, juce::Range<double> pitchRangeIn,
                               EnvelopeModel& ampModelIn,   juce::Range<double> ampRangeIn,
                               double initialViewDuration, juce::AudioProcessor& processorForPlayHead)
    : pitchModel(&pitchModelIn), pitchRange(pitchRangeIn),
      ampModel(&ampModelIn),     ampRange(ampRangeIn),
      viewDuration(initialViewDuration), processor(processorForPlayHead)
{
    // Opaque + self-drawn background (paint() below) means a repaint of this
    // component no longer requires the parent editor to repaint behind it.
    setOpaque(true);
    lastKnownBpm = getCurrentBpm();
    startTimerHz(kTimerHz);
}

float EnvelopeCanvas::timeToX(double time) const
{
    return (float) ((time - viewStartTime) / viewDuration * getWidth());
}

double EnvelopeCanvas::xToTime(float x) const
{
    return viewStartTime + (double) x / (double) getWidth() * viewDuration;
}

void EnvelopeCanvas::clampView()
{
    const double maxStart = juce::jmax(0.0, EnvelopeEvaluator::kTableDomainSeconds - viewDuration);
    viewStartTime = juce::jlimit(0.0, maxStart, viewStartTime);
}

double EnvelopeCanvas::yToValue(float y, juce::Range<double> valueRange, bool useLog) const
{
    const double normalized = 1.0 - (double) y / (double) getHeight();
    if (useLog)
    {
        const double logStart = std::log(valueRange.getStart());
        const double logEnd   = std::log(valueRange.getEnd());
        return std::exp(logStart + normalized * (logEnd - logStart));
    }
    return valueRange.getStart() + normalized * valueRange.getLength();
}

juce::Point<float> EnvelopeCanvas::toPixel(double time, double value, juce::Range<double> valueRange, bool useLog) const
{
    double normalized;
    if (useLog)
    {
        const double logStart = std::log(valueRange.getStart());
        const double logEnd   = std::log(valueRange.getEnd());
        const double logValue = std::log(juce::jmax(value, valueRange.getStart()));
        normalized = (logValue - logStart) / (logEnd - logStart);
    }
    else
    {
        normalized = (value - valueRange.getStart()) / valueRange.getLength();
    }

    const float y = (float) getHeight() * (float) (1.0 - normalized);
    return { timeToX(time), y };
}

void EnvelopeCanvas::setPitchLogScale(bool shouldUseLog)
{
    pitchLogScale = shouldUseLog;
    repaint();
}

double EnvelopeCanvas::getCurrentBpm() const
{
    auto* playHead = processor.getPlayHead();
    if (playHead == nullptr)
        return 0.0;

    const auto position = playHead->getPosition();
    if (! position.hasValue())
        return 0.0;

    const auto bpm = position->getBpm();
    return (bpm.hasValue() && *bpm > 0.0) ? *bpm : 0.0;
}

void EnvelopeCanvas::drawBeatGrid(juce::Graphics& g) const
{
    const double bpm = getCurrentBpm();
    if (bpm <= 0.0)
        return;

    const double beatSeconds = 60.0 / bpm;
    const double viewEnd     = viewStartTime + viewDuration;
    const double firstBeat   = std::ceil(viewStartTime / beatSeconds) * beatSeconds;

    g.setColour(juce::Colours::white.withAlpha(0.15f));
    for (double t = firstBeat; t <= viewEnd; t += beatSeconds)
        g.drawVerticalLine((int) timeToX(t), 0.0f, (float) getHeight());
}

void EnvelopeCanvas::timerCallback()
{
    const double bpm = getCurrentBpm();
    if (std::abs(bpm - lastKnownBpm) > 1.0e-6)
    {
        lastKnownBpm = bpm;
        repaint();
    }
}

void EnvelopeCanvas::drawEnvelope(juce::Graphics& g, const EnvelopeModel& model,
                                   juce::Range<double> valueRange, juce::Colour colour, bool useLog) const
{
    const int numNodes = model.getNumNodes();

    juce::Path path;
    for (int i = 0; i + 1 < numNodes; ++i)
    {
        const auto a = model.getNode(i);
        const auto b = model.getNode(i + 1);

        const BezierSegment::Point p0 { a.time,      a.value };
        const BezierSegment::Point c1 { a.cpOutTime, a.cpOutValue };
        const BezierSegment::Point c2 { b.cpInTime,  b.cpInValue };
        const BezierSegment::Point p3 { b.time,      b.value };

        for (int s = 0; s <= kSamplesPerSegment; ++s)
        {
            const double t = (double) s / (double) kSamplesPerSegment;
            const auto pt = BezierSegment::pointAt(p0, c1, c2, p3, t);
            const auto px = toPixel(pt.time, pt.value, valueRange, useLog);

            if (i == 0 && s == 0)
                path.startNewSubPath(px);
            else
                path.lineTo(px);
        }
    }

    g.setColour(colour);
    g.strokePath(path, juce::PathStrokeType(2.0f));

    static constexpr float kNodeRadius = 4.0f;
    for (int i = 0; i < numNodes; ++i)
    {
        const auto n  = model.getNode(i);
        const auto px = toPixel(n.time, n.value, valueRange, useLog);
        g.fillEllipse(px.x - kNodeRadius, px.y - kNodeRadius, kNodeRadius * 2.0f, kNodeRadius * 2.0f);
    }
}

void EnvelopeCanvas::drawDragReadout(juce::Graphics& g) const
{
    if (draggedNodeIndex == -1)
        return;

    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.drawText(readoutText, (int) readoutPos.x + 10, (int) readoutPos.y - 20, 160, 18,
               juce::Justification::centredLeft);
}

void EnvelopeCanvas::drawLengthHandle(juce::Graphics& g, const EnvelopeModel& model, juce::Colour colour) const
{
    const float x = timeToX(model.getLength());
    if (x < 0.0f || x > (float) getWidth())
        return; // off-screen at the current zoom/pan — nothing to draw

    static constexpr float dashLengths[] = { 4.0f, 4.0f };
    g.setColour(colour.withAlpha(0.35f));
    g.drawDashedLine({ x, 0.0f, x, (float) getHeight() }, dashLengths, 2);

    static constexpr float kHandleHalfWidth = 6.0f;
    static constexpr float kHandleHeight    = 10.0f;
    const float baseY = (float) getHeight();

    juce::Path triangle;
    triangle.addTriangle(x - kHandleHalfWidth, baseY,
                          x + kHandleHalfWidth, baseY,
                          x,                    baseY - kHandleHeight);
    g.setColour(colour);
    g.fillPath(triangle);
}

void EnvelopeCanvas::paint(juce::Graphics& g)
{
    // Opaque component (Step 6): must fill its own background now, since the
    // parent editor's dark background is no longer composited behind it.
    g.fillAll(juce::Colour(0xff1a1a1a));

    drawBeatGrid(g);
    drawEnvelope(g, *pitchModel, pitchRange, juce::Colours::cyan,   pitchLogScale);
    drawEnvelope(g, *ampModel,   ampRange,   juce::Colours::orange, false); // amp always linear
    drawLengthHandle(g, *pitchModel, juce::Colours::cyan);
    drawLengthHandle(g, *ampModel,   juce::Colours::orange);
    drawDragReadout(g);
}

EnvelopeCanvas::NodeHit EnvelopeCanvas::findNodeAt(juce::Point<float> pos) const
{
    static constexpr float kHitRadius = 10.0f;

    NodeHit best;
    float bestDistSq = kHitRadius * kHitRadius;

    auto check = [&](const EnvelopeModel& model, juce::Range<double> range, bool useLog, bool isPitch)
    {
        for (int i = 0; i < model.getNumNodes(); ++i)
        {
            const auto n = model.getNode(i);
            const auto px = toPixel(n.time, n.value, range, useLog);
            const float distSq = px.getDistanceSquaredFrom(pos);
            if (distSq <= bestDistSq)
            {
                bestDistSq = distSq;
                best = { isPitch, i };
            }
        }
    };

    check(*pitchModel, pitchRange, pitchLogScale, true);
    check(*ampModel,   ampRange,   false,         false);

    return best;
}

EnvelopeCanvas::LengthHandleHit EnvelopeCanvas::findLengthHandleAt(juce::Point<float> pos) const
{
    static constexpr float kHitHalfWidth = 8.0f;
    static constexpr float kHitBandHeight = 14.0f; // near the bottom edge, matching the drawn handle

    if (pos.y < (float) getHeight() - kHitBandHeight)
        return {};

    const float pitchX = timeToX(pitchModel->getLength());
    if (std::abs(pos.x - pitchX) <= kHitHalfWidth)
        return { true, true };

    const float ampX = timeToX(ampModel->getLength());
    if (std::abs(pos.x - ampX) <= kHitHalfWidth)
        return { false, true };

    return {};
}

void EnvelopeCanvas::deleteNodeAt(NodeHit hit)
{
    auto& model = modelFor(hit.isPitch);
    model.deleteNode(hit.index);
    repaint();
}

void EnvelopeCanvas::resetCurve(bool isPitch)
{
    auto& model = modelFor(isPitch);

    model.beginGesture();
    while (model.getNumNodes() > 0)
        model.deleteNode(0);

    if (isPitch)
        DefaultEnvelopes::seedPitch(model);
    else
        DefaultEnvelopes::seedAmp(model);

    model.endGesture();
    repaint();
}

void EnvelopeCanvas::copyCurve(bool isPitch) const
{
    const auto& model = modelFor(isPitch);
    const std::unique_ptr<juce::XmlElement> xml(model.getValueTree().createXml());
    juce::SystemClipboard::copyTextToClipboard(xml->toString());
}

void EnvelopeCanvas::pasteCurve(bool isPitch)
{
    const auto xml = juce::parseXML(juce::SystemClipboard::getTextFromClipboard());
    if (xml == nullptr)
        return;

    const auto tree = juce::ValueTree::fromXml(*xml);
    if (! tree.hasType(EnvelopeIDs::envelopeType))
        return;

    auto& model = modelFor(isPitch);

    model.beginGesture();
    while (model.getNumNodes() > 0)
        model.deleteNode(0);

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto srcNode = tree.getChild(i);
        const int index = model.addNode(srcNode[EnvelopeIDs::time], srcNode[EnvelopeIDs::value]);
        model.setControlPoints(index,
                                srcNode[EnvelopeIDs::cpOutTime], srcNode[EnvelopeIDs::cpOutValue],
                                srcNode[EnvelopeIDs::cpInTime],  srcNode[EnvelopeIDs::cpInValue]);
    }

    model.endGesture();
    repaint();
}

void EnvelopeCanvas::showContextMenu(NodeHit hit)
{
    const bool isPitch = (hit.index != -1) ? hit.isPitch : (activeCurve == ActiveCurve::pitch);

    juce::PopupMenu menu;
    if (hit.index != -1)
        menu.addItem("Delete Node", [this, hit] { deleteNodeAt(hit); });
    menu.addItem("Reset Curve", [this, isPitch] { resetCurve(isPitch); });
    menu.addItem("Copy Curve",  [this, isPitch] { copyCurve(isPitch); });
    menu.addItem("Paste Curve", [this, isPitch] { pasteCurve(isPitch); });

    menu.showMenuAsync(juce::PopupMenu::Options());
}

// Squared distance from p to the line segment [a, b], plus the parametric
// position (0-1) of the closest point along that segment.
static float distanceSquaredToSegment(juce::Point<float> p, juce::Point<float> a, juce::Point<float> b, float& outFraction)
{
    const auto ab = b - a;
    const float lengthSq = ab.x * ab.x + ab.y * ab.y;
    outFraction = lengthSq > 1.0e-6f
                    ? juce::jlimit(0.0f, 1.0f, ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lengthSq)
                    : 0.0f;
    const auto closest = a + ab * outFraction;
    return closest.getDistanceSquaredFrom(p);
}

EnvelopeCanvas::CurveHit EnvelopeCanvas::findCurveAt(juce::Point<float> pos) const
{
    static constexpr float kHitRadius = 10.0f;

    CurveHit best;
    float bestDistSq = kHitRadius * kHitRadius;

    auto check = [&](const EnvelopeModel& model, juce::Range<double> range, bool useLog, bool isPitch)
    {
        const int numNodes = model.getNumNodes();
        for (int i = 0; i + 1 < numNodes; ++i)
        {
            const auto a = model.getNode(i);
            const auto b = model.getNode(i + 1);

            const BezierSegment::Point p0 { a.time,      a.value };
            const BezierSegment::Point c1 { a.cpOutTime, a.cpOutValue };
            const BezierSegment::Point c2 { b.cpInTime,  b.cpInValue };
            const BezierSegment::Point p3 { b.time,      b.value };

            // Compare against the line segments the path is actually drawn
            // with (between consecutive samples), not just the sample points
            // themselves — otherwise most of the visible curve falls in gaps
            // between samples too far apart for a tight hit radius to catch.
            auto prevPx = toPixel(p0.time, p0.value, range, useLog);
            for (int s = 1; s <= kSamplesPerSegment; ++s)
            {
                const double t0 = (double) (s - 1) / (double) kSamplesPerSegment;
                const double t1 = (double) s       / (double) kSamplesPerSegment;
                const auto pt1 = BezierSegment::pointAt(p0, c1, c2, p3, t1);
                const auto px1 = toPixel(pt1.time, pt1.value, range, useLog);

                float fraction = 0.0f;
                const float distSq = distanceSquaredToSegment(pos, prevPx, px1, fraction);
                if (distSq <= bestDistSq)
                {
                    bestDistSq = distSq;
                    best = { isPitch, i, t0 + fraction * (t1 - t0) };
                }

                prevPx = px1;
            }
        }
    };

    check(*pitchModel, pitchRange, pitchLogScale, true);
    check(*ampModel,   ampRange,   false,         false);

    return best;
}

void EnvelopeCanvas::mouseDown(const juce::MouseEvent& e)
{
    const auto nodeHit = findNodeAt(e.position);

    if (e.mods.isPopupMenu())
    {
        showContextMenu(nodeHit);
        return;
    }

    if (nodeHit.index != -1)
    {
        draggingPitch    = nodeHit.isPitch;
        draggedNodeIndex = nodeHit.index;

        auto& model = modelFor(draggingPitch);
        model.beginGesture();
        return;
    }

    const auto lengthHit = findLengthHandleAt(e.position);
    if (lengthHit.hit)
    {
        draggingLength      = true;
        lengthDragIsPitch   = lengthHit.isPitch;

        auto& model = modelFor(lengthDragIsPitch);
        model.beginGesture();
        return;
    }

    const auto curveHit = findCurveAt(e.position);
    if (curveHit.segmentIndex == -1)
    {
        // Neither a node nor the curve line: empty space. Pan the view (Step 4).
        panning          = true;
        panOrigViewStart = viewStartTime;
        panOrigMouseX    = e.position.x;
        return;
    }

    reshapingPitch      = curveHit.isPitch;
    reshapeSegmentIndex = curveHit.segmentIndex;
    reshapeT             = curveHit.t;

    auto& model = modelFor(reshapingPitch);
    const auto a = model.getNode(reshapeSegmentIndex);
    const auto b = model.getNode(reshapeSegmentIndex + 1);
    reshapeOrigCpOutTime  = a.cpOutTime;
    reshapeOrigCpOutValue = a.cpOutValue;
    reshapeOrigCpInTime   = b.cpInTime;
    reshapeOrigCpInValue  = b.cpInValue;

    auto& valueRange  = reshapingPitch ? pitchRange : ampRange;
    const bool useLog = reshapingPitch && pitchLogScale;
    reshapeOrigMouseTime  = xToTime(e.position.x);
    reshapeOrigMouseValue = yToValue(e.position.y, valueRange, useLog);

    model.beginGesture();
}

void EnvelopeCanvas::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto hit = findNodeAt(e.position);
    if (hit.index != -1)
    {
        deleteNodeAt(hit);
        return;
    }

    auto& model        = modelFor(activeCurve == ActiveCurve::pitch);
    auto& valueRange    = (activeCurve == ActiveCurve::pitch) ? pitchRange : ampRange;
    const bool useLog   = (activeCurve == ActiveCurve::pitch) && pitchLogScale;

    const double time  = juce::jlimit(0.0, model.getLength(), xToTime(e.position.x));
    const double value = juce::jlimit(valueRange.getStart(), valueRange.getEnd(),
                                       yToValue(e.position.y, valueRange, useLog));

    model.addNode(time, value);
    repaint();
}

void EnvelopeCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedNodeIndex != -1)
    {
        auto& model      = modelFor(draggingPitch);
        auto& valueRange  = draggingPitch ? pitchRange : ampRange;
        const bool useLog = draggingPitch && pitchLogScale;

        const double newTime  = juce::jlimit(0.0, model.getLength(), xToTime(e.position.x));
        const double newValue = juce::jlimit(valueRange.getStart(), valueRange.getEnd(),
                                              yToValue(e.position.y, valueRange, useLog));

        // moveNode can return a different index than the one passed in, when
        // the drag crosses a neighbouring node's time and the model re-sorts
        // — this return value must be reassigned every call (lesson from the
        // Phase 3 Step 0 spike, where discarding it left the tracked node stale).
        draggedNodeIndex = model.moveNode(draggedNodeIndex, newTime, newValue);

        readoutPos  = toPixel(newTime, newValue, valueRange, useLog);
        readoutText = draggingPitch
                        ? juce::String(newValue, 1) + " Hz"
                        : juce::String(newValue, 3);
        readoutText += "  @ " + juce::String(newTime * 1000.0, 1) + " ms";

        repaint();
        return;
    }

    if (reshapeSegmentIndex != -1)
    {
        auto& model       = modelFor(reshapingPitch);
        auto& valueRange   = reshapingPitch ? pitchRange : ampRange;
        const bool useLog  = reshapingPitch && pitchLogScale;

        double deltaTime  = xToTime(e.position.x) - reshapeOrigMouseTime;
        double deltaValue = yToValue(e.position.y, valueRange, useLog) - reshapeOrigMouseValue;

        // Shift held: fine-drag at reduced sensitivity for precise reshaping (3c).
        if (e.mods.isShiftDown())
        {
            deltaTime  *= kFineDragScale;
            deltaValue *= kFineDragScale;
        }

        // Moving both control points of a segment by the same delta shifts
        // the curve's point at parameter t by 3*t*(1-t)*delta (the De
        // Casteljau basis weights for the two control points sum to that
        // when moved together) — dividing by that factor makes the curve
        // pass exactly through the cursor at the point originally grabbed.
        const double clampedT = juce::jlimit(0.05, 0.95, reshapeT);
        const double weight   = 3.0 * clampedT * (1.0 - clampedT);
        const double cpDeltaTime  = deltaTime  / weight;
        const double cpDeltaValue = deltaValue / weight;

        const auto nodeA = model.getNode(reshapeSegmentIndex);
        const auto nodeB = model.getNode(reshapeSegmentIndex + 1);

        // Control-point time is clamped to the segment's own [nodeA, nodeB]
        // time range so the curve stays monotonic in time — BezierSegment's
        // bisection-based lookup (used by the audio-thread evaluator) is only
        // defined for monotonic segments.
        const double newCpOutTime  = juce::jlimit(nodeA.time, nodeB.time, reshapeOrigCpOutTime + cpDeltaTime);
        const double newCpInTime   = juce::jlimit(nodeA.time, nodeB.time, reshapeOrigCpInTime  + cpDeltaTime);
        const double newCpOutValue = reshapeOrigCpOutValue + cpDeltaValue;
        const double newCpInValue  = reshapeOrigCpInValue  + cpDeltaValue;

        model.setControlPoints(reshapeSegmentIndex, newCpOutTime, newCpOutValue,
                                                      nodeA.cpInTime, nodeA.cpInValue);
        model.setControlPoints(reshapeSegmentIndex + 1, nodeB.cpOutTime, nodeB.cpOutValue,
                                                          newCpInTime, newCpInValue);

        repaint();
        return;
    }

    if (panning)
    {
        // Dragging right reveals earlier time (the content follows the
        // cursor, like grabbing a scrollable canvas).
        const float dx = e.position.x - panOrigMouseX;
        viewStartTime = panOrigViewStart - (double) dx / (double) getWidth() * viewDuration;
        clampView();
        repaint();
        return;
    }

    if (draggingLength)
    {
        auto& model = modelFor(lengthDragIsPitch);
        const double newLength = juce::jlimit(kMinLength, EnvelopeEvaluator::kTableDomainSeconds, xToTime(e.position.x));
        model.setLength(newLength);

        if (onLengthChanged)
            onLengthChanged();

        repaint();
        return;
    }
}

void EnvelopeCanvas::mouseUp(const juce::MouseEvent&)
{
    if (draggedNodeIndex != -1)
    {
        auto& model = modelFor(draggingPitch);
        model.endGesture();
        draggedNodeIndex = -1;
        repaint();
        return;
    }

    if (reshapeSegmentIndex != -1)
    {
        auto& model = modelFor(reshapingPitch);
        model.endGesture();
        reshapeSegmentIndex = -1;
        repaint();
        return;
    }

    if (panning)
    {
        panning = false;
        return;
    }

    if (draggingLength)
    {
        auto& model = modelFor(lengthDragIsPitch);
        model.endGesture();
        draggingLength = false;
        return;
    }
}

void EnvelopeCanvas::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
        return;

    // Zoom centered on the cursor: keep the time under the cursor fixed
    // while the view duration shrinks/grows around it.
    const double anchorTime     = xToTime(e.position.x);
    const double anchorFraction = juce::jlimit(0.0, 1.0, (double) e.position.x / (double) getWidth());

    const double factor = (wheel.deltaY > 0.0f) ? (1.0 - kZoomStep) : (1.0 / (1.0 - kZoomStep));
    viewDuration = juce::jlimit(kMinViewDuration, EnvelopeEvaluator::kTableDomainSeconds, viewDuration * factor);

    viewStartTime = anchorTime - anchorFraction * viewDuration;
    clampView();

    repaint();
}
