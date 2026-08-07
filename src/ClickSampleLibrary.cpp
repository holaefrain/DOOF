#include "ClickSampleLibrary.h"
#include <algorithm>

// BinaryData.h only exists when CMake found at least one .wav in resources/clicks/ and built the
// DOOFClickSamples target. Guarding the include is what lets an empty folder still compile, which
// is the state this repo is normally in.
#if DOOF_HAS_CLICK_SAMPLES
 #include <BinaryData.h>
#endif

// Decodes every embedded sample into a mono float buffer, once, at construction.
ClickSampleLibrary::ClickSampleLibrary()
{
#if DOOF_HAS_CLICK_SAMPLES
    // Sorted by original filename rather than taken in BinaryData's own order. CMake already sorts
    // the glob, but relying on that alone would put slot order at the mercy of a build-system
    // detail — and slot order is part of what a saved preset means, so it has to be pinned on both
    // sides. Resource index is carried alongside because getNamedResource is keyed by the mangled
    // symbol name, not by the original filename.
    struct Embedded { juce::String filename; int resourceIndex; };
    std::vector<Embedded> found;

    for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
        found.push_back({ juce::String(BinaryData::originalFilenames[i]), i });

    std::sort(found.begin(), found.end(),
              [](const Embedded& a, const Embedded& b) { return a.filename < b.filename; });

    // Anything past the fourth file is ignored: the click type parameter is permanently eight
    // entries wide and can never grow (see ParamIDs::ClickType), so a fifth sample has nowhere to
    // go. The README in resources/clicks/ says so too.
    const int usable = juce::jmin((int) found.size(), ClickVoice::kNumSampleSlots);

    for (int slot = 0; slot < usable; ++slot)
    {
        int sizeInBytes = 0;
        const char* data = BinaryData::getNamedResource(
            BinaryData::namedResourceList[found[(size_t) slot].resourceIndex], sizeInBytes);

        if (data == nullptr || sizeInBytes <= 0)
            continue;

        loadSlotFromMemory(slot, data, sizeInBytes, found[(size_t) slot].filename);
    }
#endif
}

// Reads one encoded file into `slot`, folding to mono. Returns false on anything unreadable,
// leaving the slot empty rather than throwing or half-filling it.
bool ClickSampleLibrary::loadSlotFromMemory(int slot, const void* data, int sizeInBytes,
                                            const juce::String& filename)
{
    if (! juce::isPositiveAndBelow(slot, ClickVoice::kNumSampleSlots))
        return false;

    // Built here rather than held as a member: decoding happens a handful of times at startup, so
    // there is nothing to gain from keeping a format manager alive for the plugin's lifetime.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // The stream does not take ownership of the embedded bytes, which live in the binary's rodata
    // for the process's lifetime.
    auto stream = std::make_unique<juce::MemoryInputStream>(data, (size_t) sizeInBytes, false);

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(std::move(stream)));
    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 1)
        return false;

    // Guards against an absurdly long file being embedded by accident. A click is milliseconds;
    // ten seconds is far past anything reasonable and still only a few MB.
    const int numSamples = (int) juce::jmin(reader->lengthInSamples,
                                             (juce::int64) (reader->sampleRate * 10.0));

    juce::AudioBuffer<float> interleaved((int) reader->numChannels, numSamples);
    if (! reader->read(&interleaved, 0, numSamples, 0, true, true))
        return false;

    // Folded to mono because DOOF's core is mono end to end — stereo only appears after the FX
    // stage in Phase 7. Averaged rather than summed, so a dual-mono file does not come out 6 dB
    // hotter than the same content saved as mono.
    auto& buffer = storage[(size_t) slot];
    buffer.assign((size_t) numSamples, 0.0f);

    for (int channel = 0; channel < interleaved.getNumChannels(); ++channel)
    {
        const auto* source = interleaved.getReadPointer(channel);
        for (int i = 0; i < numSamples; ++i)
            buffer[(size_t) i] += source[i];
    }

    const float scale = 1.0f / (float) interleaved.getNumChannels();
    for (auto& value : buffer)
        value *= scale;

    names[(size_t) slot] = filename;

    decoded[(size_t) slot].data = buffer.data();
    decoded[(size_t) slot].numSamples = numSamples;
    decoded[(size_t) slot].sourceSampleRate = reader->sampleRate;

    return true;
}

int ClickSampleLibrary::numLoaded() const
{
    int count = 0;
    for (const auto& slot : decoded)
        if (slot.isLoaded())
            ++count;

    return count;
}

juce::String ClickSampleLibrary::nameOf(int slot) const
{
    if (! juce::isPositiveAndBelow(slot, ClickVoice::kNumSampleSlots))
        return {};

    return names[(size_t) slot];
}
