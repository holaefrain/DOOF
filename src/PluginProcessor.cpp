#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DefaultEnvelopes.h"
#include "ParamIDs.h"
#include "LayerAudibility.h"

// State-serialisation schema for getStateInformation/setStateInformation and the .doof file:
//
//   DOOFState { version }
//     DOOF_State                                  <- apvts.copyState(), every parameter
//     LAYER { index, logScale, editingPitch, pitchSnap }  <- one per layer, five of them
//       ENVELOPE { curve="pitch", ... }
//       ENVELOPE { curve="amp",   ... }
//     MACROS                                      <- the FX macros' assignments (version 5)
//       ASSIGN { macro, slot, target, depth }
//
// Both envelopes are EnvelopeIDs::envelopeType ("ENVELOPE"), so curveProp distinguishes them.
// Never rename any of these once a preset has shipped - same rule as APVTS parameter IDs (§2).
//
// Versions, oldest first:
//   absent - Phase 3: no version, both ENVELOPE trees straight on the root, describing one layer.
//   2      - Phase 4: LAYER children, five of them. No view preferences.
//   3      - Phase 4: adds logScale/editingPitch to each LAYER.
//   4      - Phase 6: adds pitchSnap to each LAYER. Pitch NODEs may now carry keytrack/phaseLock,
//            and the APVTS holds the Phase 6 parameters; all three default to the pre-Phase-6
//            behaviour when absent, so reading a version 3 file needs no branch either.
//   5      - Phase 7: adds MACROS. The FX parameters are in the APVTS and default to an empty FX
//            section with every layer routed Dry, which is exactly how an older file sounded; a
//            missing MACROS means no assignments. No branch again.
//
// Only the flat layout needs detecting explicitly, which absence of versionProp does. Reading a
// version 2 file needs no branch of its own: the view preferences are read with defaults, and the
// defaults are precisely the behaviour a file written before they existed had.
namespace PresetIDs
{
    static const juce::Identifier rootType          { "DOOFState" };
    static const juce::Identifier layerNodeType     { "LAYER" };
    static const juce::Identifier curveProp         { "curve" };
    static const juce::Identifier indexProp         { "index" };
    static const juce::Identifier versionProp       { "version" };
    static const juce::Identifier logScaleProp      { "logScale" };
    static const juce::Identifier editingPitchProp  { "editingPitch" };
    static const juce::Identifier pitchSnapProp     { "pitchSnap" };
    static const juce::String pitchCurveTag = "pitch";
    static const juce::String ampCurveTag   = "amp";

    // Bumped whenever the layout above changes shape.
    static constexpr int currentVersion = 5;
}

namespace
{
    // A slot's generic control p1-p6, whose meaning depends on the module in the slot (§7.1). It
    // reads and parses its value in that module's terms - "2.50 kHz" for a filter's cutoff, "4.0:1"
    // for a compressor's ratio - so a host's generic editor and its automation lanes show the real
    // thing rather than a bare 0-1, and the FX page's knobs get the same text through their
    // attachments. The stored value is the normalised position either way.
    class FXSlotParameter final : public juce::AudioParameterFloat
    {
    public:
        FXSlotParameter(const juce::String& id, const juce::String& parameterName,
                        const juce::AudioParameterChoice& slotType, int paramIndex)
            : juce::AudioParameterFloat(juce::ParameterID { id, 1 }, parameterName,
                                        juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f),
              type(slotType), index(paramIndex)
        {
        }

        juce::String getText(float normalised, int maximumLength) const override
        {
            const auto& spec = currentSpec();
            const auto text = spec.isUsed() ? FX::toText(spec, normalised) : juce::String("-");
            return maximumLength > 0 ? text.substring(0, maximumLength) : text;
        }

        float getValueForText(const juce::String& text) const override
        {
            const auto& spec = currentSpec();
            return spec.isUsed() ? FX::fromText(spec, text) : juce::jlimit(0.0f, 1.0f, text.getFloatValue());
        }

    private:
        const FX::ParamSpec& currentSpec() const
        {
            return FX::getInfo((FX::ModuleType) type.getIndex()).params[(size_t) index];
        }

        const juce::AudioParameterChoice& type;
        const int index;
    };

    juce::String percentText(float value, int) { return juce::String(juce::roundToInt(value * 100.0f)) + "%"; }
    float percentFromText(const juce::String& text) { return juce::jlimit(0.0f, 1.0f, text.trim().getFloatValue() / 100.0f); }
}

// Construct the processor, declare the bus layout, and initialise the APVTS.
// Output is always stereo (FX domain is stereo end-to-end per §2).
// Sidechain input is declared optional-mono here; it becomes active in Phase 10 (TRIGGER).
DOOFAudioProcessor::DOOFAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output",   juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::mono(),   false)),
      apvts(*this, nullptr, "DOOF_State", createParameterLayout())
{
    // Build the layers and seed every one with the out-of-the-box envelope
    // shapes — including the four that default to Off, so switching one to Sub
    // makes a sound immediately instead of silence from an empty envelope.
    for (int i = 0; i < getNumLayers(); ++i)
        layers[(size_t) i] = std::make_unique<Layer>(envelopeUndoManager);

    // Same helper the legacy preset path uses, so the factory patch can't drift between the two.
    for (int i = 0; i < getNumLayers(); ++i)
        seedLayerWithDefaults(i);

    // Point every click voice at the decoded factory samples. Done once here rather than in
    // prepareToPlay because the buffers never change after construction - there is nothing to
    // republish and no snapshot to swap.
    for (int i = 0; i < getNumLayers(); ++i)
        getLayer(i).clickVoice.setSampleSlots(clickSamples.slots());

    // Seeding above went through the normal undoable edit path, so it left one
    // transaction per added node in the history — roughly 70 across ten models,
    // which would both fill the entire 30-step cap with factory setup and let
    // Cmd+Z at startup dismantle the default patch. The factory patch is a
    // starting point, not a user edit, so the history starts empty. Same
    // reasoning as EnvelopeModel::setState() clearing it after a preset load.
    envelopeUndoManager.clearUndoHistory();

    // Resolve every parameter pointer once, so processBlock never looks a
    // parameter up by string. These stay valid for the processor's lifetime.
    masterGainParam = apvts.getRawParameterValue(ParamIDs::subGain);
    masterTuneParam = apvts.getRawParameterValue(ParamIDs::masterTune);
    keyRootParam    = apvts.getRawParameterValue(ParamIDs::masterKeyRoot);
    jassert(masterGainParam != nullptr && masterTuneParam != nullptr && keyRootParam != nullptr);

    for (int i = 0; i < getNumLayers(); ++i)
    {
        auto& cached = layerParams[(size_t) i];
        cached.type  = apvts.getRawParameterValue(ParamIDs::layerType(i));
        cached.level = apvts.getRawParameterValue(ParamIDs::layerLevel(i));
        cached.mute  = apvts.getRawParameterValue(ParamIDs::layerMute(i));
        cached.solo  = apvts.getRawParameterValue(ParamIDs::layerSolo(i));

        cached.clickType  = apvts.getRawParameterValue(ParamIDs::layerClickType(i));
        cached.clickTone  = apvts.getRawParameterValue(ParamIDs::layerClickTone(i));
        cached.clickDecay = apvts.getRawParameterValue(ParamIDs::layerClickDecay(i));

        cached.subSustain    = apvts.getRawParameterValue(ParamIDs::layerSubSustain(i));
        cached.subHarmGain   = apvts.getRawParameterValue(ParamIDs::layerSubHarmGain(i));
        cached.subHarmRatio  = apvts.getRawParameterValue(ParamIDs::layerSubHarmRatio(i));
        cached.subHarmDecay  = apvts.getRawParameterValue(ParamIDs::layerSubHarmDecay(i));
        cached.subHarmPreset = apvts.getRawParameterValue(ParamIDs::layerSubHarmPreset(i));
        cached.subPhase      = apvts.getRawParameterValue(ParamIDs::layerSubPhase(i));
        jassert(cached.subPhase != nullptr);
        jassert(cached.subSustain != nullptr && cached.subHarmGain != nullptr
                 && cached.subHarmRatio != nullptr && cached.subHarmDecay != nullptr
                 && cached.subHarmPreset != nullptr);

        // A null here means an ID in ParamIDs disagrees with the one
        // createParameterLayout() registered — the mixer would then silently
        // read nothing for that control.
        jassert(cached.type != nullptr && cached.level != nullptr
                 && cached.mute != nullptr && cached.solo != nullptr);
        jassert(cached.clickType != nullptr && cached.clickTone != nullptr
                 && cached.clickDecay != nullptr);

        cached.route = apvts.getRawParameterValue(ParamIDs::layerRoute(i));
        jassert(cached.route != nullptr);
    }

    oversamplingParam = apvts.getRawParameterValue(ParamIDs::fxOversampling);
    jassert(oversamplingParam != nullptr);

    for (int i = 0; i < 2; ++i)
    {
        insertLevelParams[(size_t) i] = apvts.getRawParameterValue(ParamIDs::fxInsertLevel(i));
        jassert(insertLevelParams[(size_t) i] != nullptr);
    }

    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        macroParams[(size_t) m] = apvts.getRawParameterValue(ParamIDs::fxMacro(m));
        jassert(macroParams[(size_t) m] != nullptr);
    }

    for (int slot = 0; slot < FX::kNumSlots; ++slot)
    {
        auto& cached = slotParams[(size_t) slot];
        cached.type = apvts.getRawParameterValue(ParamIDs::fxSlotType(slot));
        cached.on   = apvts.getRawParameterValue(ParamIDs::fxSlotOn(slot));
        cached.mix  = apvts.getRawParameterValue(ParamIDs::fxSlotMix(slot));
        jassert(cached.type != nullptr && cached.on != nullptr && cached.mix != nullptr);

        for (int k = 0; k < FX::kNumSlotParams; ++k)
        {
            cached.values[(size_t) k] = apvts.getRawParameterValue(ParamIDs::fxSlotParam(slot, k));
            jassert(cached.values[(size_t) k] != nullptr);
        }
    }

    // Records the slots' types so prepareToPlay builds them; no module exists before a sample rate.
    updateFXModules();
    startTimerHz(kFXPollHz);
}

// Snapshot of one layer's mixer flags for the audibility rule. Off is compared
// against the LayerType index rather than a bare integer so the mapping stays
// tied to the enum; the bools use a 0.5 midpoint, which is how JUCE's own
// AudioParameterBool converts its normalised value back to a boolean.
LayerAudibility::LayerFlags DOOFAudioProcessor::getLayerFlags(int index) const
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));
    const auto& cached = layerParams[(size_t) index];

    LayerAudibility::LayerFlags flags;
    flags.isOff = ((int) cached.type->load() == (int) ParamIDs::LayerType::off);
    flags.mute  = (cached.mute->load() > 0.5f);
    flags.solo  = (cached.solo->load() > 0.5f);
    return flags;
}

// Gathers every layer's flags so the multi-solo question can be answered across
// the whole mix (§3.3), not one layer at a time.
bool DOOFAudioProcessor::isAnyLayerSoloed() const
{
    std::array<LayerAudibility::LayerFlags, LayerAudibility::kNumLayers> flags;
    for (int i = 0; i < getNumLayers(); ++i)
        flags[(size_t) i] = getLayerFlags(i);

    return LayerAudibility::anyLayerSoloed(flags);
}

float DOOFAudioProcessor::getTargetGain(int index, bool anySoloed) const
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));

    if (! LayerAudibility::isAudible(getLayerFlags(index), anySoloed))
        return 0.0f;

    return layerParams[(size_t) index].level->load();
}

DOOFAudioProcessor::~DOOFAudioProcessor()
{
    stopTimer();
}

// Bounds-checked in debug; index must come from a loop over getNumLayers() or
// a validated selection, never straight from untrusted state.
Layer& DOOFAudioProcessor::getLayer(int index)
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));
    return *layers[(size_t) index];
}

const Layer& DOOFAudioProcessor::getLayer(int index) const
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));
    return *layers[(size_t) index];
}

// Beside getLayer, since the two are looked up together: the editor asks for a layer's models
// and how that layer was last being viewed at the same moment.
LayerViewPrefs& DOOFAudioProcessor::getLayerViewPrefs(int index)
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));
    return layerViewPrefs[(size_t) index];
}

const LayerViewPrefs& DOOFAudioProcessor::getLayerViewPrefs(int index) const
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));
    return layerViewPrefs[(size_t) index];
}

// Build the initial parameter layout for the APVTS.
// Called once from the constructor initialiser list; adding a parameter here
// registers it for host automation and preset save/restore automatically.
juce::AudioProcessorValueTreeState::ParameterLayout
DOOFAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // sub.gain — linear master gain applied after the layer mix, range [0, 1].
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { ParamIDs::subGain, 1 },
        "Sub Gain",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        0.8f // default: 80% level
    ));

    // Master tuning and key root (Phase 6, §3.5). Both global: they describe the patch as a whole,
    // and every Sub layer reads the same pair. Defaults change nothing - 0 cents, and a root of C3
    // only matters once a node is flagged keytracked, which no factory node is.
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { ParamIDs::masterTune, 1 },
        "Master Tune",
        juce::NormalisableRange<float>(-ParamIDs::masterTuneMaxCents, ParamIDs::masterTuneMaxCents),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel("cents")
    ));

    layout.add(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID { ParamIDs::masterKeyRoot, 1 },
        "Key Root",
        0, 127,
        ParamIDs::keyRootDefault,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction([](int note, int) { return ParamIDs::noteName(note); })
            // Typed text that is not a note keeps the default rather than becoming MIDI 0.
            .withValueFromStringFunction([](const juce::String& text)
            {
                const int note = PitchSnap::noteFromName(text);
                return note >= 0 ? note : ParamIDs::keyRootDefault;
            })
    ));

    // Per-layer mixer parameters (§3.2, §3.3): type, level, mute, solo.
    //
    // Defaults are chosen so the out-of-the-box patch is exactly what it was
    // before the mixer existed: layer 1 is a Sub at unity level, layers 2-5
    // are Off, and the 0.8 master above is still the only gain in the path.
    // That keeps every Phase 1/2/3 render assertion valid unchanged.
    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
    {
        const auto layerNumber = juce::String(i + 1);
        const auto defaultType = (i == 0) ? ParamIDs::LayerType::sub : ParamIDs::LayerType::off;

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { ParamIDs::layerType(i), 1 },
            "Layer " + layerNumber + " Type",
            ParamIDs::layerTypeChoices(),
            (int) defaultType
        ));

        // Linear, matching sub.gain's units. A dB-scaled display is a
        // skinning concern (Phase 12), not a change to the stored value.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerLevel(i), 1 },
            "Layer " + layerNumber + " Level",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            1.0f
        ));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { ParamIDs::layerMute(i), 1 },
            "Layer " + layerNumber + " Mute",
            false
        ));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { ParamIDs::layerSolo(i), 1 },
            "Layer " + layerNumber + " Solo",
            false
        ));

        // Click controls (§6 Phase 5: "per-click level/tone"). Level is the layer's existing
        // level parameter — a click does not get a second one — so what a Click layer adds is
        // which click, how bright, and how long.
        //
        // Registered for every layer regardless of its type, exactly as the envelope models are
        // seeded for every layer: switching a layer to Click must sound immediately rather than
        // waiting for a parameter that only exists once the type has changed.
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { ParamIDs::layerClickType(i), 1 },
            "Layer " + layerNumber + " Click Type",
            ParamIDs::clickTypeChoices(),
            (int) ParamIDs::ClickType::tick
        ));

        // Normalised rather than expressed in Hz: tone means a different thing per click type
        // (a filter corner for tick/noise/snap, a blip frequency for thump), so there is no one
        // honest unit to give it. ClickVoice maps it exponentially internally, which is the log
        // skew §0.5 asks for on frequency controls.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerClickTone(i), 1 },
            "Layer " + layerNumber + " Click Tone",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.5f
        ));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerClickDecay(i), 1 },
            "Layer " + layerNumber + " Click Decay",
            clickDecayRange(),
            ParamIDs::clickDecayDefaultSeconds
        ));

        // Sub controls (Phase 6). Off by default, which is exactly the behaviour before sustain
        // existed: a note-off changes nothing.
        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { ParamIDs::layerSubSustain(i), 1 },
            "Layer " + layerNumber + " Sustain",
            false
        ));

        // Harmonics. Gain defaults to 0 - harmonics off - so the factory patch is the clean sine
        // it always was; the other three only matter once gain is raised.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerSubHarmGain(i), 1 },
            "Layer " + layerNumber + " Harmonics Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.0f
        ));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerSubHarmRatio(i), 1 },
            "Layer " + layerNumber + " Harmonics Ratio",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.5f
        ));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerSubHarmDecay(i), 1 },
            "Layer " + layerNumber + " Harmonics Decay",
            harmDecayRange(),
            ParamIDs::subHarmDecayDefaultSeconds
        ));

        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { ParamIDs::layerSubHarmPreset(i), 1 },
            "Layer " + layerNumber + " Harmonics Preset",
            ParamIDs::harmPresetChoices(),
            (int) SubVoice::HarmonicSet::all
        ));

        // 0 degrees is the sin(0) start every note has always had.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::layerSubPhase(i), 1 },
            "Layer " + layerNumber + " Phase",
            juce::NormalisableRange<float>(0.0f, 360.0f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel("deg")
        ));
    }

    // FX (Phase 7, §7.1). Appended after everything that existed before, so no earlier parameter
    // moves in a host's list. Every default leaves the sound exactly as it was before FX existed:
    // each layer routed Dry, every slot empty, insert levels at unity, macros at zero.
    for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { ParamIDs::layerRoute(i), 1 },
            "Layer " + juce::String(i + 1) + " Route",
            FX::routeChoices(),
            (int) FX::Route::dry
        ));
    }

    // 2x by default: it removes most of the audible aliasing a distorted kick produces, for a
    // latency of a few samples - and only once a nonlinear module is actually in a slot.
    layout.add(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID { ParamIDs::fxOversampling, 1 },
        "FX Oversampling",
        FX::oversamplingChoices(),
        1
    ));

    for (int i = 0; i < 2; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::fxInsertLevel(i), 1 },
            i == 0 ? "Insert A Level" : "Insert B Level",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            1.0f
        ));
    }

    for (int m = 0; m < FX::kNumMacros; ++m)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::fxMacro(m), 1 },
            "Macro " + juce::String(m + 1),
            juce::NormalisableRange<float>(0.0f, 1.0f),
            0.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(percentText)
                                                 .withValueFromStringFunction(percentFromText)
        ));
    }

    for (int slot = 0; slot < FX::kNumSlots; ++slot)
    {
        const auto name = ParamIDs::fxSlotName(slot);

        auto type = std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { ParamIDs::fxSlotType(slot), 1 },
            name + " Type",
            FX::moduleTypeChoices(),
            (int) FX::ModuleType::none
        );
        const auto& typeParam = *type; // outlives the controls below: the APVTS owns both
        layout.add(std::move(type));

        layout.add(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { ParamIDs::fxSlotOn(slot), 1 },
            name + " On",
            true
        ));

        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { ParamIDs::fxSlotMix(slot), 1 },
            name + " Mix",
            juce::NormalisableRange<float>(0.0f, 1.0f),
            1.0f,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction(percentText)
                                                 .withValueFromStringFunction(percentFromText)
        ));

        for (int k = 0; k < FX::kNumSlotParams; ++k)
            layout.add(std::make_unique<FXSlotParameter>(ParamIDs::fxSlotParam(slot, k),
                                                         name + " P" + juce::String(k + 1), typeParam, k));
    }

    return layout;
}

// Skewed so the default 8 ms sits at the centre of the control's travel. §0.5 settles on a log
// skew for time parameters, and without it the 1-20 ms range §6 actually cares about would be
// crushed into the bottom third of the knob while the rarely-wanted long end took the rest.
// Skewed around the default for the same reason as the click decay.
juce::NormalisableRange<float> DOOFAudioProcessor::harmDecayRange()
{
    juce::NormalisableRange<float> range { ParamIDs::subHarmDecayMinSeconds,
                                           ParamIDs::subHarmDecayMaxSeconds };
    range.setSkewForCentre(ParamIDs::subHarmDecayDefaultSeconds);
    return range;
}

juce::NormalisableRange<float> DOOFAudioProcessor::clickDecayRange()
{
    juce::NormalisableRange<float> range { ParamIDs::clickDecayMinSeconds,
                                           ParamIDs::clickDecayMaxSeconds };
    range.setSkewForCentre(ParamIDs::clickDecayDefaultSeconds);
    return range;
}

// Prepare the voice and DC blocker for the upcoming playback session.
// The DC blocker coefficient is computed here so it adapts to any sample rate.
void DOOFAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    // Every layer's voice is prepared, including Off ones — a layer switched on
    // mid-session must be ready to sound without waiting for another
    // prepareToPlay call from the host.
    for (int i = 0; i < getNumLayers(); ++i)
    {
        getLayer(i).subVoice.prepare(sampleRate);
        getLayer(i).clickVoice.prepare(sampleRate);
    }

    // Every layer starts on its sub voice; the first processBlock resolves the real answer from
    // the type parameter before anything is rendered.
    layerSource.fill(LayerSource::sub);

    // Each layer's gain starts *at* its target rather than ramping up to it, so
    // opening a session doesn't fade the first note in from silence. Only later
    // changes ramp.
    const bool anySoloed = isAnyLayerSoloed();
    for (int i = 0; i < getNumLayers(); ++i)
    {
        layerGain[(size_t) i].reset(sampleRate, kGainRampSeconds);
        layerGain[(size_t) i].setCurrentAndTargetValue(getTargetGain(i, anySoloed));
    }

    // DC blocker: y[n] = x[n] - x[n-1] + R * y[n-1]
    // R = 1 - 2π * fc / sr  where fc ≈ 10 Hz removes any synthesis DC offset.
    dcBlockerR = 1.0f - static_cast<float>(
        juce::MathConstants<double>::twoPi * 10.0 / sampleRate);
    dcBlockerX.fill(0.0f);
    dcBlockerY.fill(0.0f);

    // The FX section at this rate. Audio is stopped, so the slots' modules are installed directly
    // rather than crossfaded in, and the first block already sounds as the parameters say.
    // processBlock renders in chunks of at most this size, so a host that exceeds the size it
    // announced is still handled.
    fx.prepare(sampleRate, juce::jmax(1, samplesPerBlock));
    std::array<int, FX::kNumSlots> types {};
    for (int slot = 0; slot < FX::kNumSlots; ++slot)
        types[(size_t) slot] = (int) slotParams[(size_t) slot].type->load();
    fx.setModuleTypes(types, (int) oversamplingParam->load(), true);
    setLatencySamples(fx.getLatencySamples());
}

// Free any resources allocated in prepareToPlay.
// Phase 1 has no heap-allocated DSP resources; this grows as later phases add them.
void DOOFAudioProcessor::releaseResources() {}

// Validate that the host's proposed channel layout is one DOOF supports.
// Rejects anything other than stereo out; allows the sidechain to be mono or absent.
bool DOOFAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Sidechain must be mono or disabled — no stereo sidechain support.
    if (!layouts.inputBuses.isEmpty())
    {
        const auto& sc = layouts.inputBuses[0];
        if (!sc.isDisabled() && sc != juce::AudioChannelSet::mono())
            return false;
    }

    return true;
}

// Audio processing callback — called by the host once per buffer on the audio thread.
// ScopedNoDenormals suppresses denormal CPU spikes in long tails and reverb decays.
//
// Processing order:
//   1. Resolve each layer's target gain for this block (type, level, mute/solo).
//   2. Walk the MIDI buffer in order, rendering up to each event's sample position
//      before applying it, then render whatever is left after the last event.
//
// MIDI dispatch is sample-accurate: an event at position N is applied after exactly N samples of
// this block have been rendered, not at the start of the block. Until Phase 5 every note-on in a
// block fired at sample 0, which on a 512-sample buffer is up to 11 ms early — inaudible on the
// sub's slow attack, but plainly audible timing slop on a click transient, and enough to make
// §6's "assert the click onset is sample-aligned to note-on" unpassable.
//
// Real-time safety guarantee: no allocations, no locks, no file/network I/O in this function
// or any function it calls.  Verify with AddressSanitizer's alloc hooks when running headless.
void DOOFAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int chunkSize  = fx.getMaxBlockSize();

    // Before prepareToPlay there is nothing to render with; silence rather than a crash.
    if (chunkSize <= 0)
    {
        buffer.clear();
        return;
    }

    // Step 1 — mixer state for this block.
    //
    // anySoloed is evaluated once across every layer before any individual
    // layer's audibility is decided: §3.3's rule is about the mix as a whole,
    // so asking per-layer mid-loop could see it change halfway through.
    const bool anySoloed = isAnyLayerSoloed();

    // Read once for the block; each voice latches them at its next note-on.
    const int   keyRoot   = (int) keyRootParam->load();
    const float tuneCents = masterTuneParam->load();

    for (int i = 0; i < getNumLayers(); ++i)
    {
        auto& layer = getLayer(i);

        // Load each snapshot once for the whole block (§2: the audio thread
        // loads the atomic pointer once per block, never per-sample).
        layer.subVoice.setSnapshot(layer.publisher.getSnapshot());
        layer.subVoice.setTuning(keyRoot, tuneCents);
        const auto& cached = layerParams[(size_t) i];
        layer.subVoice.setSustain(cached.subSustain->load() > 0.5f);
        layer.subVoice.setPhaseRotation(cached.subPhase->load());
        layer.subVoice.setHarmonics(cached.subHarmGain->load(), cached.subHarmRatio->load(),
                                    (double) cached.subHarmDecay->load(),
                                    (SubVoice::HarmonicSet) juce::jlimit(0, SubVoice::kNumHarmonicSets - 1,
                                                                         (int) cached.subHarmPreset->load()));

        // Click controls likewise: read once here, latched by ClickVoice at the next note-on.
        layerSource[(size_t) i] = configureLayerSource(i);

        // Ramped, not assigned — see kGainRampSeconds.
        layerGain[(size_t) i].setTargetValue(getTargetGain(i, anySoloed));

        layerRoute[(size_t) i] = (FX::Route) juce::jlimit(0, 3, (int) cached.route->load());
    }

    readFXParams();

    // The master gain is loaded once for the whole block rather than per chunk or sub-range, so
    // splitting the loop can't change how often it is sampled.
    const float gain = masterGainParam->load();

    // Step 2 — render the block, split at every MIDI event, in chunks no longer than the FX
    // buses. A chunk is the whole block unless the host sends more than it announced.
    //
    // `rendered` is the absolute position reached so far; also the start of the next sub-range.
    int rendered = 0;
    auto event = midiMessages.cbegin();
    const auto lastEvent = midiMessages.cend();

    auto* left  = buffer.getWritePointer(0);
    auto* right = buffer.getWritePointer(1);

    for (int chunkStart = 0; chunkStart < numSamples; chunkStart += chunkSize)
    {
        const int chunkEnd = juce::jmin(numSamples, chunkStart + chunkSize);
        const bool isLastChunk = (chunkEnd == numSamples);
        fx.clearBuses(chunkEnd - chunkStart);

        // Note-ons trigger; note-offs matter only to a Sub layer in sustain mode, which loops while
        // the key is held (Phase 6). Without sustain the amp envelope alone decides how long a kick
        // lasts, so a note-off is passed on and then changes nothing.
        for (; event != lastEvent; ++event)
        {
            const auto metadata = *event;

            // Clamped rather than trusted. MidiBuffer iterates in ascending sample position and the
            // host should keep events inside the block, but an out-of-range or out-of-order position
            // would otherwise rewind the cursor and re-render samples already written — advancing the
            // voices twice over the same span.
            const int eventPos = juce::jlimit(rendered, numSamples, metadata.samplePosition);

            // An event at or past this chunk's end belongs to a later chunk - except in the last,
            // which takes everything left, including events the host placed past the block's end.
            if (eventPos >= chunkEnd && ! isLastChunk)
                break;

            const auto msg = metadata.getMessage();
            if (! msg.isNoteOn() && ! msg.isNoteOff())
                continue;

            renderLayers(rendered - chunkStart, eventPos - rendered);
            rendered = eventPos;

            // Note-ons go to both of every layer's voices, including Off ones and including whichever
            // voice the layer's type is not currently selecting. A layer switched on — or switched
            // between Sub and Click — part-way through a note then plays from the right point rather
            // than restarting, and triggering an unselected voice costs nothing since its output is
            // never read.
            //
            // Note-offs go to the sub voices only; a click has no held state to release. Both kinds
            // land at their own sample, so releasing a sustained note is as sample-accurate as
            // starting one.
            for (int i = 0; i < getNumLayers(); ++i)
            {
                if (msg.isNoteOn())
                {
                    getLayer(i).subVoice.noteOn(msg.getNoteNumber());
                    getLayer(i).clickVoice.noteOn(msg.getNoteNumber());
                }
                else
                {
                    getLayer(i).subVoice.noteOff(msg.getNoteNumber());
                }
            }
        }

        // Everything after the chunk's last event — the whole chunk when there was none.
        renderLayers(rendered - chunkStart, chunkEnd - rendered);
        rendered = chunkEnd;

        // Step 3 — the FX section, then the output stage.
        const int chunkLength = chunkEnd - chunkStart;
        fx.process(left + chunkStart, right + chunkStart, chunkLength, fxParams);
        applyOutputStage(left + chunkStart, right + chunkStart, chunkLength, gain);
    }
}

void DOOFAudioProcessor::readFXParams()
{
    for (int slot = 0; slot < FX::kNumSlots; ++slot)
    {
        const auto& cached = slotParams[(size_t) slot];
        auto& params = fxParams.slots[(size_t) slot];

        params.type = (int) cached.type->load();
        params.on   = cached.on->load() > 0.5f;
        params.mix  = cached.mix->load();
        for (int k = 0; k < FX::kNumSlotParams; ++k)
            params.values[(size_t) k] = cached.values[(size_t) k]->load();
    }

    for (int i = 0; i < 2; ++i)
        fxParams.insertLevel[(size_t) i] = insertLevelParams[(size_t) i]->load();

    for (int m = 0; m < FX::kNumMacros; ++m)
        fxParams.macros[(size_t) m] = macroParams[(size_t) m]->load();

    fxParams.macroSnapshot = macroAssignments.getSnapshot();

    // The host's tempo, for synced delays. Kept at the last known value when a block has none.
    if (auto* playHead = getPlayHead())
        if (const auto position = playHead->getPosition())
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
                fxParams.context.bpm = *bpm;
}

// Pushes one layer's click parameters into its ClickVoice and answers which voice the layer is
// mixed from this block.
//
// Called once per layer per block, on the audio thread: plain atomic loads and stores into the
// voice, no strings and no allocation. ClickVoice latches these at its next note-on rather than
// reading them per sample, so setting them at block rate is exactly the right granularity — a hit
// already in flight is never reshaped underneath itself.
DOOFAudioProcessor::LayerSource DOOFAudioProcessor::configureLayerSource(int index)
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));

    const auto& cached = layerParams[(size_t) index];
    auto& layer = getLayer(index);

    layer.clickVoice.setTone(cached.clickTone->load());
    layer.clickVoice.setDecaySeconds((double) cached.clickDecay->load());

    // Compared against the LayerType index rather than a bare integer, so the mapping stays tied
    // to the enum — same reasoning as getLayerFlags.
    if ((int) cached.type->load() != (int) ParamIDs::LayerType::click)
        return LayerSource::sub;

    const int slot = (int) cached.clickType->load();
    layer.clickVoice.setSlot(slot);

    // The first four choice entries are ClickVoice::Type values, guaranteed by the static_asserts
    // in ParamIDs.h; the remaining four are sample slots, which may or may not have content behind
    // them in this build. The voice is asked rather than the index inspected, so that an empty slot
    // is silent instead of falling back to a synthesised type - a preset saved against a slot then
    // sounds the same whether or not the build it is loaded into happens to ship that sample.
    return layer.clickVoice.hasContentFor(slot) ? LayerSource::click : LayerSource::silent;
}

// Renders one contiguous span of the block into the FX buses, starting busOffset samples into
// them. Called once per gap between MIDI events, so a zero-length span (two events at the same
// position, or an event at sample 0) is a legitimate no-op rather than an error.
//
// Layers are summed straight, with no division by the number of active layers: normalising would
// change the sound of every other layer whenever one was enabled. Five layers at unity can
// therefore exceed full scale, which is expected — taming that is the Phase 9 master limiter's
// job, and nothing here clamps or wraps.
//
// Each layer's sample is computed once and added to its route's bus - to both insert buses for
// "Both" (parallel at full level, user decision 2026-11-17). A layer routed Dry is added to the dry
// bus in exactly the order and arithmetic the pre-FX mix used, which is what keeps the default
// patch bit-identical to Phase 6.
//
// Audio-thread only, and bound by the same real-time rules as processBlock itself.
void DOOFAudioProcessor::renderLayers(int busOffset, int numSamples)
{
    jassert(busOffset >= 0 && numSamples >= 0 && busOffset + numSamples <= fx.getMaxBlockSize());

    float* dryBus = fx.getBus(FX::Bus::dry);
    float* busA   = fx.getBus(FX::Bus::insertA);
    float* busB   = fx.getBus(FX::Bus::insertB);

    for (int i = busOffset; i < busOffset + numSamples; ++i)
    {
        // Every layer is ticked every sample, even silent ones, and never
        // skipped. Skipping would freeze that layer's envelope time, so
        // unmuting mid-note would resume from a stale position instead of where
        // the note actually is; it would also bypass the gain ramp and turn
        // switching a layer Off mid-tail into an instant cut. Idle voices
        // already cost almost nothing.
        //
        // The same argument applies to a layer's two voices: both are advanced, and only the one
        // the type selects is mixed. Ticking just the selected voice would freeze the other's
        // internal time, so switching a layer between Sub and Click mid-note would resume the
        // newly-selected voice from wherever the last switch left it.
        for (int layerIndex = 0; layerIndex < getNumLayers(); ++layerIndex)
        {
            auto& layer = getLayer(layerIndex);

            const float sub   = layer.subVoice.processSample();
            const float click = layer.clickVoice.processSample();

            float selected = 0.0f;
            switch (layerSource[(size_t) layerIndex])
            {
                case LayerSource::sub:    selected = sub;   break;
                case LayerSource::click:  selected = click; break;
                case LayerSource::silent:                   break;
            }

            const float contribution = selected * layerGain[(size_t) layerIndex].getNextValue();

            switch (layerRoute[(size_t) layerIndex])
            {
                case FX::Route::dry:     dryBus[i] += contribution; break;
                case FX::Route::insertA: busA[i]   += contribution; break;
                case FX::Route::insertB: busB[i]   += contribution; break;
                case FX::Route::both:    busA[i]   += contribution;
                                         busB[i]   += contribution; break;
            }
        }
    }
}

// Master gain, then the DC blocker, per channel. The blocker is last of all so that DC from an
// asymmetric distortion anywhere in the FX is removed with the synthesis's own.
void DOOFAudioProcessor::applyOutputStage(float* left, float* right, int numSamples, float gain)
{
    float* channels[] { left, right };

    for (size_t ch = 0; ch < 2; ++ch)
    {
        float* data = channels[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = data[i] * gain;
            const float blocked = sample - dcBlockerX[ch] + dcBlockerR * dcBlockerY[ch];
            dcBlockerX[ch] = sample;
            dcBlockerY[ch] = blocked;
            data[i] = blocked;
        }
    }
}

// Instantiates and returns the GUI editor; the host owns the returned pointer.
juce::AudioProcessorEditor* DOOFAudioProcessor::createEditor()
{
    return new DOOFAudioProcessorEditor(*this);
}

// Serialises APVTS state plus both envelope models' node data to XML and
// packs it into destData for host project save (and, via getStateAsMemoryBlock/
// setStateFromMemoryBlock-style reuse, the .doof preset file — see 7b).
// apvts.copyState() already returns an independent copy safe to reparent;
// the envelope models' own getValueTree() is their live tree, so those are
// explicitly copied first — adding a live tree as a child here would steal
// its parent link and corrupt the model still using it.
void DOOFAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree root { PresetIDs::rootType };

    // Written explicitly so a reader never has to infer the layout from which children happen to
    // be present, which is what the Phase 3 fallback would otherwise have to guess at.
    root.setProperty(PresetIDs::versionProp, PresetIDs::currentVersion, nullptr);

    root.addChild(apvts.copyState(), -1, nullptr);

    // index is stored rather than relied on positionally, so a future layer count change can't
    // silently shift every layer's envelopes onto the wrong layer.
    for (int i = 0; i < getNumLayers(); ++i)
    {
        juce::ValueTree layerNode { PresetIDs::layerNodeType };
        layerNode.setProperty(PresetIDs::indexProp, i, nullptr);

        const auto& prefs = layerViewPrefs[(size_t) i];
        layerNode.setProperty(PresetIDs::logScaleProp,     prefs.pitchLogScale, nullptr);
        layerNode.setProperty(PresetIDs::editingPitchProp, prefs.editingPitch,  nullptr);
        layerNode.setProperty(PresetIDs::pitchSnapProp,    prefs.pitchSnap,     nullptr);

        auto pitchCopy = getLayer(i).pitchModel.getValueTree().createCopy();
        pitchCopy.setProperty(PresetIDs::curveProp, PresetIDs::pitchCurveTag, nullptr);
        layerNode.addChild(pitchCopy, -1, nullptr);

        auto ampCopy = getLayer(i).ampModel.getValueTree().createCopy();
        ampCopy.setProperty(PresetIDs::curveProp, PresetIDs::ampCurveTag, nullptr);
        layerNode.addChild(ampCopy, -1, nullptr);

        root.addChild(layerNode, -1, nullptr);
    }

    root.addChild(macroAssignments.getState(), -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml(root.createXml());
    copyXmlToBinary(*xml, destData);
}

// Restores APVTS and both envelope models from a blob previously written by
// getStateInformation. Guards against malformed/older blobs at each step —
// a blob missing the envelope data (e.g. a pre-Phase-3 save) still restores
// whatever it does have instead of failing entirely.
void DOOFAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr)
        return;

    const auto root = juce::ValueTree::fromXml(*xml);
    if (! root.hasType(PresetIDs::rootType))
        return;

    // No version means Phase 3's flat layout: two ENVELOPE trees on the root, describing layer 1.
    const bool isLayered = root.hasProperty(PresetIDs::versionProp);

    // The mixer parameters need no special handling for a pre-mixer preset: replaceState appends
    // a bare PARAM child for every parameter the loaded tree omits, and reading that child back
    // falls through to the parameter's default. Verified against JUCE rather than assumed, and
    // pinned by Phase4StateTest (b) since a pre-mixer preset loading correctly depends on it.
    const auto apvtsState = root.getChildWithName(apvts.state.getType());
    if (apvtsState.isValid())
        apvts.replaceState(apvtsState);

    if (isLayered)
    {
        for (const auto layerNode : root)
        {
            if (! layerNode.hasType(PresetIDs::layerNodeType))
                continue;

            const int index = layerNode.getProperty(PresetIDs::indexProp, -1);
            if (! juce::isPositiveAndBelow(index, getNumLayers()))
                continue;

            loadEnvelopesInto(index, layerNode);

            // Defaulted rather than branched on the version: a version 2 file carries neither
            // property, and the defaults are exactly what it meant.
            const LayerViewPrefs fallback;
            auto& prefs = layerViewPrefs[(size_t) index];
            prefs.pitchLogScale = layerNode.getProperty(PresetIDs::logScaleProp,     fallback.pitchLogScale);
            prefs.editingPitch  = layerNode.getProperty(PresetIDs::editingPitchProp, fallback.editingPitch);
            prefs.pitchSnap     = layerNode.getProperty(PresetIDs::pitchSnapProp,    fallback.pitchSnap);
        }
    }
    else
    {
        loadEnvelopesInto(0, root);

        // The preset said nothing about these, so they go back to factory alongside their
        // parameters above rather than keeping the outgoing patch's curves.
        for (int i = 1; i < getNumLayers(); ++i)
            seedLayerWithDefaults(i);

        // Nor about how any layer was being viewed, so every layer's view goes back to factory
        // too - including layer 1, whose envelopes the file did describe.
        layerViewPrefs.fill(LayerViewPrefs{});
    }

    // A file older than version 5 has no MACROS, which reads as an invalid tree and clears every
    // assignment - correct, since it had none.
    macroAssignments.setState(root.getChildWithName(MacroAssignments::stateType));

    // A loaded preset is not a user edit to undo back from, and the re-seeding above would
    // otherwise leave its own transactions sitting at the top of the history.
    envelopeUndoManager.clearUndoHistory();

    // The slot types may have changed. Built now when this is the message thread; otherwise the
    // FX poll does it within a frame.
    if (juce::MessageManager::existsAndIsCurrentThread())
        updateFXModules();
}

// Reads the curve-tagged ENVELOPE children of `parent` into one layer's models. Shared by both
// load paths, which differ only in which tree the envelopes hang off.
void DOOFAudioProcessor::loadEnvelopesInto(int index, const juce::ValueTree& parent)
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));

    const auto pitchState = parent.getChildWithProperty(PresetIDs::curveProp, PresetIDs::pitchCurveTag);
    if (pitchState.isValid())
        getLayer(index).pitchModel.setState(pitchState);

    const auto ampState = parent.getChildWithProperty(PresetIDs::curveProp, PresetIDs::ampCurveTag);
    if (ampState.isValid())
        getLayer(index).ampModel.setState(ampState);
}

// Puts one layer's envelopes back to the factory shapes. setState with an empty tree clears the
// nodes and every property, so a stale Length can't survive into the reseeded envelope.
void DOOFAudioProcessor::seedLayerWithDefaults(int index)
{
    jassert(juce::isPositiveAndBelow(index, getNumLayers()));

    auto& layer = getLayer(index);
    layer.pitchModel.setState(juce::ValueTree(EnvelopeIDs::envelopeType));
    layer.ampModel.setState(juce::ValueTree(EnvelopeIDs::envelopeType));

    DefaultEnvelopes::seedPitch(layer.pitchModel);
    DefaultEnvelopes::seedAmp(layer.ampModel);
}

// ── FX (Phase 7) ─────────────────────────────────────────────────────────────

void DOOFAudioProcessor::timerCallback()
{
    updateFXModules();
}

void DOOFAudioProcessor::updateFXModules()
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());

    std::array<int, FX::kNumSlots> types {};
    for (int slot = 0; slot < FX::kNumSlots; ++slot)
        types[(size_t) slot] = (int) slotParams[(size_t) slot].type->load();

    fx.setModuleTypes(types, (int) oversamplingParam->load(), false);

    // The engine's latency changes once the audio thread has actually swapped a module in, which
    // is a block or so after the offer above - so this catches up on the following poll.
    if (const int latency = fx.getLatencySamples(); latency != getLatencySamples())
        setLatencySamples(latency);

    // Tail: the longer insert's, plus Master's, since the master chain hears both.
    double channelTail[FX::kNumChannels] {};
    for (int slot = 0; slot < FX::kNumSlots; ++slot)
    {
        FX::SlotValues values {};
        for (int k = 0; k < FX::kNumSlotParams; ++k)
            values[(size_t) k] = slotParams[(size_t) slot].values[(size_t) k]->load();

        channelTail[(int) FX::channelOf(slot)] += FX::tailSeconds((FX::ModuleType) types[(size_t) slot], values,
                                                                  fxParams.context.bpm);
    }

    fxTailSeconds.store(juce::jmax(channelTail[0], channelTail[1]) + channelTail[2], std::memory_order_relaxed);
}

void DOOFAudioProcessor::setFXSlotModule(int slot, FX::ModuleType type)
{
    jassert(juce::isPositiveAndBelow(slot, FX::kNumSlots));

    auto set = [this](const juce::String& id, float realValue)
    {
        if (auto* param = apvts.getParameter(id))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(param->convertTo0to1(realValue));
            param->endChangeGesture();
        }
    };

    const auto& info = FX::getInfo(type);

    // The new module is only built by updateFXModules below, once every control is in place, so it
    // never runs a block against the outgoing module's settings.
    set(ParamIDs::fxSlotOn(slot), 1.0f);
    set(ParamIDs::fxSlotMix(slot), info.defaultMix);
    for (int k = 0; k < FX::kNumSlotParams; ++k)
        set(ParamIDs::fxSlotParam(slot, k), FX::defaultNormalised(info.params[(size_t) k]));
    set(ParamIDs::fxSlotType(slot), (float) (int) type);

    macroAssignments.clearSlot(slot);
    updateFXModules();
}

void DOOFAudioProcessor::swapFXSlots(int slotA, int slotB)
{
    if (slotA == slotB || ! juce::isPositiveAndBelow(slotA, FX::kNumSlots) || ! juce::isPositiveAndBelow(slotB, FX::kNumSlots))
        return;

    juce::StringArray suffixes { ".type", ".on", ".mix" };
    for (int k = 0; k < FX::kNumSlotParams; ++k)
        suffixes.add(".p" + juce::String(k + 1));

    for (const auto& suffix : suffixes)
    {
        auto* a = apvts.getParameter(ParamIDs::fxSlotPrefix(slotA) + suffix);
        auto* b = apvts.getParameter(ParamIDs::fxSlotPrefix(slotB) + suffix);
        jassert(a != nullptr && b != nullptr);

        const float valueA = a->getValue();
        const float valueB = b->getValue();

        for (auto [param, value] : { std::pair { a, valueB }, std::pair { b, valueA } })
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost(value);
            param->endChangeGesture();
        }
    }

    macroAssignments.swapSlots(slotA, slotB);
    updateFXModules();
}

// Entry point called by the host to create a new plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DOOFAudioProcessor();
}
