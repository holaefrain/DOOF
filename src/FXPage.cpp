#include "FXPage.h"

const char* const FXPage::pageID           = "fxPage";
const char* const FXPage::oversamplingID   = "fxOversampling";
const char* const FXPage::slotTypeID       = "fxSlotType";
const char* const FXPage::slotOnID         = "fxSlotOn";
const char* const FXPage::slotMixID        = "fxSlotMix";
const char* const FXPage::assignmentListID = "fxMacroAssignments";

namespace
{
    const juce::Colour background   { 0xff1a1a1a };
    const juce::Colour panel        = juce::Colours::white.withAlpha(0.05f);
    const juce::Colour accent       { 0xffe0a030 };
    const juce::String dragPrefix   = "fxslot:";

    void styleCaption(juce::Label& label, juce::Justification justification = juce::Justification::centred)
    {
        label.setColour(juce::Label::textColourId, juce::Colours::white);
        label.setJustificationType(justification);
    }
}

// ── A slot tile ──────────────────────────────────────────────────────────────

// One slot in a channel column: its number, its module, and an LED for on/off. Click to edit it,
// drag it onto another tile to swap the two.
class FXPage::SlotTile : public juce::Component,
                         public juce::DragAndDropTarget
{
public:
    SlotTile(FXPage& page, int slotIndex) : owner(page), slot(slotIndex)
    {
        setComponentID(FXPage::slotTileID(slotIndex));
    }

    void update(FX::ModuleType newType, bool newOn, bool newSelected)
    {
        if (newType == type && newOn == on && newSelected == selected)
            return;

        type = newType;
        on = newOn;
        selected = newSelected;
        repaint();
    }

    FX::ModuleType getType() const { return type; }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(selected ? accent.withAlpha(0.25f) : juce::Colours::white.withAlpha(0.06f));
        g.fillRoundedRectangle(bounds, 4.0f);

        if (hovering || selected)
        {
            g.setColour(hovering ? juce::Colours::white : accent);
            g.drawRoundedRectangle(bounds, 4.0f, 1.5f);
        }

        auto area = getLocalBounds().reduced(8, 0);
        g.setColour(juce::Colours::grey);
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(juce::String(FX::slotInChannel(slot) + 1), area.removeFromLeft(14), juce::Justification::centredLeft);

        const bool empty = (type == FX::ModuleType::none);
        if (! empty)
        {
            const auto led = area.removeFromRight(10).withSizeKeepingCentre(8, 8).toFloat();
            g.setColour(on ? juce::Colour(0xff40d060) : juce::Colours::darkgrey);
            g.fillEllipse(led);
        }

        g.setColour(empty ? juce::Colours::grey : juce::Colours::white);
        g.setFont(juce::FontOptions(13.0f));
        g.drawText(empty ? juce::String("Empty") : juce::String(FX::getInfo(type).name), area.reduced(4, 0),
                   juce::Justification::centredLeft);
    }

    void mouseDown(const juce::MouseEvent&) override { owner.selectSlot(slot); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (type != FX::ModuleType::none && e.getDistanceFromDragStart() > 4 && ! owner.isDragAndDropActive())
            owner.startDragging(dragPrefix + juce::String(slot), this);
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return details.description.toString().startsWith(dragPrefix);
    }

    void itemDragEnter(const SourceDetails&) override { hovering = true;  repaint(); }
    void itemDragExit(const SourceDetails&) override  { hovering = false; repaint(); }

    void itemDropped(const SourceDetails& details) override
    {
        hovering = false;
        const int from = details.description.toString().fromFirstOccurrenceOf(dragPrefix, false, false).getIntValue();

        if (from != slot && juce::isPositiveAndBelow(from, FX::kNumSlots))
            owner.processor.swapFXSlots(from, slot);

        owner.selectSlot(slot);
        owner.refresh();
    }

private:
    FXPage& owner;
    const int slot;
    FX::ModuleType type = FX::ModuleType::none;
    bool on = true, selected = false, hovering = false;
};

// ── A knob that offers the macro menu on right-click ─────────────────────────

class FXPage::MacroKnob : public juce::Slider
{
public:
    std::function<void()> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && onRightClick)
        {
            onRightClick();
            return;
        }
        juce::Slider::mouseDown(e);
    }
};

// ── One row of the assignment list ───────────────────────────────────────────

namespace
{
    class AssignmentRow : public juce::Component
    {
    public:
        AssignmentRow(MacroAssignments& model, const FX::MacroAssignment& a, const juce::String& name)
            : assignments(model), assignment(a)
        {
            styleCaption(label, juce::Justification::centredLeft);
            label.setText(name, juce::dontSendNotification);
            addAndMakeVisible(label);

            depth.setComponentID("depth");
            depth.setSliderStyle(juce::Slider::LinearHorizontal);
            depth.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
            depth.setRange(-100.0, 100.0, 1.0);
            depth.setValue(a.depth * 100.0, juce::dontSendNotification);
            depth.textFromValueFunction = [](double v) { return (v > 0.0 ? "+" : "") + juce::String(juce::roundToInt(v)) + "%"; };
            depth.valueFromTextFunction = [](const juce::String& t) { return t.trim().getDoubleValue(); };
            depth.updateText();
            depth.onValueChange = [this]
            {
                assignments.assign(assignment.macro, assignment.slot, assignment.target, (float) (depth.getValue() / 100.0));
            };
            addAndMakeVisible(depth);

            // Removed on the click; the page's next poll drops this row. Never rebuilt from inside the
            // callback, which would delete the button whose callback is running.
            remove.setComponentID("remove");
            remove.onClick = [this] { assignments.remove(assignment.macro, assignment.slot, assignment.target); };
            addAndMakeVisible(remove);
        }

        bool matches(const FX::MacroAssignment& a) const
        {
            return a.macro == assignment.macro && a.slot == assignment.slot && a.target == assignment.target;
        }

        void showDepth(float newDepth)
        {
            if (! depth.isMouseButtonDown())
                depth.setValue(newDepth * 100.0, juce::dontSendNotification);
        }

        void resized() override
        {
            auto b = getLocalBounds();
            remove.setBounds(b.removeFromRight(24).reduced(2));
            depth.setBounds(b.removeFromRight(170));
            label.setBounds(b);
        }

    private:
        MacroAssignments& assignments;
        const FX::MacroAssignment assignment;
        juce::Label label;
        juce::Slider depth;
        juce::TextButton remove { "x" };
    };
}

// ── The page ─────────────────────────────────────────────────────────────────

FXPage::FXPage(DOOFAudioProcessor& p)
    : processor(p), apvts(p.apvts)
{
    setComponentID(pageID);
    using APVTS = juce::AudioProcessorValueTreeState;

    // Routing.
    styleCaption(routingCaption, juce::Justification::centredLeft);
    addAndMakeVisible(routingCaption);
    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
    {
        routeCaptions[(size_t) i].setText("L" + juce::String(i + 1), juce::dontSendNotification);
        styleCaption(routeCaptions[(size_t) i], juce::Justification::centredRight);
        addAndMakeVisible(routeCaptions[(size_t) i]);

        auto& box = routeBoxes[(size_t) i];
        box.addItemList(FX::routeChoices(), 1); // before the attachment, so IDs line up with indices
        box.setComponentID(routeID(i));
        addAndMakeVisible(box);
        routeAttachments[(size_t) i] = std::make_unique<APVTS::ComboBoxAttachment>(apvts, ParamIDs::layerRoute(i), box);
    }

    styleCaption(oversamplingCaption, juce::Justification::centredRight);
    addAndMakeVisible(oversamplingCaption);
    oversamplingBox.addItemList(FX::oversamplingChoices(), 1);
    oversamplingBox.setComponentID(oversamplingID);
    addAndMakeVisible(oversamplingBox);
    oversamplingAttachment = std::make_unique<APVTS::ComboBoxAttachment>(apvts, ParamIDs::fxOversampling, oversamplingBox);

    // Channels.
    for (int c = 0; c < FX::kNumChannels; ++c)
    {
        auto& caption = channelCaptions[(size_t) c];
        caption.setText(FX::channelName((FX::Channel) c), juce::dontSendNotification);
        styleCaption(caption, juce::Justification::centredLeft);
        caption.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        addAndMakeVisible(caption);
    }

    for (int i = 0; i < 2; ++i)
    {
        auto& level = insertLevels[(size_t) i];
        level.setSliderStyle(juce::Slider::LinearHorizontal);
        level.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
        level.setComponentID(insertLevelID(i));
        addAndMakeVisible(level);
        insertLevelAttachments[(size_t) i] = std::make_unique<APVTS::SliderAttachment>(apvts, ParamIDs::fxInsertLevel(i), level);
        level.textFromValueFunction = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        level.valueFromTextFunction = [](const juce::String& t) { return t.trim().getDoubleValue() / 100.0; };
        level.updateText();
    }

    for (int s = 0; s < FX::kNumSlots; ++s)
    {
        tiles[(size_t) s] = std::make_unique<SlotTile>(*this, s);
        addAndMakeVisible(*tiles[(size_t) s]);
    }

    // The slot editor.
    styleCaption(slotTitle, juce::Justification::centredLeft);
    slotTitle.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    addAndMakeVisible(slotTitle);

    slotTypeBox.addItemList(FX::moduleTypeChoices(), 1);
    slotTypeBox.setComponentID(slotTypeID);
    // Not attached: picking a module also loads its defaults, which is the processor's job.
    slotTypeBox.onChange = [this]
    {
        const int picked = slotTypeBox.getSelectedId() - 1;
        if (picked >= 0 && picked != (int) slotType(selectedSlot))
        {
            processor.setFXSlotModule(selectedSlot, (FX::ModuleType) picked);
            refresh();
        }
    };
    addAndMakeVisible(slotTypeBox);

    slotOnButton.setComponentID(slotOnID);
    slotOnButton.setColour(juce::ToggleButton::textColourId, juce::Colours::white);
    addAndMakeVisible(slotOnButton);

    auto styleKnob = [this](MacroKnob& knob, int target)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 76, 16);
        knob.onRightClick = [this, target] { showMacroMenu(target); };
        addAndMakeVisible(knob);
    };

    mixKnob = std::make_unique<MacroKnob>();
    mixKnob->setComponentID(slotMixID);
    styleKnob(*mixKnob, FX::kMixTarget);
    styleCaption(mixCaption);
    addAndMakeVisible(mixCaption);

    for (int k = 0; k < FX::kNumSlotParams; ++k)
    {
        paramKnobs[(size_t) k] = std::make_unique<MacroKnob>();
        paramKnobs[(size_t) k]->setComponentID(slotParamID(k));
        styleKnob(*paramKnobs[(size_t) k], k);
        styleCaption(paramCaptions[(size_t) k]);
        addAndMakeVisible(paramCaptions[(size_t) k]);
    }

    // Macros.
    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        auto& selector = macroSelectors[(size_t) m];
        selector.setComponentID(macroSelectID(m));
        selector.setClickingTogglesState(true);
        selector.setRadioGroupId(0x4d41); // "MA"
        selector.setColour(juce::TextButton::buttonOnColourId, accent.withAlpha(0.6f));
        selector.onClick = [this, m] { selectMacro(m); };
        addAndMakeVisible(selector);

        auto& knob = macroKnobs[(size_t) m];
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 16);
        knob.setComponentID(macroID(m));
        addAndMakeVisible(knob);
        macroAttachments[(size_t) m] = std::make_unique<APVTS::SliderAttachment>(apvts, ParamIDs::fxMacro(m), knob);
    }

    styleCaption(assignmentsCaption, juce::Justification::centredLeft);
    addAndMakeVisible(assignmentsCaption);
    assignmentList.setComponentID(assignmentListID);
    assignmentViewport.setViewedComponent(&assignmentList, false);
    assignmentViewport.setScrollBarsShown(true, false);
    addAndMakeVisible(assignmentViewport);

    selectSlot(0);
    selectMacro(0);
}

FXPage::~FXPage()
{
    stopTimer();
}

FX::ModuleType FXPage::getTileType(int slot) const
{
    return tiles[(size_t) slot]->getType();
}

FX::ModuleType FXPage::slotType(int slot) const
{
    const auto* value = apvts.getRawParameterValue(ParamIDs::fxSlotType(slot));
    return (FX::ModuleType) juce::jlimit(0, FX::kNumModuleTypes - 1, juce::roundToInt(value->load()));
}

juce::String FXPage::targetName(int slot, int target, FX::ModuleType type)
{
    const auto control = target == FX::kMixTarget ? juce::String("Mix")
                                                   : juce::String(FX::getInfo(type).params[(size_t) target].name);
    return ParamIDs::fxSlotName(slot) + " " + control;
}

void FXPage::selectSlot(int slot)
{
    jassert(juce::isPositiveAndBelow(slot, FX::kNumSlots));
    selectedSlot = slot;
    attachSlotEditor();
    refresh();
}

void FXPage::selectMacro(int macro)
{
    jassert(juce::isPositiveAndBelow(macro, FX::kNumMacros));
    selectedMacro = macro;
    macroSelectors[(size_t) macro].setToggleState(true, juce::dontSendNotification);
    listedAssignments.clear();
    assignmentRows.clear(); // forces the list to rebuild for this macro
    refresh();
}

// Re-points the editor's attachments at the selected slot - the old ones first, so the outgoing
// slot can never be written through a stale attachment.
void FXPage::attachSlotEditor()
{
    slotOnAttachment.reset();
    mixAttachment.reset();
    for (auto& attachment : paramAttachments)
        attachment.reset();

    using APVTS = juce::AudioProcessorValueTreeState;
    slotOnAttachment = std::make_unique<APVTS::ButtonAttachment>(apvts, ParamIDs::fxSlotOn(selectedSlot), slotOnButton);
    mixAttachment = std::make_unique<APVTS::SliderAttachment>(apvts, ParamIDs::fxSlotMix(selectedSlot), *mixKnob);
    for (int k = 0; k < FX::kNumSlotParams; ++k)
        paramAttachments[(size_t) k] = std::make_unique<APVTS::SliderAttachment>(apvts, ParamIDs::fxSlotParam(selectedSlot, k),
                                                                                 *paramKnobs[(size_t) k]);

    editorShowsType = (FX::ModuleType) -1; // force refreshSlotEditor to redo everything
}

void FXPage::refresh()
{
    for (int s = 0; s < FX::kNumSlots; ++s)
        tiles[(size_t) s]->update(slotType(s), apvts.getRawParameterValue(ParamIDs::fxSlotOn(s))->load() > 0.5f, s == selectedSlot);

    refreshSlotEditor();
    rebuildAssignmentList();

    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        const auto count = (int) processor.getMacroAssignments().getAssignments(m).size();
        macroSelectors[(size_t) m].setButtonText("Macro " + juce::String(m + 1) + (count > 0 ? " (" + juce::String(count) + ")" : ""));
    }
}

void FXPage::refreshSlotEditor()
{
    const auto type = slotType(selectedSlot);
    const auto& info = FX::getInfo(type);
    auto& assignments = processor.getMacroAssignments();

    // Which macros drive each control, as " M1 M3" after its name.
    auto marks = [&](int target)
    {
        juce::String text;
        for (int m = 0; m < FX::kNumMacros; ++m)
            if (assignments.isAssigned(m, selectedSlot, target))
                text << " M" << (m + 1);
        return text;
    };

    juce::String assignmentSummary;
    for (int t = 0; t < FX::kNumTargets; ++t)
        assignmentSummary << marks(t) << "|";

    if (type == editorShowsType && assignmentSummary == editorShowsAssignments)
        return;

    editorShowsType = type;
    editorShowsAssignments = assignmentSummary;

    slotTitle.setText(FX::channelName(FX::channelOf(selectedSlot)) + "  -  Slot " + juce::String(FX::slotInChannel(selectedSlot) + 1),
                      juce::dontSendNotification);
    slotTypeBox.setSelectedId((int) type + 1, juce::dontSendNotification);

    const bool hasModule = type != FX::ModuleType::none;
    slotOnButton.setVisible(hasModule);
    mixKnob->setVisible(hasModule);
    mixCaption.setVisible(hasModule);
    mixCaption.setText("Mix" + marks(FX::kMixTarget), juce::dontSendNotification);
    mixKnob->setDoubleClickReturnValue(true, info.defaultMix);

    for (int k = 0; k < FX::kNumSlotParams; ++k)
    {
        const auto& spec = info.params[(size_t) k];
        auto& knob = *paramKnobs[(size_t) k];
        knob.setVisible(spec.isUsed());
        paramCaptions[(size_t) k].setVisible(spec.isUsed());

        if (spec.isUsed())
        {
            paramCaptions[(size_t) k].setText(spec.name + marks(k), juce::dontSendNotification);
            knob.setDoubleClickReturnValue(true, FX::defaultNormalised(spec));
        }

        // The attachment reads the parameter's own text, which follows the module in the slot.
        knob.updateText();
    }
}

void FXPage::rebuildAssignmentList()
{
    auto& assignments = processor.getMacroAssignments();
    const auto current = assignments.getAssignments(selectedMacro);

    assignmentsCaption.setText("Macro " + juce::String(selectedMacro + 1) + " drives"
                               + (current.empty() ? juce::String(":  nothing yet - right-click a knob above to assign it")
                                                  : juce::String(":")),
                               juce::dontSendNotification);

    bool sameShape = current.size() == listedAssignments.size() && current.size() == assignmentRows.size();
    for (size_t i = 0; sameShape && i < current.size(); ++i)
        sameShape = static_cast<AssignmentRow*>(assignmentRows[i].get())->matches(current[i]);

    if (sameShape)
    {
        // Only depths can have moved: update them in place, never under a drag.
        for (size_t i = 0; i < current.size(); ++i)
            static_cast<AssignmentRow*>(assignmentRows[i].get())->showDepth(current[i].depth);
        listedAssignments = current;
        return;
    }

    assignmentRows.clear();
    listedAssignments = current;

    constexpr int rowHeight = 24;
    for (size_t i = 0; i < current.size(); ++i)
    {
        const auto& a = current[i];
        auto row = std::make_unique<AssignmentRow>(assignments, a, targetName(a.slot, a.target, slotType(a.slot)));
        row->setComponentID("fxAssignRow" + juce::String((int) i));
        assignmentList.addAndMakeVisible(*row);
        assignmentRows.push_back(std::move(row));
    }

    const int width = juce::jmax(100, assignmentViewport.getMaximumVisibleWidth());
    assignmentList.setSize(width, (int) current.size() * rowHeight);
    for (size_t i = 0; i < assignmentRows.size(); ++i)
        assignmentRows[i]->setBounds(0, (int) i * rowHeight, width, rowHeight);
}

void FXPage::assignToMacro(int macro, int target)
{
    const auto id = target == FX::kMixTarget ? ParamIDs::fxSlotMix(selectedSlot) : ParamIDs::fxSlotParam(selectedSlot, target);
    const float base = apvts.getParameter(id)->getValue();
    processor.getMacroAssignments().assign(macro, selectedSlot, target, base <= 0.5f ? 1.0f - base : -base);
    selectMacro(macro);
}

void FXPage::removeFromMacro(int macro, int target)
{
    processor.getMacroAssignments().remove(macro, selectedSlot, target);
    refresh();
}

void FXPage::showMacroMenu(int target)
{
    juce::PopupMenu menu;
    auto& assignments = processor.getMacroAssignments();

    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        if (assignments.isAssigned(m, selectedSlot, target))
            menu.addItem(100 + m, "Remove from Macro " + juce::String(m + 1));
        else
            menu.addItem(1 + m, "Assign to Macro " + juce::String(m + 1));
    }

    menu.showMenuAsync(juce::PopupMenu::Options(),
                       [safe = juce::Component::SafePointer<FXPage>(this), target](int result)
                       {
                           if (safe == nullptr || result == 0)
                               return;
                           if (result >= 100)
                               safe->removeFromMacro(result - 100, target);
                           else
                               safe->assignToMacro(result - 1, target);
                       });
}

void FXPage::visibilityChanged()
{
    // The poll runs only while the page can be seen; refresh at once on showing, since anything may
    // have changed while it was hidden.
    if (isVisible())
    {
        refresh();
        startTimerHz(kPollHz);
    }
    else
    {
        stopTimer();
    }
}

void FXPage::timerCallback()
{
    refresh();
}

void FXPage::paint(juce::Graphics& g)
{
    // Opaque, like the other panels: the editor paints its name behind everything.
    g.fillAll(background);

    g.setColour(panel);
    for (int c = 0; c < FX::kNumChannels; ++c)
    {
        const auto top = channelCaptions[(size_t) c].getBounds();
        const auto bottom = tiles[(size_t) FX::slotIndex((FX::Channel) c, FX::kSlotsPerChannel - 1)]->getBounds();
        g.fillRoundedRectangle(top.getUnion(bottom).expanded(6, 4).toFloat(), 6.0f);
    }

    const auto editorArea = slotTitle.getBounds().getUnion(paramKnobs.back()->getBounds()).getUnion(mixKnob->getBounds());
    g.fillRoundedRectangle(editorArea.expanded(6, 4).toFloat(), 6.0f);

    const auto macroArea = macroSelectors.front().getBounds().getUnion(assignmentViewport.getBounds()).getUnion(macroKnobs.back().getBounds());
    g.fillRoundedRectangle(macroArea.expanded(6, 4).toFloat(), 6.0f);
}

void FXPage::resized()
{
    auto bounds = getLocalBounds().reduced(10);

    // Routing and oversampling.
    auto row = bounds.removeFromTop(26);
    routingCaption.setBounds(row.removeFromLeft(58));
    oversamplingBox.setBounds(row.removeFromRight(70));
    oversamplingCaption.setBounds(row.removeFromRight(96));
    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
    {
        routeCaptions[(size_t) i].setBounds(row.removeFromLeft(24));
        routeBoxes[(size_t) i].setBounds(row.removeFromLeft(86).reduced(2, 1));
    }
    bounds.removeFromTop(14);

    // Channels: three columns.
    constexpr int tileHeight = 30, gap = 18;
    auto channels = bounds.removeFromTop(26 + FX::kSlotsPerChannel * tileHeight);
    const int columnWidth = (channels.getWidth() - 2 * gap) / FX::kNumChannels;
    for (int c = 0; c < FX::kNumChannels; ++c)
    {
        auto column = channels.removeFromLeft(columnWidth);
        channels.removeFromLeft(gap);

        auto header = column.removeFromTop(26);
        if (c < 2)
            insertLevels[(size_t) c].setBounds(header.removeFromRight(130).reduced(0, 3));
        channelCaptions[(size_t) c].setBounds(header);

        for (int s = 0; s < FX::kSlotsPerChannel; ++s)
            tiles[(size_t) FX::slotIndex((FX::Channel) c, s)]->setBounds(column.removeFromTop(tileHeight).reduced(0, 2));
    }
    bounds.removeFromTop(18);

    // The slot editor.
    auto editor = bounds.removeFromTop(150);
    auto editorTop = editor.removeFromTop(26);
    slotTitle.setBounds(editorTop.removeFromLeft(190));
    slotTypeBox.setBounds(editorTop.removeFromLeft(150).reduced(0, 1));
    editorTop.removeFromLeft(12);
    slotOnButton.setBounds(editorTop.removeFromLeft(60));
    editor.removeFromTop(6);

    const int knobWidth = editor.getWidth() / (FX::kNumSlotParams + 1);
    auto column = [&](juce::Label& caption, juce::Component& knob)
    {
        auto area = editor.removeFromLeft(knobWidth);
        caption.setBounds(area.removeFromTop(18));
        knob.setBounds(area.reduced(4, 0));
    };
    for (int k = 0; k < FX::kNumSlotParams; ++k)
        column(paramCaptions[(size_t) k], *paramKnobs[(size_t) k]);
    column(mixCaption, *mixKnob);
    bounds.removeFromTop(18);

    // Macros: four knobs, then the selected macro's list.
    auto macros = bounds.removeFromTop(juce::jmin(150, bounds.getHeight()));
    auto knobs = macros.removeFromLeft(4 * 86);
    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        auto area = knobs.removeFromLeft(86);
        macroSelectors[(size_t) m].setBounds(area.removeFromTop(24).reduced(3, 0));
        macroKnobs[(size_t) m].setBounds(area.reduced(6, 4));
    }
    macros.removeFromLeft(14);
    assignmentsCaption.setBounds(macros.removeFromTop(24));
    assignmentViewport.setBounds(macros);

    // The rows are laid out to the viewport's width, so a resize has to redo them.
    listedAssignments.clear();
    assignmentRows.clear();
    rebuildAssignmentList();
}
