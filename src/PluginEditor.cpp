#include "PluginEditor.h"
#include "EnvelopeEvaluator.h"

// Initialises the editor, stores the processor reference, and sets the default window size.
// 800x600 is the flat developer skin starting size; resized to final dimensions in Phase 12.
// Pitch range gives headroom above the default 150 Hz start; amp is always
// [0,1]. The initial view duration matches whichever model's Length (§Step 5)
// is currently longer, so the whole work area is visible on load without
// needing to zoom/pan first; the user can zoom/pan from there (Step 4).
DOOFAudioProcessorEditor::DOOFAudioProcessorEditor(DOOFAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p),
      envelopeCanvas(p.getPitchEnvelopeModel(), juce::Range<double>(20.0, 220.0),
                     p.getAmpEnvelopeModel(),   juce::Range<double>(0.0, 1.0),
                     juce::jmax(p.getPitchEnvelopeModel().getLength(), p.getAmpEnvelopeModel().getLength()), p)
{
    pitchLogToggle.onClick = [this]
    {
        envelopeCanvas.setPitchLogScale(pitchLogToggle.getToggleState());
    };
    undoButton.onClick = [this] { performUndo(); };
    redoButton.onClick = [this] { performRedo(); };

    editingCurveBox.addItem("Pitch", 1);
    editingCurveBox.addItem("Amp",   2);
    editingCurveBox.setSelectedId(1, juce::dontSendNotification);
    editingCurveBox.onChange = [this]
    {
        envelopeCanvas.setActiveCurve(editingCurveBox.getSelectedId() == 1
                                           ? EnvelopeCanvas::ActiveCurve::pitch
                                           : EnvelopeCanvas::ActiveCurve::amp);
    };

    // Matches the range EnvelopeCanvas's own Length-handle drag enforces
    // (kMinLength there, EnvelopeEvaluator::kTableDomainSeconds as the
    // ceiling — there's nothing valid beyond the evaluator's table domain).
    static constexpr double kMinLength = 0.01;

    // Labels default to black text, invisible against the dark editor
    // background (unlike TextButton/ComboBox, which use a lighter default
    // under this LookAndFeel) — set explicit colours for all four.
    for (auto* label : { &pitchLengthCaption, &pitchLengthValue, &ampLengthCaption, &ampLengthValue })
        label->setColour(juce::Label::textColourId, juce::Colours::white);
    pitchLengthValue.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.3f));
    ampLengthValue.setColour(juce::Label::outlineColourId, juce::Colours::white.withAlpha(0.3f));

    pitchLengthValue.setEditable(true);
    pitchLengthValue.onTextChange = [this]
    {
        const double clamped = juce::jlimit(kMinLength, EnvelopeEvaluator::kTableDomainSeconds,
                                             pitchLengthValue.getText().getDoubleValue());
        audioProcessor.getPitchEnvelopeModel().setLength(clamped);
        refreshLengthLabels();
        envelopeCanvas.repaint();
    };
    ampLengthValue.setEditable(true);
    ampLengthValue.onTextChange = [this]
    {
        const double clamped = juce::jlimit(kMinLength, EnvelopeEvaluator::kTableDomainSeconds,
                                             ampLengthValue.getText().getDoubleValue());
        audioProcessor.getAmpEnvelopeModel().setLength(clamped);
        refreshLengthLabels();
        envelopeCanvas.repaint();
    };
    envelopeCanvas.onLengthChanged = [this] { refreshLengthLabels(); };
    refreshLengthLabels();

    // Children must be added before setSize(), which triggers the initial
    // resized() layout pass — a lesson from Phase 3 Step 0's spike.
    addAndMakeVisible(envelopeCanvas);
    addAndMakeVisible(pitchLogToggle);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addAndMakeVisible(editingCurveBox);
    addAndMakeVisible(pitchLengthCaption);
    addAndMakeVisible(pitchLengthValue);
    addAndMakeVisible(ampLengthCaption);
    addAndMakeVisible(ampLengthValue);
    setSize(800, 600);

    // Undo/redo shortcuts need somewhere to land initially; a click on
    // envelopeCanvas later re-targets focus there, but unhandled key presses
    // bubble back up to this component regardless (JUCE's default focus
    // traversal), so keyPressed() below still fires either way.
    setWantsKeyboardFocus(true);
    grabKeyboardFocus();
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
    auto bounds = getLocalBounds();

    auto topBar = bounds.removeFromTop(24);
    pitchLogToggle.setBounds(topBar.removeFromLeft(160).reduced(2));
    undoButton.setBounds(topBar.removeFromLeft(60).reduced(2));
    redoButton.setBounds(topBar.removeFromLeft(60).reduced(2));
    editingCurveBox.setBounds(topBar.removeFromLeft(100).reduced(2));

    auto secondBar = bounds.removeFromTop(24);
    pitchLengthCaption.setBounds(secondBar.removeFromLeft(90).reduced(2));
    pitchLengthValue.setBounds(secondBar.removeFromLeft(60).reduced(2));
    ampLengthCaption.setBounds(secondBar.removeFromLeft(90).reduced(2));
    ampLengthValue.setBounds(secondBar.removeFromLeft(60).reduced(2));

    envelopeCanvas.setBounds(bounds);
}

bool DOOFAudioProcessorEditor::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier, 0))
    {
        performUndo();
        return true;
    }

    if (key == juce::KeyPress('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        performRedo();
        return true;
    }

    return false;
}

void DOOFAudioProcessorEditor::performUndo()
{
    audioProcessor.getPitchEnvelopeModel().undo();
    envelopeCanvas.repaint();
}

void DOOFAudioProcessorEditor::performRedo()
{
    audioProcessor.getPitchEnvelopeModel().redo();
    envelopeCanvas.repaint();
}

void DOOFAudioProcessorEditor::refreshLengthLabels()
{
    pitchLengthValue.setText(juce::String(audioProcessor.getPitchEnvelopeModel().getLength(), 3),
                              juce::dontSendNotification);
    ampLengthValue.setText(juce::String(audioProcessor.getAmpEnvelopeModel().getLength(), 3),
                            juce::dontSendNotification);
}
