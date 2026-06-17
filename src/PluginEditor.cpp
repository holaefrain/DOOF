#include "PluginEditor.h"

// Initialises the editor, stores the processor reference, and sets the default window size.
// 800x600 is the flat developer skin starting size; resized to final dimensions in Phase 12.
DOOFAudioProcessorEditor::DOOFAudioProcessorEditor(DOOFAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p)
{
    setSize(800, 600);
}

DOOFAudioProcessorEditor::~DOOFAudioProcessorEditor() = default;

// Paints the editor background and a centred plugin name label.
// Dark background (0x1a1a1a) matches the flat developer skin colour palette.
// Replaced by the full UI layout as pages and panels are added each phase.
void DOOFAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a1a));
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(juce::FontOptions(40.0f)));
    g.drawFittedText("DOOF", getLocalBounds(), juce::Justification::centred, 1);
}

// Positions all child components when the window is resized.
// Phase 0 has no child components; layout logic added as panels are introduced.
void DOOFAudioProcessorEditor::resized() {}
