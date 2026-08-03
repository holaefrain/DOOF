#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DefaultEnvelopes.h"
#include "ParamIDs.h"
#include "LayerAudibility.h"

// State-serialisation schema for getStateInformation/setStateInformation and the .doof file:
//
//   DOOFState { version }
//     DOOF_State                                  <- apvts.copyState(), all 21 parameters
//     LAYER { index, logScale, editingPitch }     <- one per layer, five of them
//       ENVELOPE { curve="pitch", ... }
//       ENVELOPE { curve="amp",   ... }
//
// Both envelopes are EnvelopeIDs::envelopeType ("ENVELOPE"), so curveProp distinguishes them.
// Never rename any of these once a preset has shipped - same rule as APVTS parameter IDs (§2).
//
// Versions, oldest first:
//   absent - Phase 3: no version, both ENVELOPE trees straight on the root, describing one layer.
//   2      - Phase 4: LAYER children, five of them. No view preferences.
//   3      - Phase 4: adds logScale/editingPitch to each LAYER.
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
    static const juce::String pitchCurveTag = "pitch";
    static const juce::String ampCurveTag   = "amp";

    // Bumped whenever the layout above changes shape; Phase 5 and Phase 7 will both need it again.
    static constexpr int currentVersion = 3;
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
    jassert(masterGainParam != nullptr);

    for (int i = 0; i < getNumLayers(); ++i)
    {
        auto& cached = layerParams[(size_t) i];
        cached.type  = apvts.getRawParameterValue(ParamIDs::layerType(i));
        cached.level = apvts.getRawParameterValue(ParamIDs::layerLevel(i));
        cached.mute  = apvts.getRawParameterValue(ParamIDs::layerMute(i));
        cached.solo  = apvts.getRawParameterValue(ParamIDs::layerSolo(i));

        // A null here means an ID in ParamIDs disagrees with the one
        // createParameterLayout() registered — the mixer would then silently
        // read nothing for that control.
        jassert(cached.type != nullptr && cached.level != nullptr
                 && cached.mute != nullptr && cached.solo != nullptr);
    }
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

DOOFAudioProcessor::~DOOFAudioProcessor() = default;

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
    }

    return layout;
}

// Prepare the voice and DC blocker for the upcoming playback session.
// The DC blocker coefficient is computed here so it adapts to any sample rate.
void DOOFAudioProcessor::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    // Every layer's voice is prepared, including Off ones — a layer switched on
    // mid-session must be ready to sound without waiting for another
    // prepareToPlay call from the host.
    for (int i = 0; i < getNumLayers(); ++i)
        getLayer(i).voice.prepare(sampleRate);

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
    dcBlockerX = 0.0f;
    dcBlockerY = 0.0f;
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
//   2. Consume all MIDI events (block-accurate; sample-accurate in a later phase).
//   3. For each sample: tick every layer's voice, sum them through their smoothed
//      gains, apply master gain, apply DC blocker, write to L+R.
//
// Real-time safety guarantee: no allocations, no locks, no file/network I/O in this function
// or any function it calls.  Verify with AddressSanitizer's alloc hooks when running headless.
void DOOFAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Step 1 — mixer state for this block.
    //
    // anySoloed is evaluated once across every layer before any individual
    // layer's audibility is decided: §3.3's rule is about the mix as a whole,
    // so asking per-layer mid-loop could see it change halfway through.
    const bool anySoloed = isAnyLayerSoloed();

    for (int i = 0; i < getNumLayers(); ++i)
    {
        auto& layer = getLayer(i);

        // Load each snapshot once for the whole block (§2: the audio thread
        // loads the atomic pointer once per block, never per-sample).
        layer.voice.setSnapshot(layer.publisher.getSnapshot());

        // Ramped, not assigned — see kGainRampSeconds.
        layerGain[(size_t) i].setTargetValue(getTargetGain(i, anySoloed));
    }

    // Step 2 — handle MIDI events.
    // Note-off is intentionally ignored: the amp envelope handles the voice decay.
    //
    // Note-ons go to every layer's voice, including Off ones. A layer switched
    // on part-way through a note then plays from the right point in the
    // envelope rather than restarting, and switching one on between notes costs
    // nothing since its voice is already idle.
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            for (int i = 0; i < getNumLayers(); ++i)
                getLayer(i).voice.noteOn(msg.getNoteNumber());
    }

    // Step 3 — render and mix.
    //
    // Layers are summed straight, with no division by the number of active
    // layers: normalising would change the sound of every other layer whenever
    // one was enabled. Five layers at unity can therefore exceed full scale,
    // which is expected — taming that is the Phase 9 master limiter's job, and
    // nothing here clamps or wraps.
    const float gain = masterGainParam->load();
    auto* leftCh  = buffer.getWritePointer(0);
    auto* rightCh = buffer.getWritePointer(1);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        // Every layer is ticked every sample, even silent ones, and never
        // skipped. Skipping would freeze that layer's envelope time, so
        // unmuting mid-note would resume from a stale position instead of where
        // the note actually is; it would also bypass the gain ramp and turn
        // switching a layer Off mid-tail into an instant cut. Idle voices
        // already cost almost nothing.
        float mixed = 0.0f;
        for (int layerIndex = 0; layerIndex < getNumLayers(); ++layerIndex)
            mixed += getLayer(layerIndex).voice.processSample()
                       * layerGain[(size_t) layerIndex].getNextValue();

        float sample = mixed * gain;

        // Apply DC blocker to remove any slowly-drifting offset from the synthesis.
        float blocked = sample - dcBlockerX + dcBlockerR * dcBlockerY;
        dcBlockerX = sample;
        dcBlockerY = blocked;

        // Mono kick output copied to both channels; stereo is introduced in Phase 7 FX.
        leftCh[i]  = blocked;
        rightCh[i] = blocked;
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

        auto pitchCopy = getLayer(i).pitchModel.getValueTree().createCopy();
        pitchCopy.setProperty(PresetIDs::curveProp, PresetIDs::pitchCurveTag, nullptr);
        layerNode.addChild(pitchCopy, -1, nullptr);

        auto ampCopy = getLayer(i).ampModel.getValueTree().createCopy();
        ampCopy.setProperty(PresetIDs::curveProp, PresetIDs::ampCurveTag, nullptr);
        layerNode.addChild(ampCopy, -1, nullptr);

        root.addChild(layerNode, -1, nullptr);
    }

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

    // A loaded preset is not a user edit to undo back from, and the re-seeding above would
    // otherwise leave its own transactions sitting at the top of the history.
    envelopeUndoManager.clearUndoHistory();
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

// Entry point called by the host to create a new plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DOOFAudioProcessor();
}
