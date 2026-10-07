#include "ClickPanel.h"
#include "LayerAudibility.h"

const char* const ClickPanel::panelID       = "clickPanel";
const char* const ClickPanel::typeBoxID     = "clickType";
const char* const ClickPanel::toneSliderID  = "clickTone";
const char* const ClickPanel::decaySliderID = "clickDecay";
const char* const ClickPanel::levelSliderID = "clickLevel";
const char* const ClickPanel::statusLabelID = "clickStatus";

ClickPanel::ClickPanel(juce::AudioProcessorValueTreeState& state, const ClickSampleLibrary& library)
    : apvts(state), samples(library)
{
    setComponentID(panelID);

    // Labels default to black text, invisible on the editor's dark background - the same fix the
    // editor applies to its own Length captions.
    for (auto* label : { &title, &typeCaption, &toneCaption, &decayCaption, &levelCaption })
    {
        label->setColour(juce::Label::textColourId, juce::Colours::white);
        label->setJustificationType(juce::Justification::centred);
        addAndMakeVisible(*label);
    }
    title.setFont(juce::Font(juce::FontOptions(20.0f)));

    // Filled before any attachment exists: ComboBoxAttachment expects item IDs 1..N to line up with
    // the choice indices already. The slot names are fixed for the plugin's lifetime - the library
    // decodes once, at construction - so the list is built once rather than on every setLayer().
    for (int choice = 0; choice < ParamIDs::clickTypeChoices().size(); ++choice)
        typeBox.addItem(choiceLabel(choice, samples), choice + 1);

    typeBox.setComponentID(typeBoxID);

    // Also called on any change the user makes, so the status line answers on the same gesture
    // rather than a poll later. Automation and preset loads still arrive through the poll.
    typeBox.onChange = [this] { refreshStatus(); };
    addAndMakeVisible(typeBox);

    for (auto* slider : { &toneSlider, &decaySlider, &levelSlider })
    {
        slider->setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 80, 20);
        addAndMakeVisible(*slider);
    }
    toneSlider.setComponentID(toneSliderID);
    decaySlider.setComponentID(decaySliderID);
    levelSlider.setComponentID(levelSliderID);

    status.setComponentID(statusLabelID);
    status.setJustificationType(juce::Justification::centred);
    status.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(status);
}

juce::String ClickPanel::choiceLabel(int choice, const ClickSampleLibrary& library)
{
    const auto names = ParamIDs::clickTypeChoices();
    const auto base = names[choice];

    const int slot = choice - ParamIDs::firstClickSampleSlot;
    if (! juce::isPositiveAndBelow(slot, ParamIDs::numClickSampleSlots))
        return base;

    // The file's name is only ever shown, never stored - a preset keeps the slot index, which is
    // why the choice's own "Sample N" stays first: it is the part that means something to a preset.
    const auto file = library.nameOf(slot);
    return file.isNotEmpty() ? base + " - " + file : base + " (empty)";
}

juce::String ClickPanel::percentToText(double value)
{
    return juce::String(juce::roundToInt(value * 100.0)) + "%";
}

double ClickPanel::percentFromText(const juce::String& text)
{
    return text.trim().getDoubleValue() / 100.0;
}

juce::String ClickPanel::decayToText(double seconds)
{
    return juce::String(seconds * 1000.0, 1) + " ms";
}

double ClickPanel::decayFromText(const juce::String& text)
{
    // getDoubleValue reads the leading number and ignores a trailing "ms", so "12", "12ms" and
    // "12 ms" all mean twelve milliseconds. The slider clamps the result to the parameter's range.
    return text.trim().getDoubleValue() / 1000.0;
}

void ClickPanel::setLayer(int layerIndex)
{
    jassert(juce::isPositiveAndBelow(layerIndex, LayerAudibility::kNumLayers));

    if (layerIndex == layer)
        return;

    layer = layerIndex;

    // Old attachments go first. Two attachments on one control would each drive it, and the
    // outgoing one would write this layer's values into the previous layer's parameters.
    typeAttachment.reset();
    toneAttachment.reset();
    decayAttachment.reset();
    levelAttachment.reset();

    using APVTS = juce::AudioProcessorValueTreeState;
    typeAttachment  = std::make_unique<APVTS::ComboBoxAttachment>(apvts, ParamIDs::layerClickType(layer),  typeBox);
    toneAttachment  = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerClickTone(layer),  toneSlider);
    decayAttachment = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerClickDecay(layer), decaySlider);
    levelAttachment = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerLevel(layer),      levelSlider);

    // After the attachment, which installs the parameter's own text conversion and would overwrite
    // anything set before it.
    decaySlider.textFromValueFunction = [](double value) { return decayToText(value); };
    decaySlider.valueFromTextFunction = [](const juce::String& text) { return decayFromText(text); };
    decaySlider.updateText();

    // Tone and Level are both 0-1 with no natural unit, and the parameter's own text prints them
    // to seven decimal places. A percentage is what a knob's position actually communicates.
    for (auto* slider : { &toneSlider, &levelSlider })
    {
        slider->textFromValueFunction = [](double value) { return percentToText(value); };
        slider->valueFromTextFunction = [](const juce::String& text) { return percentFromText(text); };
        slider->updateText();
    }

    title.setText("Layer " + juce::String(layer + 1) + " - Click", juce::dontSendNotification);

    clickTypeParam = apvts.getRawParameterValue(ParamIDs::layerClickType(layer));
    jassert(clickTypeParam != nullptr);

    // Forced, since the new layer can share the old one's choice while the status is still correct
    // only by coincidence - and the very first setLayer has nothing to compare against.
    lastStatusChoice = -1;
    refreshStatus();
}

void ClickPanel::refreshStatus()
{
    if (clickTypeParam == nullptr)
        return;

    const int choice = (int) clickTypeParam->load();
    if (choice == lastStatusChoice)
        return;

    lastStatusChoice = choice;

    const int slot = choice - ParamIDs::firstClickSampleSlot;
    const bool isSampleSlot = juce::isPositiveAndBelow(slot, ParamIDs::numClickSampleSlots);

    // A sample plays to its own end (see ClickVoice::Generator::renderSample), so Decay does nothing
    // to it. Greyed out rather than hidden, so the layout does not jump as the type changes.
    decaySlider.setEnabled(! isSampleSlot);

    if (! isSampleSlot)
        status.setText({}, juce::dontSendNotification);
    else if (samples.slots()[slot].isLoaded())
        status.setText("Plays " + samples.nameOf(slot) + " to its own end - Decay does not apply "
                       "to samples",
                       juce::dontSendNotification);
    else
        // Says so outright, because otherwise a silent Click layer looks like a bug. An empty slot
        // is silent by design (Phase5ClickRoutingTest (c)), never a fallback to another sound.
        status.setText("Sample slot " + juce::String(slot + 1) + " is empty, so this layer is silent",
                       juce::dontSendNotification);
}

void ClickPanel::paint(juce::Graphics& g)
{
    // Opaque first: the editor paints its name across the middle of the window, and a translucent
    // panel lets it show through between the knobs.
    g.fillAll(juce::Colour(0xff1a1a1a));
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    g.fillRoundedRectangle(getLocalBounds().reduced(8).toFloat(), 6.0f);
}

// Title across the top, the type list under it, then the three knobs side by side and the status
// line beneath. Sized from the bounds given rather than fixed, per §0.5.
void ClickPanel::resized()
{
    auto bounds = getLocalBounds().reduced(24);

    title.setBounds(bounds.removeFromTop(32));
    bounds.removeFromTop(8);

    auto typeRow = bounds.removeFromTop(28);
    const int typeWidth = juce::jmin(320, typeRow.getWidth());
    auto typeArea = typeRow.withSizeKeepingCentre(typeWidth, typeRow.getHeight());
    typeCaption.setBounds(typeArea.removeFromLeft(60));
    typeBox.setBounds(typeArea);

    status.setBounds(bounds.removeFromBottom(28));
    bounds.removeFromTop(16);

    const int knobSize = juce::jmin(140, bounds.getWidth() / 3, bounds.getHeight() - 24);
    auto knobRow = bounds.withSizeKeepingCentre(knobSize * 3 + 2 * 24, knobSize + 24);

    auto place = [&](juce::Label& caption, juce::Slider& slider)
    {
        auto column = knobRow.removeFromLeft(knobSize);
        caption.setBounds(column.removeFromTop(24));
        slider.setBounds(column);
        knobRow.removeFromLeft(24);
    };

    place(toneCaption,  toneSlider);
    place(decayCaption, decaySlider);
    place(levelCaption, levelSlider);
}
