#include "EnvelopeCanvas.h"
#include "BezierSegment.h"

EnvelopeCanvas::EnvelopeCanvas(const EnvelopeModel& pitchModelIn, juce::Range<double> pitchRangeIn,
                               const EnvelopeModel& ampModelIn,   juce::Range<double> ampRangeIn,
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

juce::Point<float> EnvelopeCanvas::toPixel(double time, double value, juce::Range<double> valueRange) const
{
    const float normalized = (float) ((value - valueRange.getStart()) / valueRange.getLength());
    const float y = (float) getHeight() * (1.0f - normalized);
    return { timeToX(time), y };
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
                                   juce::Range<double> valueRange, juce::Colour colour) const
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
            const auto px = toPixel(pt.time, pt.value, valueRange);

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
        const auto px = toPixel(n.time, n.value, valueRange);
        g.fillEllipse(px.x - kNodeRadius, px.y - kNodeRadius, kNodeRadius * 2.0f, kNodeRadius * 2.0f);
    }
}

void EnvelopeCanvas::paint(juce::Graphics& g)
{
    drawBeatGrid(g);
    drawEnvelope(g, pitchModel, pitchRange, juce::Colours::cyan);
    drawEnvelope(g, ampModel,   ampRange,   juce::Colours::orange);
}
