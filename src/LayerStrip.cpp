#include "LayerStrip.h"
#include "LayerAudibility.h"

const char* const LayerStrip::typeBoxID     = "type";
const char* const LayerStrip::muteButtonID  = "mute";
const char* const LayerStrip::soloButtonID  = "solo";
const char* const LayerStrip::levelSliderID = "level";

LayerStrip::LayerStrip(juce::AudioProcessorValueTreeState& apvts, int layerIndex)
    : index(layerIndex)
{
    jassert(juce::isPositiveAndBelow(layerIndex, LayerAudibility::kNumLayers));

    indexLabel.setText(juce::String(layerIndex + 1), juce::dontSendNotification);
    indexLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(indexLabel);

    // Filled before the attachment is made: ComboBoxAttachment expects the item IDs to already
    // line up 1..N with the choice parameter's indices, and asserts if they do not.
    typeBox.addItemList(ParamIDs::layerTypeChoices(), 1);
    typeBox.setComponentID(typeBoxID);
    addAndMakeVisible(typeBox);

    // TextButton is momentary until told otherwise; ButtonAttachment drives getToggleState().
    muteButton.setClickingTogglesState(true);
    soloButton.setClickingTogglesState(true);
    muteButton.setComponentID(muteButtonID);
    soloButton.setComponentID(soloButtonID);
    addAndMakeVisible(muteButton);
    addAndMakeVisible(soloButton);

    levelSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    levelSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    levelSlider.setComponentID(levelSliderID);
    addAndMakeVisible(levelSlider);

    using APVTS = juce::AudioProcessorValueTreeState;
    typeAttachment  = std::make_unique<APVTS::ComboBoxAttachment>(apvts, ParamIDs::layerType(layerIndex),  typeBox);
    muteAttachment  = std::make_unique<APVTS::ButtonAttachment>  (apvts, ParamIDs::layerMute(layerIndex),  muteButton);
    soloAttachment  = std::make_unique<APVTS::ButtonAttachment>  (apvts, ParamIDs::layerSolo(layerIndex),  soloButton);
    levelAttachment = std::make_unique<APVTS::SliderAttachment>  (apvts, ParamIDs::layerLevel(layerIndex), levelSlider);

    // A mouse listener rather than each control's onClick/onChange hook: it covers a slider drag
    // and the combo box's popup uniformly, and it cannot be forgotten when a control is added.
    // The `true` includes nested children, which the combo box and slider both have.
    for (auto* child : getChildren())
        child->addMouseListener(this, true);

    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
    {
        auto& cached = flagParams[(size_t) i];
        cached.type = apvts.getRawParameterValue(ParamIDs::layerType(i));
        cached.mute = apvts.getRawParameterValue(ParamIDs::layerMute(i));
        cached.solo = apvts.getRawParameterValue(ParamIDs::layerSolo(i));
        jassert(cached.type != nullptr && cached.mute != nullptr && cached.solo != nullptr);
    }

    // Set before the first poll, so the strip opens showing the right state instead of taking up
    // to a frame to catch up - the same reason the controls are attached before being shown.
    refreshAppearance();

    // Stopped by Timer's destructor when the editor closes, per §0.5's rule that the editor owns
    // its timers. A shared timer across all panels is the eventual direction, also per §0.5.
    startTimerHz(kPollHz);
}

LayerAudibility::LayerFlags LayerStrip::flagsFor(int layerIndex) const
{
    const auto& cached = flagParams[(size_t) layerIndex];

    LayerAudibility::LayerFlags flags;
    flags.isOff = ((int) cached.type->load() == (int) ParamIDs::LayerType::off);
    flags.mute  = (cached.mute->load() > 0.5f);
    flags.solo  = (cached.solo->load() > 0.5f);
    return flags;
}

void LayerStrip::refreshAppearance()
{
    std::array<LayerAudibility::LayerFlags, LayerAudibility::kNumLayers> all;
    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
        all[(size_t) i] = flagsFor(i);

    const auto updated = LayerAudibility::appearanceFor(all[(size_t) index],
                                                         LayerAudibility::anyLayerSoloed(all));

    if (updated.lit == appearance.lit && updated.dimmed == appearance.dimmed)
        return;

    appearance = updated;
    repaint();
}

void LayerStrip::timerCallback()
{
    refreshAppearance();
}

void LayerStrip::setSelected(bool shouldBeSelected)
{
    if (selected == shouldBeSelected)
        return;

    selected = shouldBeSelected;
    repaint();
}

void LayerStrip::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);

    g.setColour(juce::Colours::black.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds, 3.0f);

    // Soloed layers light up (§3.3). Behind the children, so it reads as the cell glowing rather
    // than as a wash over the controls.
    if (appearance.lit)
    {
        g.setColour(juce::Colours::yellow.withAlpha(0.22f));
        g.fillRoundedRectangle(bounds, 3.0f);
    }

    // Selection is a border, not a fill, so it stays legible under the dim wash and cannot be
    // mistaken for the lit state.
    g.setColour(juce::Colours::white.withAlpha(selected ? 1.0f : 0.25f));
    g.drawRoundedRectangle(bounds, 3.0f, selected ? 2.0f : 1.0f);
}

void LayerStrip::paintOverChildren(juce::Graphics& g)
{
    if (! appearance.dimmed)
        return;

    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 3.0f);
}

void LayerStrip::resized()
{
    auto bounds = getLocalBounds().reduced(4);

    auto topRow = bounds.removeFromTop(20);
    indexLabel.setBounds(topRow.removeFromLeft(18));
    typeBox.setBounds(topRow.reduced(2, 0));

    auto flagRow = bounds.removeFromTop(20);
    muteButton.setBounds(flagRow.removeFromLeft(28).reduced(1));
    soloButton.setBounds(flagRow.removeFromLeft(28).reduced(1));

    levelSlider.setBounds(bounds.removeFromTop(20).reduced(1, 0));
}

void LayerStrip::mouseDown(const juce::MouseEvent&)
{
    if (onLayerSelected != nullptr)
        onLayerSelected();
}
