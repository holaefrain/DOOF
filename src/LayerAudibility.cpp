#include "LayerAudibility.h"

namespace LayerAudibility
{

bool anyLayerSoloed(const std::array<LayerFlags, kNumLayers>& layers)
{
    for (const auto& layer : layers)
        if (layer.solo && ! layer.isOff)
            return true;

    return false;
}

bool isAudible(bool isOff, bool mute, bool solo, bool anySoloed)
{
    if (isOff)
        return false;

    // Mute wins over solo unconditionally (§3.3's solo+mute rows).
    if (mute)
        return false;

    // Once anything is soloed, only soloed layers survive.
    if (anySoloed)
        return solo;

    return true;
}

bool isAudible(const LayerFlags& flags, bool anySoloed)
{
    return isAudible(flags.isOff, flags.mute, flags.solo, anySoloed);
}

} // namespace LayerAudibility
