#pragma once
#include "PluginProcessor.h"
#include "EnvelopeCanvas.h"

// DOOFAudioProcessorEditor — the plugin's GUI root window.
// Owns all visible panels and controls. Created and destroyed by the host
// independently of the processor; the processor must never store a pointer
// to this editor (it may be null at any time).
class DOOFAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    // Constructs the editor and sets the initial window size.
    // Takes a reference to the processor so controls can read/write parameters.
    explicit DOOFAudioProcessorEditor(DOOFAudioProcessor&);

    // Detaches all listeners and timers registered in the constructor.
    ~DOOFAudioProcessorEditor() override;

    // Redraws the editor's background and any static graphics each frame.
    void paint(juce::Graphics&) override;

    // Called whenever the window is resized; repositions all child components.
    void resized() override;

    // Cmd+Z / Cmd+Shift+Z (Ctrl on Windows): undo/redo the shared envelope
    // undo history (§Step 1's shared UndoManager).
    bool keyPressed(const juce::KeyPress&) override;

private:
    // Shared by keyPressed and the undo/redo buttons. Either model's
    // undo()/redo() reaches the same shared UndoManager (§Step 1), so which
    // one is called through doesn't matter.
    void performUndo();
    void performRedo();

    // Re-reads both models' Length into the numeric fields below. Called
    // after the canvas's Length handle is dragged, and whenever a field's
    // own edit is committed (to reflect any clamping the model applied).
    void refreshLengthLabels();

    // Back-reference to the processor; used to access parameters and engine state.
    // Guaranteed valid for the lifetime of this editor (processor outlives editor).
    DOOFAudioProcessor& audioProcessor;

    // The central canvas (§3.4): pitch + amp curves overlaid on one time axis.
    // Declared after audioProcessor since its constructor reads model
    // references out of it.
    EnvelopeCanvas envelopeCanvas;

    // Log/Linear vertical-axis toggle for the pitch curve (§3.4).
    juce::ToggleButton pitchLogToggle { "Log Scale (Pitch)" };

    // On-screen undo/redo, mirroring the Cmd+Z / Cmd+Shift+Z shortcuts.
    juce::TextButton undoButton { "Undo" };
    juce::TextButton redoButton { "Redo" };

    // Which curve double-click-to-add-a-node targets (§Step 3, 3b).
    juce::ComboBox editingCurveBox;

    // Numeric Length fields (§Step 5), alongside the canvas's draggable
    // Length handles — both edit the same underlying EnvelopeModel::setLength().
    juce::Label pitchLengthCaption { {}, "Pitch Len (s)" };
    juce::Label pitchLengthValue;
    juce::Label ampLengthCaption   { {}, "Amp Len (s)" };
    juce::Label ampLengthValue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessorEditor)
};
