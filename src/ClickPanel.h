#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "ParamIDs.h"
#include "ClickSampleLibrary.h"
#include <memory>

// ClickPanel — the contextual panel for a Click layer (§4.6, §6 Phase 5: "per-click level/tone").
// Shown by the editor in place of the envelope canvas whenever the selected layer's type is Click.
//
// One panel serves all five layers: setLayer() re-points its attachments rather than the editor
// keeping five panels alive, the same way the canvas is re-pointed at a layer's models. Every
// control is APVTS-attached, so host automation and preset loads move it with no polling. The one
// thing an attachment cannot show is whether the chosen sample slot has anything in it, which is
// what refreshStatus() is for.
//
// Position-agnostic per §0.5: it lays itself out within whatever bounds the editor gives it.
class ClickPanel : public juce::Component
{
public:
    // `samples` names the four sample slots in the type list and decides whether a selected slot
    // is empty. Both references must outlive the panel; in practice both belong to the processor.
    ClickPanel(juce::AudioProcessorValueTreeState& apvts, const ClickSampleLibrary& samples);

    // Points every control at layer `layerIndex` (zero-based). A no-op when already pointed there,
    // so the editor can call it on every panel refresh without rebuilding attachments each time.
    void setLayer(int layerIndex);

    // Which layer the controls currently drive, or -1 before the first setLayer().
    int getLayer() const { return layer; }

    // Re-reads the selected click type and updates the status line and the Decay control's enabled
    // state, but only when the type has changed since the last call. Public so the editor's poll can
    // drive it: a type change from automation or a preset load reaches the combo box through its
    // attachment, but nothing else here would notice.
    void refreshStatus();

    // The text the type list shows for a choice index: the parameter's own name for the four
    // synthesised types, and for a sample slot its name plus the file behind it, or "(empty)".
    static juce::String choiceLabel(int choice, const ClickSampleLibrary& samples);

    // Decay is stored in seconds (ParamIDs::clickDecay*) but shown in milliseconds, because a click
    // lives in single-digit milliseconds and "0.008" reads as nothing. Display only - the stored
    // value and its range are untouched.
    static juce::String decayToText(double seconds);
    static double       decayFromText(const juce::String& text);

    // Tone and Level shown as 0-100%, for the same reason: the stored 0-1 value is untouched.
    static juce::String percentToText(double value);
    static double       percentFromText(const juce::String& text);

    // Component IDs, so tests can reach the controls without this class exposing its children.
    static const char* const panelID;
    static const char* const typeBoxID;
    static const char* const toneSliderID;
    static const char* const decaySliderID;
    static const char* const levelSliderID;
    static const char* const statusLabelID;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::AudioProcessorValueTreeState& apvts;
    const ClickSampleLibrary& samples;

    int layer = -1;

    // The selected layer's click.type, resolved in setLayer so refreshStatus does no string lookup.
    std::atomic<float>* clickTypeParam = nullptr;

    // What refreshStatus last acted on, so an unchanged poll does nothing. -1 matches no choice.
    int lastStatusChoice = -1;

    juce::Label title;                         // "Layer N - Click"
    juce::Label typeCaption  { {}, "Type" };
    juce::Label toneCaption  { {}, "Tone" };
    juce::Label decayCaption { {}, "Decay" };
    juce::Label levelCaption { {}, "Level" };

    juce::ComboBox typeBox;                    // the eight click.type choices
    juce::Slider   toneSlider;                 // click.tone, 0 dark to 1 bright
    juce::Slider   decaySlider;                // click.decay, shown in ms
    juce::Slider   levelSlider;                // the layer's level - a click has no second one
    juce::Label    status;                     // what the chosen sample slot will actually play

    // Declared after the controls: each attachment refers to a control above it and must be
    // destroyed first, and C++ destroys members in reverse declaration order.
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   toneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   decayAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   levelAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClickPanel)
};
