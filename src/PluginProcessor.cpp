#include "PluginProcessor.h"
#include "PluginEditor.h"

// ── Stable parameter IDs ───────────────────────────────────────────────────────
// IDs follow the namespaced format from §2 of project-reference.md.
// Never rename or remove an ID once any preset has been saved with it.
namespace ParamIDs
{
    static const juce::String subGain = "sub.gain"; // master output gain for the Sub layer
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
}

DOOFAudioProcessor::~DOOFAudioProcessor() = default;

// Build the initial parameter layout for the APVTS.
// Called once from the constructor initialiser list; adding a parameter here
// registers it for host automation and preset save/restore automatically.
juce::AudioProcessorValueTreeState::ParameterLayout
DOOFAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // sub.gain — linear master gain for the sub layer output, range [0, 1].
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID { ParamIDs::subGain, 1 },
        "Sub Gain",
        juce::NormalisableRange<float>(0.0f, 1.0f),
        0.8f // default: 80% level
    ));

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

// Serialises current APVTS state to XML and packs it into destData for host project save.
void DOOFAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

// Restores APVTS state from a blob previously written by getStateInformation.
// Guards against malformed blobs with a tag-name check before replacing state.
void DOOFAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml && xml->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

// Entry point called by the host to create a new plugin instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DOOFAudioProcessor();
}
