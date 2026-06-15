#include "PluginEditor.h"

DOOFAudioProcessorEditor::DOOFAudioProcessorEditor(DOOFAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(800, 600);
}

DOOFAudioProcessorEditor::~DOOFAudioProcessorEditor() = default;

void DOOFAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a1a));
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(40.0f)));
    g.drawFittedText("DOOF", getLocalBounds(), juce::Justification::centred, 1);
}

void DOOFAudioProcessorEditor::resized() {}
