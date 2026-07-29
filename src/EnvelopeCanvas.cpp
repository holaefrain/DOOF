#include "EnvelopeCanvas.h"
#include "BezierSegment.h"
#include <cmath>

EnvelopeCanvas::EnvelopeCanvas(EnvelopeModel& pitchModelIn, juce::Range<double> pitchRangeIn,
                               EnvelopeModel& ampModelIn,   juce::Range<double> ampRangeIn,
                               double visibleSecondsIn, juce::AudioProcessor& processorForPlayHead)
    : pitchModel(pitchModelIn), pitchRange(pitchRangeIn),
      ampModel(ampModelIn),     ampRange(ampRangeIn),
      visibleSeconds(visibleSecondsIn), processor(processorForPlayHead)
{
}

float EnvelopeCanvas::timeToX(double time) const
{
    return (float) (time / visibleSeconds * getWidth());
}

double EnvelopeCanvas::xToTime(float x) const
{
    return (double) x / (double) getWidth() * visibleSeconds;
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

void EnvelopeCanvas::drawBeatGrid(juce::Graphics& g) const
{
    auto* playHead = processor.getPlayHead();
    if (playHead == nullptr)
        return;

    const auto position = playHead->getPosition();
    if (! position.hasValue())
        return;

    const auto bpm = position->getBpm();
    if (! bpm.hasValue() || *bpm <= 0.0)
        return;

    const double beatSeconds = 60.0 / *bpm;

    g.setColour(juce::Colours::white.withAlpha(0.15f));
    for (double t = 0.0; t <= visibleSeconds; t += beatSeconds)
        g.drawVerticalLine((int) timeToX(t), 0.0f, (float) getHeight());
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

void EnvelopeCanvas::paint(juce::Graphics& g)
{
    drawBeatGrid(g);
    drawEnvelope(g, pitchModel, pitchRange, juce::Colours::cyan,   pitchLogScale);
    drawEnvelope(g, ampModel,   ampRange,   juce::Colours::orange, false); // amp always linear
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

    check(pitchModel, pitchRange, pitchLogScale, true);
    check(ampModel,   ampRange,   false,         false);

    return best;
}

void EnvelopeCanvas::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = findNodeAt(e.position);
    if (hit.index == -1)
        return;

    draggingPitch    = hit.isPitch;
    draggedNodeIndex = hit.index;

    auto& model = draggingPitch ? pitchModel : ampModel;
    model.beginGesture();
}

void EnvelopeCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (draggedNodeIndex == -1)
        return;

    auto& model      = draggingPitch ? pitchModel : ampModel;
    auto& valueRange  = draggingPitch ? pitchRange : ampRange;
    const bool useLog = draggingPitch && pitchLogScale;

    const double newTime  = juce::jlimit(0.0, visibleSeconds, xToTime(e.position.x));
    const double newValue = juce::jlimit(valueRange.getStart(), valueRange.getEnd(),
                                          yToValue(e.position.y, valueRange, useLog));

    // moveNode can return a different index than the one passed in, when the
    // drag crosses a neighbouring node's time and the model re-sorts — this
    // return value must be reassigned every call (lesson from the Phase 3
    // Step 0 spike, where discarding it left the tracked node stale).
    draggedNodeIndex = model.moveNode(draggedNodeIndex, newTime, newValue);

    readoutPos  = toPixel(newTime, newValue, valueRange, useLog);
    readoutText = draggingPitch
                    ? juce::String(newValue, 1) + " Hz"
                    : juce::String(newValue, 3);
    readoutText += "  @ " + juce::String(newTime * 1000.0, 1) + " ms";

    repaint();
}

void EnvelopeCanvas::mouseUp(const juce::MouseEvent&)
{
    if (draggedNodeIndex == -1)
        return;

    auto& model = draggingPitch ? pitchModel : ampModel;
    model.endGesture();

    draggedNodeIndex = -1;
    repaint();
}
