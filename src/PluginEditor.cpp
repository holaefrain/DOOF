#include "PluginEditor.h"

// Initialises the editor, stores the processor reference, and sets the default window size.
// 800x600 is the flat developer skin starting size; resized to final dimensions in Phase 12.
// Pitch range gives headroom above the default 150 Hz start; amp is always
// [0,1]. visibleSeconds = 0.5 shows the whole default envelope shape (pitch
// sweep ends at 300 ms, amp tail is near-silent by 400 ms) with a little
// margin — Step 5's Length control will make this adjustable.
DOOFAudioProcessorEditor::DOOFAudioProcessorEditor(DOOFAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p),
      envelopeCanvas(p.getPitchEnvelopeModel(), juce::Range<double>(20.0, 220.0),
                     p.getAmpEnvelopeModel(),   juce::Range<double>(0.0, 1.0),
                     0.5)
{
    // Children must be added before setSize(), which triggers the initial
    // resized() layout pass — a lesson from Phase 3 Step 0's spike.
    addAndMakeVisible(envelopeCanvas);
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
// The canvas fills the whole window for now; layout logic will divide this
// up as more panels are added in later phases.
void DOOFAudioProcessorEditor::resized()
{
    envelopeCanvas.setBounds(getLocalBounds());
}
