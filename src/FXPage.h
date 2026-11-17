#pragma once
#include "PluginProcessor.h"
#include "ParamIDs.h"
#include <array>
#include <memory>
#include <vector>

// FXPage — the FX page (§4's page tabs: Edit · FX · ...), Phase 7.
//
// Top to bottom: each layer's route and the global oversampling; the three channels - Insert A,
// Insert B, Master - as six slot tiles each, with the inserts' levels; the selected slot's editor
// (module, on, mix, and its six controls, labelled and read in that module's own terms); and the
// four macros with the selected macro's assignment list.
//
// Drag a tile onto another to swap the two slots, settings and macro assignments included (user
// decision 2026-11-17). Right-click any knob in the slot editor to assign it to a macro.
//
// Every control follows host automation and preset loads: the knobs through their attachments,
// everything else - tile names, which controls a module uses, the assignment list - through a
// ~30 fps poll that runs only while the page is showing. Position-agnostic per §0.5.
class FXPage : public juce::Component,
               public juce::DragAndDropContainer,
               private juce::Timer
{
public:
    explicit FXPage(DOOFAudioProcessor& processor);
    ~FXPage() override;

    // Which slot the editor shows (0-17), and which macro's assignments the list shows (0-3).
    void selectSlot(int slot);
    int  getSelectedSlot() const { return selectedSlot; }
    void selectMacro(int macro);
    int  getSelectedMacro() const { return selectedMacro; }

    // What a knob's right-click menu does, on the selected slot. target is 0-5 for p1-p6 or
    // FX::kMixTarget. A new assignment's depth sweeps the control from where it is set to whichever
    // end is further away, so turning the macro up always does something audible.
    void assignToMacro(int macro, int target);
    void removeFromMacro(int macro, int target);

    // Re-reads everything that has no attachment. The poll calls it; tests call it directly.
    void refresh();

    // Component IDs, so tests can reach the controls without this class exposing its children.
    static const char* const pageID;
    static const char* const oversamplingID;
    static const char* const slotTypeID;
    static const char* const slotOnID;
    static const char* const slotMixID;
    static const char* const assignmentListID;
    static juce::String routeID(int layer)        { return "fxRoute" + juce::String(layer + 1); }
    static juce::String insertLevelID(int insert) { return insert == 0 ? "fxInsertALevel" : "fxInsertBLevel"; }
    static juce::String slotTileID(int slot)      { return "fxSlotTile" + juce::String(slot); }
    static juce::String slotParamID(int index)    { return "fxSlotParam" + juce::String(index + 1); }
    static juce::String macroID(int macro)        { return "fxMacro" + juce::String(macro + 1); }
    static juce::String macroSelectID(int macro)  { return "fxMacroSelect" + juce::String(macro + 1); }

    // "Insert A 3 Cutoff", "Master 1 Mix" - how an assignment is named in the list.
    static juce::String targetName(int slot, int target, FX::ModuleType type);

    // The module a slot's tile currently shows - what the poll last read for it.
    FX::ModuleType getTileType(int slot) const;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class SlotTile;
    class MacroKnob;

    void timerCallback() override;
    void attachSlotEditor();
    void refreshSlotEditor();
    void rebuildAssignmentList();
    FX::ModuleType slotType(int slot) const;
    void showMacroMenu(int target);

    DOOFAudioProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;

    int selectedSlot = 0;
    int selectedMacro = 0;

    // Routing and oversampling.
    juce::Label routingCaption { {}, "Routing" };
    std::array<juce::Label, LayerAudibility::kNumLayers> routeCaptions;
    std::array<juce::ComboBox, LayerAudibility::kNumLayers> routeBoxes;
    juce::Label oversamplingCaption { {}, "Oversampling" };
    juce::ComboBox oversamplingBox;

    // The three channels.
    std::array<juce::Label, FX::kNumChannels> channelCaptions;
    std::array<juce::Slider, 2> insertLevels;
    std::array<std::unique_ptr<SlotTile>, FX::kNumSlots> tiles;

    // The selected slot's editor.
    juce::Label slotTitle;
    juce::ComboBox slotTypeBox;
    juce::ToggleButton slotOnButton { "On" };
    juce::Label mixCaption { {}, "Mix" };
    std::unique_ptr<MacroKnob> mixKnob;
    std::array<juce::Label, FX::kNumSlotParams> paramCaptions;
    std::array<std::unique_ptr<MacroKnob>, FX::kNumSlotParams> paramKnobs;
    FX::ModuleType editorShowsType = FX::ModuleType::none;
    juce::String editorShowsAssignments;

    // Macros.
    std::array<juce::TextButton, FX::kNumMacros> macroSelectors;
    std::array<juce::Slider, FX::kNumMacros> macroKnobs;
    juce::Label assignmentsCaption;
    juce::Viewport assignmentViewport;
    juce::Component assignmentList;
    std::vector<std::unique_ptr<juce::Component>> assignmentRows;
    std::vector<FX::MacroAssignment> listedAssignments;

    // Attachments last, so each is destroyed before the control it drives.
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>, LayerAudibility::kNumLayers> routeAttachments;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> oversamplingAttachment;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, 2> insertLevelAttachments;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, FX::kNumMacros> macroAttachments;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> slotOnAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mixAttachment;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>, FX::kNumSlotParams> paramAttachments;

    static constexpr int kPollHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FXPage)
};
