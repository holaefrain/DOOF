#pragma once

// BezierSegment — evaluates a single cubic Bezier curve segment between two
// envelope nodes, as a function of time rather than the raw Bezier parameter.
//
// An envelope's pitch/amp curve is a function of time: for any time within a
// segment there is exactly one value. A cubic Bezier's own parameter t does
// not correspond linearly to time, so evaluating "value at time T" requires
// first inverting the curve's time coordinate to find the t that produces T,
// then reading the value coordinate at that t.
//
// Precondition: p0.time < p3.time, and the segment's time coordinate must be
// monotonically non-decreasing in t (i.e. control points don't fold the curve
// backward in time). Nothing here enforces that — the Phase 3 canvas is
// expected to constrain control-point dragging so this always holds; a
// non-monotonic segment gives undefined results from valueAtTime's bisection.
//
// No JUCE dependency: pure math, so it's cheap to unit test in isolation.
namespace BezierSegment
{
    // A point in (time, value) space — either an envelope node or a control point.
    struct Point
    {
        double time  = 0.0;
        double value = 0.0;
    };

    // Point on the curve at Bezier parameter t in [0, 1], via De Casteljau's
    // algorithm on the four control points (p0 = segment start, p3 = segment end).
    Point pointAt(const Point& p0, const Point& c1, const Point& c2, const Point& p3, double t);

    // Value of the envelope at absolute `time`, found by bisecting on t until
    // the curve's time coordinate matches `time`, then reading the value there.
    // `time` is clamped into [p0.time, p3.time] before inverting.
    double valueAtTime(const Point& p0, const Point& c1, const Point& c2, const Point& p3, double time);
}
