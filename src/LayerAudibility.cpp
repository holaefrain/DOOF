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

Appearance appearanceFor(const LayerFlags& flags, bool anySoloed)
{
    Appearance appearance;

    // Off excluded for the same reason anyLayerSoloed() excludes it: a soloed Off layer does not
    // put the mix into solo mode, so lighting it up would advertise an effect it is not having.
    appearance.lit = flags.solo && ! flags.isOff;

    // Silent now, but audible in a world where nothing is soloed - which is precisely "silenced
    // because of someone else's solo". Written as a difference of isAudible() rather than as its
    // own condition so it cannot disagree with the mixer about what silences a layer.
    appearance.dimmed = ! isAudible(flags, anySoloed) && isAudible(flags, false);

    return appearance;
}

} // namespace LayerAudibility
