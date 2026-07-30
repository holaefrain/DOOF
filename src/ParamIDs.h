#pragma once
#include <juce_core/juce_core.h>

// ParamIDs — the stable APVTS parameter ID strings, in one place.
//
// Shared between PluginProcessor (which declares the parameters) and the GUI
// (which attaches controls to them by ID). Kept in a header rather than in
// PluginProcessor.cpp specifically so the layer strip can't build these
// strings itself: a second copy of the format would be free to drift, and
// per §2 of project-architecture.md an ID that any preset has been saved
// against can never be renamed or removed.
namespace ParamIDs
{
    // Master output gain, applied after the layer mix. Predates the mixer,
    // when it was the single Sub layer's own gain — hence the name, which is
    // now historical but frozen by the never-rename rule above.
    static const juce::String subGain = "sub.gain";

    // Choice indices for layerN.type. These integers ARE the parameter's
    // stored choice indices, so this order is part of the preset contract and
    // must only ever be appended to.
    enum class LayerType { off = 0, sub = 1, click = 2 };

    // Display names for the choices above; index-aligned with LayerType.
    inline juce::StringArray layerTypeChoices() { return { "Off", "Sub", "Click" }; }

    // Per-layer parameter IDs. layerIndex is the zero-based internal index,
    // but the ID it produces is one-based ("layer1.level" for index 0) to
    // match the layer numbering shown in the GUI.
    inline juce::String layerType  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".type";  }
    inline juce::String layerLevel (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".level"; }
    inline juce::String layerMute  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".mute";  }
    inline juce::String layerSolo  (int layerIndex) { return "layer" + juce::String(layerIndex + 1) + ".solo";  }
}
