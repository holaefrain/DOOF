#pragma once
#include "PluginProcessor.h"
#include "EnvelopeCanvas.h"
#include "LayerStrip.h"
#include <array>
#include <memory>

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

    // Marks one layer as selected and clears the other four, so exactly one is ever selected.
    // The selection currently only changes which strip is outlined; retargeting the canvas and
    // the contextual panel to that layer is Step 6.
    void setSelectedLayer(int layerIndex);

    // Back-reference to the processor; used to access parameters and engine state.
    // Guaranteed valid for the lifetime of this editor (processor outlives editor).
    DOOFAudioProcessor& audioProcessor;

    // The horizontal layer selector (§4.3), one cell per layer. Held by unique_ptr because
    // LayerStrip needs the APVTS at construction and so is not default-constructible.
    std::array<std::unique_ptr<LayerStrip>, LayerAudibility::kNumLayers> layerStrips;

    // Which layer the strip currently has selected. Layer 1 to start, matching the default patch
    // where it is the only audible layer.
    int selectedLayer = 0;

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

    // Minimal preset save/load (§Step 7b): writes/reads the exact same state
    // getStateInformation/setStateInformation use for host project save, to
    // a standalone .doof file.
    juce::TextButton savePresetButton { "Save..." };
    juce::TextButton loadPresetButton { "Load..." };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessorEditor)
};
