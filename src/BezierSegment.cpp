#include "BezierSegment.h"
#include <algorithm>

namespace BezierSegment
{

// De Casteljau's algorithm: repeatedly lerp between the four control points
// until a single point remains. Numerically stable and easy to verify by hand.
Point pointAt(const Point& p0, const Point& c1, const Point& c2, const Point& p3, double t)
{
    auto lerp = [] (const Point& a, const Point& b, double u) -> Point
    {
        return { a.time  + (b.time  - a.time)  * u,
                 a.value + (b.value - a.value) * u };
    };

    Point ab = lerp(p0, c1, t);
    Point bc = lerp(c1, c2, t);
    Point cd = lerp(c2, p3, t);

    Point abbc = lerp(ab, bc, t);
    Point bccd = lerp(bc, cd, t);

    return lerp(abbc, bccd, t);
}

double valueAtTime(const Point& p0, const Point& c1, const Point& c2, const Point& p3, double time)
{
    const double clampedTime = std::clamp(time, p0.time, p3.time);

    // Bisection on t: pointAt(...).time is assumed monotonically non-decreasing
    // in t (see header precondition), so a single sign change brackets the root.
    // 40 iterations gives ~2^-40 precision on t — far more than needed, but this
    // runs once per table sample at edit time, never at audio rate, so it's cheap.
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i)
    {
        const double mid     = 0.5 * (lo + hi);
        const double midTime = pointAt(p0, c1, c2, p3, mid).time;

        if (midTime < clampedTime)
            lo = mid;
        else
            hi = mid;
    }

    return pointAt(p0, c1, c2, p3, 0.5 * (lo + hi)).value;
}

} // namespace BezierSegment
