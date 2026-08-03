#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParamIDs.h"

// LayerStrip — one cell of the horizontal layer selector (§4.3): index, type, mute/solo, level.
//
// Position-agnostic per §0.5: it draws and lays itself out but has no opinion about where it
// sits. The editor owns five and arranges them.
//
// Every control is APVTS-attached, so host automation and preset loads move the GUI with no
// polling here. The one thing attachments cannot express is §3.3's dimming, which depends on the
// *other* layers' flags rather than this one's parameter - that arrives in 5b.
class LayerStrip : public juce::Component
{
public:
    // layerIndex is zero-based. The parameter IDs it builds are one-based to match the number
    // shown to the user, which ParamIDs handles - see the layerType/layerLevel helpers there.
    LayerStrip(juce::AudioProcessorValueTreeState& apvts, int layerIndex);

    // Fired when anything in this strip is clicked, its controls included, so selecting a layer
    // and operating it are one gesture rather than two. Set by the owner.
    std::function<void()> onLayerSelected;

    // Whether this is the layer whose contextual panel is showing (§4.6). Purely visual; the
    // owner decides selection, since only it knows about the other four strips.
    void setSelected(bool shouldBeSelected);
    bool isSelected() const { return selected; }

    // Which layer this strip drives, zero-based.
    int getLayerIndex() const { return index; }

    // Component IDs on the four controls, so tests can reach them without this class having to
    // expose its children. Kept as constants rather than literals so both sides can't drift.
    static const char* const typeBoxID;
    static const char* const muteButtonID;
    static const char* const soloButtonID;
    static const char* const levelSliderID;

    // Draws the cell background and the selection border.
    void paint(juce::Graphics& g) override;

    // Stacks index/type, then mute/solo, then level within whatever bounds the owner gave.
    void resized() override;

    // Selects this layer. Registered as a mouse listener on the children too, so a click that
    // lands on a control arrives here as well.
    void mouseDown(const juce::MouseEvent& event) override;

private:
    const int index;      // zero-based layer this strip drives
    bool selected = false; // drawn as a brighter, thicker border

    juce::Label      indexLabel;               // the 1-5 the user sees
    juce::ComboBox   typeBox;                  // Off / Sub / Click (§3.2)
    juce::TextButton muteButton { "M" };       // toggle, attached to layerN.mute
    juce::TextButton soloButton { "S" };       // toggle, attached to layerN.solo
    juce::Slider     levelSlider;              // 0-1, attached to layerN.level

    // Declared after the controls deliberately: each attachment holds a reference to the control
    // above it and must be destroyed first, and C++ destroys members in reverse declaration order.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   muteAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   soloAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LayerStrip)
};
