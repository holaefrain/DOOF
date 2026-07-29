#include "DefaultEnvelopes.h"

namespace DefaultEnvelopes
{

void seedPitch(EnvelopeModel& model)
{
    model.addNode(0.0, 150.0);
    model.addNode(0.3,  50.0);
}

void seedAmp(EnvelopeModel& model)
{
    model.addNode(0.0,   0.0);
    model.addNode(0.002, 1.0);
    model.addNode(0.4,   0.00001);
}

} // namespace DefaultEnvelopes
