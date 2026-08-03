#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParamIDs.h"
#include "LayerAudibility.h"
#include <array>

// LayerStrip — one cell of the horizontal layer selector (§4.3): index, type, mute/solo, level.
//
// Position-agnostic per §0.5: it draws and lays itself out but has no opinion about where it
// sits. The editor owns five and arranges them.
//
// Every control is APVTS-attached, so host automation and preset loads move the GUI with no
// polling here. The one thing attachments cannot express is §3.3's dimming, which depends on the
// *other* layers' flags rather than this one's parameter - that arrives in 5b.
class LayerStrip : public juce::Component,
                   private juce::Timer
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

    // Re-reads every layer's flags and updates this strip's lit/dimmed appearance, repainting
    // only if it actually changed. Called by the poll timer; public so a test can step it without
    // waiting on the message loop, and so an owner could drive all five from one timer later.
    void refreshAppearance();

    // Current appearance, per §3.3. Reflects the last refreshAppearance(), not a live read.
    bool isSoloLit() const { return appearance.lit; }
    bool isDimmed()  const { return appearance.dimmed; }

    // Draws the cell background, the solo highlight, and the selection border.
    void paint(juce::Graphics& g) override;

    // The dim wash, drawn after the children so the controls dim with the cell. paint() runs
    // before them and would leave the combo and buttons at full brightness.
    void paintOverChildren(juce::Graphics& g) override;

    // Stacks index/type, then mute/solo, then level within whatever bounds the owner gave.
    void resized() override;

    // Selects this layer. Registered as a mouse listener on the children too, so a click that
    // lands on a control arrives here as well.
    void mouseDown(const juce::MouseEvent& event) override;

private:
    // Polls the mixer flags. §3.3's dimming depends on the *other* layers, which no APVTS
    // attachment can express, and host automation moves solo without any click here to react to -
    // so a poll is the mechanism, at the ~30 fps §0.5 asks for. It repaints only on a real change,
    // so a still patch costs nothing beyond the comparison.
    void timerCallback() override;

    // Reads one layer's flags from the cached pointers below.
    LayerAudibility::LayerFlags flagsFor(int layerIndex) const;

    static constexpr int kPollHz = 30;

    const int index;      // zero-based layer this strip drives
    bool selected = false; // drawn as a brighter, thicker border

    // Last computed appearance, compared against each poll to decide whether to repaint.
    LayerAudibility::Appearance appearance;

    // Every layer's flag parameters, not just this strip's: whether this layer dims depends on
    // whether any *other* layer is soloed. Resolved once, since getRawParameterValue hashes a
    // string and the pointers it returns stay valid for the processor's lifetime.
    struct FlagParams
    {
        std::atomic<float>* type = nullptr;
        std::atomic<float>* mute = nullptr;
        std::atomic<float>* solo = nullptr;
    };
    std::array<FlagParams, LayerAudibility::kNumLayers> flagParams;

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
