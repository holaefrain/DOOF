#include "PluginProcessor.h"
#include "PluginEditor.h"

DOOFAudioProcessor::DOOFAudioProcessor()
    : AudioProcessor(BusesProperties()
        .withOutput("Output",   juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::mono(),   false)) // optional; used in Phase 10 TRIGGER
{
}

DOOFAudioProcessor::~DOOFAudioProcessor() = default;

void DOOFAudioProcessor::prepareToPlay(double /*sampleRate*/, int /*samplesPerBlock*/) {}

void DOOFAudioProcessor::releaseResources() {}

bool DOOFAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // Sidechain input must be mono or disabled
    if (!layouts.inputBuses.isEmpty())
    {
        const auto& sc = layouts.inputBuses[0];
        if (!sc.isDisabled() && sc != juce::AudioChannelSet::mono())
            return false;
    }

    return true;
}

void DOOFAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                      juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused(midiMessages);

    buffer.clear(); // Phase 0: silence — synthesis added in Phase 1
}

juce::AudioProcessorEditor* DOOFAudioProcessor::createEditor()
{
    return new DOOFAudioProcessorEditor(*this);
}

void DOOFAudioProcessor::getStateInformation(juce::MemoryBlock& /*destData*/) {}

void DOOFAudioProcessor::setStateInformation(const void* /*data*/, int /*sizeInBytes*/) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DOOFAudioProcessor();
}
