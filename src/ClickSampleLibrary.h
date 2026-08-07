#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include "ClickVoice.h"
#include <array>
#include <vector>

// ClickSampleLibrary — decodes the factory click samples embedded by CMake into plain float
// buffers the audio thread can read.
//
// Lives in the full plugin target rather than the lean DOOFTests one, because decoding a WAV needs
// juce_audio_formats. ClickVoice deliberately does not: it takes bare ClickSample pointers, so the
// voice itself stays juce_core only and keeps compiling into the fast iteration target.
//
// Threading. All decoding happens once, in the constructor, on whatever thread builds the
// processor - the message thread in every host. After that the buffers are immutable and the audio
// thread only reads them, so no lock, no atomic and no snapshot swap is needed. This is the one
// place in the engine where file decoding happens, and it is nowhere near the audio callback (§2's
// cardinal rule).
//
// An empty library is the normal state of this repo. resources/clicks/ ships with no audio because
// §8 lists factory content licensing as a project risk, so every slot may legitimately be empty and
// a slot with nothing behind it is silent.
class ClickSampleLibrary
{
public:
    // Decodes every embedded sample. Safe to construct when the build embedded none, which is what
    // DOOF_HAS_CLICK_SAMPLES=0 means - the result is simply four empty slots.
    ClickSampleLibrary();

    // The four slots, in the order the click type parameter's Sample 1-4 entries name them.
    // Always addresses exactly ClickVoice::kNumSampleSlots entries, loaded or not, so a caller
    // never has to bounds-check against how many files happened to be embedded.
    const ClickSample* slots() const { return decoded.data(); }

    // How many slots actually have content. Zero in a build with an empty resources/clicks/.
    int numLoaded() const;

    // The original filename behind a slot, or an empty string for an empty slot. Intended for the
    // Phase 5 panel and for test diagnostics, never for identifying a slot in saved state - a
    // preset stores the slot index, so renaming a file re-points every preset that used it.
    juce::String nameOf(int slot) const;

    // Decodes a block of encoded audio into one slot, replacing whatever was there. Returns false
    // and leaves the slot empty if the data is not audio this build can read, so one bad file
    // cannot take the plugin down.
    //
    // Public because it is the only way to test the decoder at all: the embedded assets are fixed
    // at build time, and resources/clicks/ is empty in this repo, so a test that could only reach
    // this path through BinaryData could never run. Tests generate a WAV, read it into memory and
    // load it here - the same function the constructor uses, not a parallel one that could drift.
    // Message thread only; never call it while audio is running, since it reallocates the buffer a
    // voice may be reading.
    bool loadSlotFromMemory(int slot, const void* data, int sizeInBytes, const juce::String& name);

private:
    // Decoded mono audio, one vector per slot. Owns the memory the ClickSample entries point into,
    // so this must be declared before `decoded` is populated and must never be resized afterwards.
    std::array<std::vector<float>, ClickVoice::kNumSampleSlots> storage;

    // Filenames, index-aligned with storage, for nameOf().
    std::array<juce::String, ClickVoice::kNumSampleSlots> names;

    // The pointers handed to ClickVoice, each addressing its matching storage entry.
    std::array<ClickSample, ClickVoice::kNumSampleSlots> decoded;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClickSampleLibrary)
};
