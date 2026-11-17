#include "PluginEditor.h"
#include "EnvelopeEvaluator.h"

// Initialises the editor, stores the processor reference, and sets the default window size.
// 800x600 is the flat developer skin starting size; resized to final dimensions in Phase 12.
// Pitch range gives headroom above the default 150 Hz start; amp is always
// [0,1]. The initial view duration matches whichever model's Length (§Step 5)
// is currently longer, so the whole work area is visible on load without
// needing to zoom/pan first; the user can zoom/pan from there (Step 4).
const char* const DOOFAudioProcessorEditor::placeholderID   = "contextualPlaceholder";
const char* const DOOFAudioProcessorEditor::tuneSliderID    = "masterTune";
const char* const DOOFAudioProcessorEditor::keyRootSliderID = "keyRoot";
const char* const DOOFAudioProcessorEditor::editTabID       = "tabEdit";
const char* const DOOFAudioProcessorEditor::fxTabID         = "tabFX";

DOOFAudioProcessorEditor::DOOFAudioProcessorEditor(DOOFAudioProcessor& p)
    : AudioProcessorEditor(&p), audioProcessor(p),
      clickPanel(p.apvts, p.getClickSamples()),
      subPanel(p.apvts),
      fxPage(p),
      envelopeCanvas(p.getLayer(0).pitchModel, juce::Range<double>(20.0, 220.0),
                     p.getLayer(0).ampModel,   juce::Range<double>(0.0, 1.0),
                     juce::jmax(p.getLayer(0).pitchModel.getLength(), p.getLayer(0).ampModel.getLength()), p)
{
    // Each of these writes the preference back to the selected layer as well as applying it, so
    // switching away and back restores what you were looking at rather than the previous layer's
    // view. setSelectedLayer pushes the other direction.
    pitchLogToggle.onClick = [this]
    {
        const bool useLog = pitchLogToggle.getToggleState();
        audioProcessor.getLayerViewPrefs(selectedLayer).pitchLogScale = useLog;
        envelopeCanvas.setPitchLogScale(useLog);
    };
    pitchSnapToggle.onClick = [this]
    {
        const bool snap = pitchSnapToggle.getToggleState();
        audioProcessor.getLayerViewPrefs(selectedLayer).pitchSnap = snap;
        envelopeCanvas.setPitchSnap(snap);
    };

    // Global tuning. The attachments install the parameters' text; Tune's is then replaced with a
    // cents readout, while Key Root keeps its own, which already reads and parses note names.
    tuneSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tuneSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
    tuneSlider.setComponentID(tuneSliderID);
    keyRootSlider.setSliderStyle(juce::Slider::IncDecButtons);
    keyRootSlider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 44, 20);
    keyRootSlider.setComponentID(keyRootSliderID);

    tuneAttachment    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                            p.apvts, ParamIDs::masterTune, tuneSlider);
    keyRootAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
                            p.apvts, ParamIDs::masterKeyRoot, keyRootSlider);

    tuneSlider.textFromValueFunction = [](double cents)
    {
        return (cents > 0.0 ? "+" : "") + juce::String(cents, 1) + " c";
    };
    tuneSlider.valueFromTextFunction = [](const juce::String& text) { return text.trim().getDoubleValue(); };
    tuneSlider.updateText();

    for (auto* label : { &tuneCaption, &keyRootCaption })
        label->setColour(juce::Label::textColourId, juce::Colours::white);

    undoButton.onClick = [this] { performUndo(); };
    redoButton.onClick = [this] { performRedo(); };

    editingCurveBox.addItem("Pitch", kEditingPitchId);
    editingCurveBox.addItem("Amp",   kEditingAmpId);
    editingCurveBox.onChange = [this]
    {
        const bool editingPitch = (editingCurveBox.getSelectedId() == kEditingPitchId);
        audioProcessor.getLayerViewPrefs(selectedLayer).editingPitch = editingPitch;
        envelopeCanvas.setActiveCurve(editingPitch ? EnvelopeCanvas::ActiveCurve::pitch
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
        audioProcessor.getLayer(selectedLayer).pitchModel.setLength(clamped);
        refreshLengthLabels();
        envelopeCanvas.repaint();
    };
    ampLengthValue.setEditable(true);
    ampLengthValue.onTextChange = [this]
    {
        const double clamped = juce::jlimit(kMinLength, EnvelopeEvaluator::kTableDomainSeconds,
                                             ampLengthValue.getText().getDoubleValue());
        audioProcessor.getLayer(selectedLayer).ampModel.setLength(clamped);
        refreshLengthLabels();
        envelopeCanvas.repaint();
    };
    envelopeCanvas.onLengthChanged = [this] { refreshLengthLabels(); };
    refreshLengthLabels();

    // Save/Load write and read the exact same state getStateInformation/
    // setStateInformation use for host project save (§7a) — just to a .doof
    // file instead of the host's project data. The FileChooser is kept alive
    // by the shared_ptr captured into its own async callback.
    savePresetButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Save DOOF preset...",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.doof");

        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                              | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, chooser](const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{})
                    return;

                juce::MemoryBlock block;
                audioProcessor.getStateInformation(block);
                file.replaceWithData(block.getData(), block.getSize());
            });
    };

    loadPresetButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Load DOOF preset...",
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
            "*.doof");

        chooser->launchAsync(juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser](const juce::FileChooser& fc)
            {
                const auto file = fc.getResult();
                if (file == juce::File{})
                    return;

                juce::MemoryBlock block;
                if (! file.loadFileAsData(block))
                    return;

                audioProcessor.setStateInformation(block.getData(), (int) block.getSize());
                refreshLengthLabels();
                envelopeCanvas.repaint();
            });
    };

    // Built here rather than in the initialiser list so the callback wiring sits beside the
    // construction. Each strip drives its own layer's parameters through its own attachments;
    // the editor only arbitrates which one is selected, since no strip can know about the others.
    for (int i = 0; i < DOOFAudioProcessor::getNumLayers(); ++i)
    {
        auto strip = std::make_unique<LayerStrip>(audioProcessor.apvts, i);
        strip->onLayerSelected = [this, i] { setSelectedLayer(i); };
        addAndMakeVisible(*strip);
        layerStrips[(size_t) i] = std::move(strip);
    }

    layerStrips[(size_t) selectedLayer]->setSelected(true);

    for (int i = 0; i < DOOFAudioProcessor::getNumLayers(); ++i)
    {
        layerTypeParams[(size_t) i]    = audioProcessor.apvts.getRawParameterValue(ParamIDs::layerType(i));
        layerSustainParams[(size_t) i] = audioProcessor.apvts.getRawParameterValue(ParamIDs::layerSubSustain(i));
        jassert(layerTypeParams[(size_t) i] != nullptr && layerSustainParams[(size_t) i] != nullptr);
    }

    contextualPlaceholder.setComponentID(placeholderID);
    contextualPlaceholder.setJustificationType(juce::Justification::centred);
    contextualPlaceholder.setColour(juce::Label::textColourId, juce::Colours::grey);
    addChildComponent(contextualPlaceholder); // added but not shown; the panel decides
    addChildComponent(clickPanel);            // likewise
    addChildComponent(subPanel);              // likewise

    // Children must be added before setSize(), which triggers the initial
    // resized() layout pass — a lesson from Phase 3 Step 0's spike.
    addAndMakeVisible(envelopeCanvas);
    addAndMakeVisible(pitchLogToggle);
    addAndMakeVisible(pitchSnapToggle);
    addAndMakeVisible(tuneCaption);
    addAndMakeVisible(tuneSlider);
    addAndMakeVisible(keyRootCaption);
    addAndMakeVisible(keyRootSlider);
    addAndMakeVisible(undoButton);
    addAndMakeVisible(redoButton);
    addAndMakeVisible(editingCurveBox);
    addAndMakeVisible(pitchLengthCaption);
    addAndMakeVisible(pitchLengthValue);
    addAndMakeVisible(ampLengthCaption);
    addAndMakeVisible(ampLengthValue);
    addAndMakeVisible(savePresetButton);
    addAndMakeVisible(loadPresetButton);

    // Page tabs. Radio buttons, so exactly one is lit.
    for (auto* tab : { &editTab, &fxTab })
    {
        tab->setClickingTogglesState(true);
        tab->setRadioGroupId(0x5047); // "PG"
        tab->setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffe0a030).withAlpha(0.6f));
        addAndMakeVisible(*tab);
    }
    editTab.setComponentID(editTabID);
    fxTab.setComponentID(fxTabID);
    editTab.setToggleState(true, juce::dontSendNotification);
    editTab.onClick = [this] { showPage(Page::edit); };
    fxTab.onClick   = [this] { showPage(Page::fx); };
    addChildComponent(fxPage);
    // 700 tall: the Sub panel (Phase 6) and the page tabs (Phase 7) share what the canvas used to have.
    setSize(800, 700);

    // The editor is always built against state that already exists - plugin open, preset load,
    // session restore - so the toolbar has to show the selected layer's stored view immediately
    // rather than only from the first layer switch onwards.
    applyViewPrefsToUI();
    refreshLengthLabels();

    // After setSize, so the placeholder already has the canvas's bounds if it is the one shown.
    refreshContextualPanel();
    startTimerHz(kPollHz);

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

    // The tab row, on every page: the tabs at the left, preset save/load (also global) at the right.
    auto tabRow = bounds.removeFromTop(kTabRowHeight);
    editTab.setBounds(tabRow.removeFromLeft(80).reduced(2));
    fxTab.setBounds(tabRow.removeFromLeft(80).reduced(2));
    loadPresetButton.setBounds(tabRow.removeFromRight(70).reduced(2));
    savePresetButton.setBounds(tabRow.removeFromRight(70).reduced(2));

    // Everything below the tabs is the FX page when it shows.
    fxPage.setBounds(bounds);

    auto topBar = bounds.removeFromTop(24);
    pitchLogToggle.setBounds(topBar.removeFromLeft(160).reduced(2));
    pitchSnapToggle.setBounds(topBar.removeFromLeft(120).reduced(2));
    undoButton.setBounds(topBar.removeFromLeft(60).reduced(2));
    redoButton.setBounds(topBar.removeFromLeft(60).reduced(2));
    editingCurveBox.setBounds(topBar.removeFromLeft(100).reduced(2));

    auto secondBar = bounds.removeFromTop(24);
    pitchLengthCaption.setBounds(secondBar.removeFromLeft(90).reduced(2));
    pitchLengthValue.setBounds(secondBar.removeFromLeft(60).reduced(2));
    ampLengthCaption.setBounds(secondBar.removeFromLeft(90).reduced(2));
    ampLengthValue.setBounds(secondBar.removeFromLeft(60).reduced(2));

    // The global tuning pair, at the right of the second bar.
    keyRootSlider.setBounds(secondBar.removeFromRight(100).reduced(2));
    keyRootCaption.setBounds(secondBar.removeFromRight(64).reduced(2));
    tuneSlider.setBounds(secondBar.removeFromRight(150).reduced(2));
    tuneCaption.setBounds(secondBar.removeFromRight(40).reduced(2));

    // The layer selector (§4.3), above the canvas and spanning the full width. Cells are divided
    // by remaining width rather than a fixed size, so the row still fills a resized window; the
    // integer division leaves at most four pixels unused at the right, which the last cell takes.
    auto layerBar = bounds.removeFromTop(80);
    const int cellWidth = layerBar.getWidth() / (int) layerStrips.size();

    for (size_t i = 0; i < layerStrips.size(); ++i)
    {
        const bool isLast = (i + 1 == layerStrips.size());
        layerStrips[i]->setBounds(isLast ? layerBar : layerBar.removeFromLeft(cellWidth));
    }

    // The click panel and the placeholder take the whole editing area. A Sub layer splits it: the
    // canvas above, its SubPanel below. refreshContextualPanel decides which set is visible.
    contextualPlaceholder.setBounds(bounds);
    clickPanel.setBounds(bounds);

    auto canvasArea = bounds;
    subPanel.setBounds(canvasArea.removeFromBottom(kSubPanelHeight));
    envelopeCanvas.setBounds(canvasArea);
}

void DOOFAudioProcessorEditor::applyViewPrefsToUI()
{
    const auto& prefs = audioProcessor.getLayerViewPrefs(selectedLayer);

    // dontSendNotification because the callback would only write back the value just read. What
    // actually keeps a switch from writing onto the wrong layer is the order in setSelectedLayer:
    // selectedLayer is assigned before this runs. Suppressing the notification too means that
    // ordering is not the single thing standing between a layer switch and silent data loss.
    pitchLogToggle.setToggleState(prefs.pitchLogScale, juce::dontSendNotification);
    pitchSnapToggle.setToggleState(prefs.pitchSnap, juce::dontSendNotification);
    envelopeCanvas.setPitchSnap(prefs.pitchSnap);
    editingCurveBox.setSelectedId(prefs.editingPitch ? kEditingPitchId : kEditingAmpId,
                                   juce::dontSendNotification);

    // Which means the canvas has to be told directly, since the callbacks that normally do it
    // were just suppressed.
    envelopeCanvas.setPitchLogScale(prefs.pitchLogScale);
    envelopeCanvas.setActiveCurve(prefs.editingPitch ? EnvelopeCanvas::ActiveCurve::pitch
                                                      : EnvelopeCanvas::ActiveCurve::amp);
}

void DOOFAudioProcessorEditor::refreshContextualPanel()
{
    // The FX page covers the editing area; nothing here may show through it. showPage puts the
    // panel back on returning to the Edit page.
    if (currentPage != Page::edit)
        return;

    const int type = (int) layerTypeParams[(size_t) selectedLayer]->load();

    if (selectedLayer == lastShownLayer && type == lastShownType)
        return;

    lastShownLayer = selectedLayer;
    lastShownType  = type;

    // Sub edits curves on the canvas, Click gets its own panel, and Off has nothing to show by
    // definition.
    const bool isSub   = (type == (int) ParamIDs::LayerType::sub);
    const bool isClick = (type == (int) ParamIDs::LayerType::click);

    envelopeCanvas.setVisible(isSub);

    // The canvas toolbar goes with it - these controls all act on the curves, so leaving them
    // live over an empty panel would let you edit a layer that isn't on screen. Undo/redo and
    // preset save/load stay put: they are global, not per-layer.
    pitchLogToggle.setVisible(isSub);
    pitchSnapToggle.setVisible(isSub);
    editingCurveBox.setVisible(isSub);

    // Re-pointed before it is shown, like the click panel below.
    if (isSub)
        subPanel.setLayer(selectedLayer);
    subPanel.setVisible(isSub);
    pitchLengthCaption.setVisible(isSub);
    pitchLengthValue.setVisible(isSub);
    ampLengthCaption.setVisible(isSub);
    ampLengthValue.setVisible(isSub);

    // Re-pointed before it is shown, so it never appears for a frame still driving the previous
    // layer. setLayer is a no-op when the layer has not changed.
    if (isClick)
        clickPanel.setLayer(selectedLayer);
    clickPanel.setVisible(isClick);

    contextualPlaceholder.setVisible(! isSub && ! isClick);
    if (! isSub && ! isClick)
        contextualPlaceholder.setText("Layer off", juce::dontSendNotification);
}

void DOOFAudioProcessorEditor::timerCallback()
{
    refreshContextualPanel();

    // The click type can move under automation or a preset load with no gesture here. The combo
    // box follows through its attachment, but the panel's status line needs telling.
    if (clickPanel.isVisible())
        clickPanel.refreshStatus();

    // Sustain can be automated too; the canvas draws the loop band only while it is on.
    envelopeCanvas.setSustainLoopShown(layerSustainParams[(size_t) selectedLayer]->load() > 0.5f);
}

std::vector<juce::Component*> DOOFAudioProcessorEditor::editPageFixtures()
{
    std::vector<juce::Component*> fixtures { &tuneCaption, &tuneSlider, &keyRootCaption, &keyRootSlider,
                                             &undoButton, &redoButton };
    for (auto& strip : layerStrips)
        fixtures.push_back(strip.get());
    return fixtures;
}

void DOOFAudioProcessorEditor::showPage(Page page)
{
    currentPage = page;
    const bool edit = (page == Page::edit);

    (edit ? editTab : fxTab).setToggleState(true, juce::dontSendNotification);

    for (auto* component : editPageFixtures())
        component->setVisible(edit);

    if (edit)
    {
        fxPage.setVisible(false);

        // Re-decided from scratch: the contextual panel may have changed type while hidden.
        lastShownLayer = lastShownType = -1;
        refreshContextualPanel();
    }
    else
    {
        // Everything refreshContextualPanel might have shown goes too.
        for (auto* component : std::initializer_list<juce::Component*> {
                 &envelopeCanvas, &subPanel, &clickPanel, &contextualPlaceholder, &pitchLogToggle,
                 &pitchSnapToggle, &editingCurveBox, &pitchLengthCaption, &pitchLengthValue,
                 &ampLengthCaption, &ampLengthValue })
            component->setVisible(false);

        fxPage.setVisible(true);
    }
}

void DOOFAudioProcessorEditor::setSelectedLayer(int layerIndex)
{
    jassert(juce::isPositiveAndBelow(layerIndex, DOOFAudioProcessor::getNumLayers()));

    selectedLayer = layerIndex;

    // Every strip is told, not just the two that changed: setSelected() already no-ops when the
    // value is unchanged, so this cannot leave two outlined if selection ever moves another way.
    for (int i = 0; i < DOOFAudioProcessor::getNumLayers(); ++i)
        layerStrips[(size_t) i]->setSelected(i == layerIndex);

    // The canvas now edits the selected layer's curves. The toolbar around it - the Length
    // fields, the log toggle, the editing-curve selector - still reads layer 1 until 6c.
    auto& layer = audioProcessor.getLayer(layerIndex);
    envelopeCanvas.setModels(layer.pitchModel, layer.ampModel);

    // Both read selectedLayer, so both must run after the assignment above. Running
    // applyViewPrefsToUI first would push the incoming layer's view onto the outgoing one.
    applyViewPrefsToUI();
    refreshLengthLabels();

    // Immediately, rather than waiting up to a frame for the poll to notice: a click should swap
    // the panel on the same gesture, not visibly a moment later.
    refreshContextualPanel();
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
    audioProcessor.getLayer(0).pitchModel.undo();
    envelopeCanvas.repaint();
}

void DOOFAudioProcessorEditor::performRedo()
{
    audioProcessor.getLayer(0).pitchModel.redo();
    envelopeCanvas.repaint();
}

void DOOFAudioProcessorEditor::refreshLengthLabels()
{
    pitchLengthValue.setText(juce::String(audioProcessor.getLayer(selectedLayer).pitchModel.getLength(), 3),
                              juce::dontSendNotification);
    ampLengthValue.setText(juce::String(audioProcessor.getLayer(selectedLayer).ampModel.getLength(), 3),
                            juce::dontSendNotification);
}
