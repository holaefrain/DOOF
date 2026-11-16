#include "SubPanel.h"
#include "LayerAudibility.h"

const char* const SubPanel::panelID     = "subPanel";
const char* const SubPanel::presetBoxID = "subHarmPreset";
const char* const SubPanel::harmGainID  = "subHarmGain";
const char* const SubPanel::harmRatioID = "subHarmRatio";
const char* const SubPanel::harmDecayID = "subHarmDecay";
const char* const SubPanel::sustainID   = "subSustain";
const char* const SubPanel::phaseID     = "subPhase";

SubPanel::SubPanel(juce::AudioProcessorValueTreeState& state)
    : apvts(state)
{
    setComponentID(panelID);

    // Labels default to black text, invisible on the editor's dark background.
    for (auto* label : { &title, &presetCaption, &gainCaption, &ratioCaption, &decayCaption, &phaseCaption })
    {
        label->setColour(juce::Label::textColourId, juce::Colours::white);
        label->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*label);
    }
    title.setJustificationType(juce::Justification::centredLeft);

    // Filled before any attachment exists, so item IDs 1..N line up with the choice indices.
    presetBox.addItemList(ParamIDs::harmPresetChoices(), 1);
    presetBox.setComponentID(presetBoxID);
    addAndMakeVisible(presetBox);

    for (auto* slider : { &harmGain, &harmRatio, &harmDecay, &phase })
    {
        slider->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 64, 16);
        addAndMakeVisible(*slider);
    }
    harmGain.setComponentID(harmGainID);
    harmRatio.setComponentID(harmRatioID);
    harmDecay.setComponentID(harmDecayID);
    phase.setComponentID(phaseID);

    sustainButton.setComponentID(sustainID);
    sustainButton.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible(sustainButton);
}

juce::String SubPanel::percentToText(double value) { return juce::String(juce::roundToInt(value * 100.0)) + "%"; }
double SubPanel::percentFromText(const juce::String& text) { return text.trim().getDoubleValue() / 100.0; }

juce::String SubPanel::timeToText(double seconds)
{
    if (seconds >= 1.0)
        return juce::String(seconds, 2) + " s";

    return juce::String(juce::roundToInt(seconds * 1000.0)) + " ms";
}

double SubPanel::timeFromText(const juce::String& text)
{
    const auto trimmed = text.trim().toLowerCase();
    const double number = trimmed.getDoubleValue();

    // Seconds only when the text says so and does not say ms; a bare number is milliseconds,
    // matching how the control reads.
    if (trimmed.endsWith("s") && ! trimmed.endsWith("ms"))
        return number;

    return number / 1000.0;
}

juce::String SubPanel::degreesToText(double degrees) { return juce::String(juce::roundToInt(degrees)) + " deg"; }
double SubPanel::degreesFromText(const juce::String& text) { return text.trim().getDoubleValue(); }

void SubPanel::setLayer(int layerIndex)
{
    jassert(juce::isPositiveAndBelow(layerIndex, LayerAudibility::kNumLayers));

    if (layerIndex == layer)
        return;

    layer = layerIndex;

    // Old attachments first, so the outgoing layer can never be written through a stale one.
    presetAttachment.reset();
    gainAttachment.reset();
    ratioAttachment.reset();
    decayAttachment.reset();
    phaseAttachment.reset();
    sustainAttachment.reset();

    using APVTS = juce::AudioProcessorValueTreeState;
    presetAttachment  = std::make_unique<APVTS::ComboBoxAttachment>(apvts, ParamIDs::layerSubHarmPreset(layer), presetBox);
    gainAttachment    = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerSubHarmGain(layer),   harmGain);
    ratioAttachment   = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerSubHarmRatio(layer),  harmRatio);
    decayAttachment   = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerSubHarmDecay(layer),  harmDecay);
    phaseAttachment   = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerSubPhase(layer),      phase);
    sustainAttachment = std::make_unique<APVTS::ButtonAttachment>  (apvts, ParamIDs::layerSubSustain(layer),    sustainButton);

    // After the attachments, which install the parameters' own text conversion.
    auto display = [](juce::Slider& slider, auto toText, auto fromText)
    {
        slider.textFromValueFunction = toText;
        slider.valueFromTextFunction = fromText;
        slider.updateText();
    };
    display(harmGain,  percentToText, percentFromText);
    display(harmRatio, percentToText, percentFromText);
    display(harmDecay, timeToText,    timeFromText);
    display(phase,     degreesToText, degreesFromText);

    title.setText("Layer " + juce::String(layer + 1) + " - Sub", juce::dontSendNotification);
}

void SubPanel::paint(juce::Graphics& g)
{
    // Opaque, like ClickPanel: the editor paints its name behind everything.
    g.fillAll(juce::Colour(0xff1a1a1a));
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRoundedRectangle(getLocalBounds().reduced(4).toFloat(), 6.0f);
}

// One row: the title, the harmonics group (preset over its three knobs), then Sustain and Phase.
void SubPanel::resized()
{
    auto bounds = getLocalBounds().reduced(8);

    title.setBounds(bounds.removeFromLeft(110));

    const int knob = juce::jmin(64, bounds.getHeight() - 18);

    auto column = [&](juce::Label& caption, juce::Component& control, int width)
    {
        auto area = bounds.removeFromLeft(width);
        caption.setBounds(area.removeFromTop(18));
        control.setBounds(area.withSizeKeepingCentre(width, juce::jmin(area.getHeight(), knob + 18)));
        bounds.removeFromLeft(8);
    };

    auto presetArea = bounds.removeFromLeft(110);
    presetCaption.setBounds(presetArea.removeFromTop(18));
    presetBox.setBounds(presetArea.withSizeKeepingCentre(presetArea.getWidth(), 24));
    bounds.removeFromLeft(8);

    column(gainCaption,  harmGain,  72);
    column(ratioCaption, harmRatio, 72);
    column(decayCaption, harmDecay, 72);

    bounds.removeFromLeft(16);
    sustainButton.setBounds(bounds.removeFromLeft(96).withSizeKeepingCentre(96, 24));
    bounds.removeFromLeft(8);

    column(phaseCaption, phase, 72);
}
