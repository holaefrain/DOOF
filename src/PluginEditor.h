#pragma once
#include "PluginProcessor.h"
#include "EnvelopeCanvas.h"
#include "LayerStrip.h"
#include "ClickPanel.h"
#include "SubPanel.h"
#include "FXPage.h"
#include <array>
#include <memory>

// DOOFAudioProcessorEditor — the plugin's GUI root window.
// Owns all visible panels and controls. Created and destroyed by the host
// independently of the processor; the processor must never store a pointer
// to this editor (it may be null at any time).
class DOOFAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::Timer
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

    // Component ID on the placeholder below, so a test can find it without this class exposing
    // its children. Same approach as LayerStrip's control IDs.
    static const char* const placeholderID;

    // Component IDs on the global tuning controls, for tests (Phase 6).
    static const char* const tuneSliderID;
    static const char* const keyRootSliderID;

    // The page tabs (§4: Edit · FX · EQ · Limiter · Settings - the first two exist from Phase 7).
    enum class Page { edit, fx };
    void showPage(Page page);
    Page getPage() const { return currentPage; }
    static const char* const editTabID;
    static const char* const fxTabID;

private:
    // Shared by keyPressed and the undo/redo buttons. Either model's
    // undo()/redo() reaches the same shared UndoManager (§Step 1), so which
    // one is called through doesn't matter.
    void performUndo();
    void performRedo();

    // Pushes the selected layer's stored view preferences into the log toggle, the editing-curve
    // selector and the canvas. Deliberately notifies nothing: the widgets' own callbacks write
    // preferences *back*, so letting them fire here would have the layer being switched to
    // immediately overwrite itself with whatever the widgets happened to be showing.
    void applyViewPrefsToUI();

    // Item IDs for editingCurveBox. Named because the mapping to LayerViewPrefs::editingPitch is
    // now read in two directions, and a bare 1/2 at each site is easy to get backwards.
    static constexpr int kEditingPitchId = 1;
    static constexpr int kEditingAmpId   = 2;

    // Re-reads both models' Length into the numeric fields below. Called
    // after the canvas's Length handle is dragged, and whenever a field's
    // own edit is committed (to reflect any clamping the model applied).
    void refreshLengthLabels();

    // Shows or hides the canvas and its toolbar according to the selected layer's type (§4.6:
    // the contextual panel swaps with the layer type). Only touches the components when the
    // answer has actually changed, so it is cheap to call from the poll below.
    void refreshContextualPanel();

    // Polls the selected layer's type. A poll rather than a callback because the type can change
    // without any click here - host automation and preset loads both move it - and the strip's
    // combo box is not the only route to it. Same ~30 fps as LayerStrip, per §0.5.
    void timerCallback() override;
    static constexpr int kPollHz = 30;

    // What refreshContextualPanel last acted on, so a poll that changes nothing does nothing.
    // -1 means "nothing shown yet", which no real layer or type can equal.
    int lastShownLayer = -1;
    int lastShownType  = -1;

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

    // Every layer's type parameter, resolved once so the poll does no string lookups.
    std::array<std::atomic<float>*, LayerAudibility::kNumLayers> layerTypeParams { };

    // Every layer's sustain parameter, for the canvas's loop band (Phase 6), likewise.
    std::array<std::atomic<float>*, LayerAudibility::kNumLayers> layerSustainParams { };

    // Stands in for the canvas when the selected layer has nothing to edit - one switched Off.
    // Occupies the canvas's bounds so the swap reads as the panel changing rather than as the
    // window emptying.
    juce::Label contextualPlaceholder;

    // The Click layer's controls (Phase 5), shown in the canvas's place when the selected layer is
    // a Click. One panel re-pointed at whichever layer is selected, like the canvas.
    ClickPanel clickPanel;

    // The Sub layer's controls (Phase 6): harmonics, sustain, phase. Shown under the canvas when
    // the selected layer is a Sub, re-pointed at whichever layer that is.
    SubPanel subPanel;

    // Height SubPanel takes from the bottom of the editing area when it is showing.
    static constexpr int kSubPanelHeight = 110;

    // The page tabs, and the FX page (Phase 7). The Edit page is every other control in this class:
    // they stay direct children, shown and hidden as a set, so nothing about them moved for FX.
    juce::TextButton editTab { "Edit" };
    juce::TextButton fxTab   { "FX" };
    Page currentPage = Page::edit;
    FXPage fxPage;

    // Height of the tab row across the top of every page.
    static constexpr int kTabRowHeight = 28;

    // Everything on the Edit page that is not governed by refreshContextualPanel.
    std::vector<juce::Component*> editPageFixtures();

    // The central canvas (§3.4): pitch + amp curves overlaid on one time axis.
    // Declared after audioProcessor since its constructor reads model
    // references out of it.
    EnvelopeCanvas envelopeCanvas;

    // Log/Linear vertical-axis toggle for the pitch curve (§3.4).
    juce::ToggleButton pitchLogToggle { "Log Scale (Pitch)" };

    // Pitch snap (§3.5), per layer like the log toggle: drags and adds land on the nearest note.
    juce::ToggleButton pitchSnapToggle { "Snap to Note" };

    // Global tuning (Phase 6, §3.5): master tuning in cents, and the key root keytracked nodes
    // play as drawn at. Attached once - they are the same for every layer.
    juce::Label  tuneCaption    { {}, "Tune" };
    juce::Slider tuneSlider;
    juce::Label  keyRootCaption { {}, "Key Root" };
    juce::Slider keyRootSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> keyRootAttachment;

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
