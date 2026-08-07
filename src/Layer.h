#pragma once
#include "EnvelopeModel.h"
#include "EnvelopePublisher.h"
#include "SubVoice.h"
#include "ClickVoice.h"

// Layer — one of DOOF's five layers (§3.2 of project-architecture.md): its own
// pitch and amp envelope models, the publisher bridging them to the audio
// thread, and the voice that renders them.
//
// Each layer publishes its own EnvelopeSnapshot rather than sharing one
// combined snapshot across all five. That keeps EnvelopePublisher,
// EnvelopeSnapshot and SubVoice's contracts exactly as Phase 2 built them, and
// means editing one layer's envelope rebuilds two lookup tables instead of ten.
//
// Not copyable or movable (EnvelopePublisher holds references to the two
// models beside it), and not default-constructible, since those models need
// the shared UndoManager. The owner therefore holds these by unique_ptr — see
// DOOFAudioProcessor::layers.
//
// The mixer parameters (type/level/mute/solo) deliberately live in the APVTS
// rather than here, so the host can automate them and preset save/restore
// covers them for free.
struct Layer
{
    // sharedUndoManager is shared by every layer's models, so one undo step
    // reverts the most recent edit chronologically regardless of which layer
    // or which curve it touched (§3.4: "Undo / redo, 30 steps"). Must outlive
    // this Layer.
    explicit Layer(juce::UndoManager& sharedUndoManager)
        : pitchModel(&sharedUndoManager),
          ampModel(&sharedUndoManager),
          publisher(pitchModel, ampModel)
    {
    }

    // Declaration order matters and must not be rearranged: publisher stores
    // references to both models, so they have to be constructed first. C++
    // constructs members in declaration order regardless of initialiser-list
    // order.
    EnvelopeModel     pitchModel; // node-based pitch envelope, message-thread only
    EnvelopeModel     ampModel;   // node-based amp envelope, message-thread only
    EnvelopePublisher publisher;  // rebuilds + atomically publishes this layer's snapshot on edit

    // Both voices exist on every layer regardless of its type, and both are ticked every sample —
    // only the one the type selects is mixed. Holding them side by side rather than swapping
    // between them means switching a layer's type mid-note cannot resume the newly-selected voice
    // from a stale position, and an idle voice costs almost nothing. Renamed from plain `voice` in
    // Phase 5, when there was no longer only one.
    SubVoice          subVoice;   // renders this layer when its type is Sub
    ClickVoice        clickVoice; // renders this layer when its type is Click

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Layer)
};
