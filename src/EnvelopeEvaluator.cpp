#include "EnvelopeEvaluator.h"
#include "BezierSegment.h"
#include <algorithm>
#include <cmath>

namespace EnvelopeEvaluator
{

std::vector<float> buildTable(const EnvelopeModel& model)
{
    std::vector<float> table((size_t) kTableSize, 0.0f);

    const int numNodes = model.getNumNodes();
    if (numNodes == 0)
        return table; // no nodes -> silence

    const double dt = kTableDomainSeconds / (double) kTableSize;

    // Read all nodes once up front rather than re-walking the ValueTree per sample.
    std::vector<EnvelopeModel::Node> nodes;
    nodes.reserve((size_t) numNodes);
    for (int i = 0; i < numNodes; ++i)
        nodes.push_back(model.getNode(i));

    // Hold at the first node's value for any table time before it.
    const int firstIndex = std::clamp((int) std::ceil(nodes.front().time / dt), 0, kTableSize);
    for (int i = 0; i < firstIndex; ++i)
        table[(size_t) i] = (float) nodes.front().value;

    // Walk each segment, filling the table indices whose time falls within
    // [a.time, b.time). Segments share endpoints, so consecutive ranges abut
    // exactly with no gap or overlap.
    for (int seg = 0; seg + 1 < numNodes; ++seg)
    {
        const auto& a = nodes[(size_t) seg];
        const auto& b = nodes[(size_t) seg + 1];

        const BezierSegment::Point p0 { a.time,      a.value };
        const BezierSegment::Point c1 { a.cpOutTime, a.cpOutValue };
        const BezierSegment::Point c2 { b.cpInTime,  b.cpInValue };
        const BezierSegment::Point p3 { b.time,      b.value };

        const int startIndex = std::clamp((int) std::ceil(a.time / dt), 0, kTableSize);
        const int endIndex   = std::clamp((int) std::ceil(b.time / dt), 0, kTableSize);

        for (int i = startIndex; i < endIndex; ++i)
        {
            const double t = (double) i * dt;
            table[(size_t) i] = (float) BezierSegment::valueAtTime(p0, c1, c2, p3, t);
        }
    }

    // Hold at the last node's value from wherever the final segment left off
    // (covers the tail past the last node) through the end of the table.
    const int lastIndex = std::clamp((int) std::ceil(nodes.back().time / dt), 0, kTableSize);
    for (int i = lastIndex; i < kTableSize; ++i)
        table[(size_t) i] = (float) nodes.back().value;

    return table;
}

} // namespace EnvelopeEvaluator
