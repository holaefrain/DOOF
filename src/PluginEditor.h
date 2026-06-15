#pragma once
#include "PluginProcessor.h"

class DOOFAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit DOOFAudioProcessorEditor(DOOFAudioProcessor&);
    ~DOOFAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    DOOFAudioProcessor& audioProcessor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DOOFAudioProcessorEditor)
};
