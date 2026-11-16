#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParamIDs.h"
#include <memory>

// SubPanel — the Sub layer's parameter strip (§4.6's contextual panel, Phase 6): harmonics
// (preset, Gain, Ratio, Decay), Sustain, and Phase. Shown under the envelope canvas whenever the
// selected layer's type is Sub; the canvas above it is where the curves, keytracking and the
// phase lock are edited.
//
// Built like ClickPanel: one panel serves all five layers, setLayer() re-points its APVTS
// attachments, and every control follows host automation and preset loads through them with no
// polling. Position-agnostic per §0.5.
class SubPanel : public juce::Component
{
public:
    explicit SubPanel(juce::AudioProcessorValueTreeState& apvts);

    // Points every control at layer `layerIndex` (zero-based). A no-op when already pointed there.
    void setLayer(int layerIndex);

    // Which layer the controls currently drive, or -1 before the first setLayer().
    int getLayer() const { return layer; }

    // Display text, kept static so tests can check it without a live slider. Stored values are
    // untouched: gain and ratio are 0-1, decay is seconds, phase is degrees.
    static juce::String percentToText(double value);
    static double       percentFromText(const juce::String& text);
    static juce::String timeToText(double seconds);      // "40 ms", or "1.25 s" from a second up
    static double       timeFromText(const juce::String& text); // milliseconds unless it says "s"
    static juce::String degreesToText(double degrees);   // "90 deg" - ASCII, see the Phase 4 log
    static double       degreesFromText(const juce::String& text);

    // Component IDs, so tests can reach the controls without this class exposing its children.
    static const char* const panelID;
    static const char* const presetBoxID;
    static const char* const harmGainID;
    static const char* const harmRatioID;
    static const char* const harmDecayID;
    static const char* const sustainID;
    static const char* const phaseID;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    int layer = -1;

    juce::Label title;
    juce::Label presetCaption { {}, "Harmonics" };
    juce::Label gainCaption   { {}, "Gain" };
    juce::Label ratioCaption  { {}, "Ratio" };
    juce::Label decayCaption  { {}, "Decay" };
    juce::Label phaseCaption  { {}, "Phase" };

    juce::ComboBox     presetBox;
    juce::Slider       harmGain, harmRatio, harmDecay, phase;
    juce::ToggleButton sustainButton { "Sustain" };

    // After the controls, so each attachment is destroyed before the control it refers to.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> presetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   ratioAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   decayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   phaseAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   sustainAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubPanel)
};
