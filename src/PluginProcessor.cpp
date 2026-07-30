#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DefaultEnvelopes.h"
#include "ParamIDs.h"
#include "LayerAudibility.h"

// State-serialisation schema (Phase 3 Step 7): wraps apvts state and both
// envelope models' trees into one root for getStateInformation/setStateInformation
// and the .doof preset file. pitchEnvelopeModel and ampEnvelopeModel are both
// EnvelopeIDs::envelopeType ("ENVELOPE"), so curveProp distinguishes them —
// same rule as APVTS parameter IDs: never rename once a preset has shipped.
namespace PresetIDs
{
    static const juce::Identifier rootType  { "DOOFState" };
    static const juce::Identifier curveProp { "curve" };
    static const juce::String pitchCurveTag = "pitch";
    static const juce::String ampCurveTag   = "amp";
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
    // Seed the out-of-the-box envelope shapes. Editing these becomes possible
    // once the Phase 3 canvas exists; until then this is the only sound DOOF makes.
    DefaultEnvelopes::seedPitch(pitchEnvelopeModel);
    DefaultEnvelopes::seedAmp(ampEnvelopeModel);
}

DOOFAudioProcessor::~DOOFAudioProcessor() = default;

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
    voice.prepare(sampleRate);

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
//   1. Consume all MIDI events for this block (block-accurate; sample-accurate in a later phase).
//   2. For each sample: tick the voice, apply gain, apply DC blocker, write to L+R.
//
// Real-time safety guarantee: no allocations, no locks, no file/network I/O in this function
// or any function it calls.  Verify with AddressSanitizer's alloc hooks when running headless.
void DOOFAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Load the current envelope snapshot once for this whole block (§2: the
    // audio thread loads the atomic pointer once per block, never per-sample).
    voice.setSnapshot(envelopePublisher.getSnapshot());

    // Step 1 — handle MIDI events.
    // Note-off is intentionally ignored: the amp envelope handles the voice decay.
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            voice.noteOn(msg.getNoteNumber());
    }

    // Step 2 — render samples.
    const float gain = apvts.getRawParameterValue(ParamIDs::subGain)->load();
    auto* leftCh  = buffer.getWritePointer(0);
    auto* rightCh = buffer.getWritePointer(1);

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float sample = voice.processSample() * gain;

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
    root.addChild(apvts.copyState(), -1, nullptr);

    auto pitchCopy = pitchEnvelopeModel.getValueTree().createCopy();
    pitchCopy.setProperty(PresetIDs::curveProp, PresetIDs::pitchCurveTag, nullptr);
    root.addChild(pitchCopy, -1, nullptr);

    auto ampCopy = ampEnvelopeModel.getValueTree().createCopy();
    ampCopy.setProperty(PresetIDs::curveProp, PresetIDs::ampCurveTag, nullptr);
    root.addChild(ampCopy, -1, nullptr);

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

    const auto apvtsState = root.getChildWithName(apvts.state.getType());
    if (apvtsState.isValid())
        apvts.replaceState(apvtsState);

    const auto pitchState = root.getChildWithProperty(PresetIDs::curveProp, PresetIDs::pitchCurveTag);
    if (pitchState.isValid())
        pitchEnvelopeModel.setState(pitchState);

    const auto ampState = root.getChildWithProperty(PresetIDs::curveProp, PresetIDs::ampCurveTag);
    if (ampState.isValid())
        ampEnvelopeModel.setState(ampState);
}

// Entry point called by the host to create a new plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DOOFAudioProcessor();
}
