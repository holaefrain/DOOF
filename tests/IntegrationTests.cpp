#include "PluginProcessor.h"
#include "ParamIDs.h"
#include "LayerAudibility.h"

#include <cmath>
#include <limits>
#include <vector>

// IntegrationTests — full-engine tests that need the real PluginProcessor
// (and therefore PluginEditor/EnvelopeCanvas, since PluginProcessor::
// createEditor() references them, and the full audio_utils/audio_processors/
// gui_basics/dsp chain they require). Kept in a separate binary/CTest entry
// from DOOFTests, which is deliberately kept lean (juce_core/data_structures/
// events only) for fast day-to-day iteration — see DOOFTests' own CMakeLists.txt
// comments. Run this target before a phase's final verify pass, or whenever
// changing PluginProcessor-level behaviour (state serialisation, the full
// signal path end to end, etc).

// Phase3PresetRoundTripTest — Phase 3 §6 Verify: "Save to .doof, alter the
// patch, reload — the reloaded render null-matches the original." Exercises
// the real getStateInformation/setStateInformation path (not just the
// EnvelopeModel data) end to end through actual audio rendering. This is
// what caught a real bug during Phase 3 Step 7: EnvelopeModel::setState()
// used to reassign its tree (`tree = newState`) instead of mutating it in
// place, which silently orphaned EnvelopePublisher's ValueTree::Listener
// (attached once, in its constructor) — after any preset load, the audio
// thread would stop receiving snapshot updates entirely, even though
// EnvelopeModel::getNode() kept reading correctly (so the canvas looked
// fine). A model-data-only round-trip check wouldn't have caught this;
// only rendering through the real engine does.
class Phase3PresetRoundTripTest : public juce::UnitTest
{
public:
    Phase3PresetRoundTripTest() : juce::UnitTest("Phase3PresetRoundTrip") {}

    void runTest() override
    {
        beginTest("Render at state A, save, alter to state B, reload A, render again: null-matches original A render");

        DOOFAudioProcessor processor;
        processor.prepareToPlay(44100.0, 512);

        // State A: deliberately reshaped away from the defaults.
        processor.getLayer(0).pitchModel.moveNode(0, 0.0, 200.0);
        processor.getLayer(0).ampModel.moveNode(1, 0.002, 0.9);

        auto renderNote = [&](int numSamples)
        {
            std::vector<float> out((size_t) numSamples);
            juce::AudioBuffer<float> buffer(2, numSamples);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            processor.processBlock(buffer, midi);
            auto* ch = buffer.getReadPointer(0);
            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = ch[i];
            return out;
        };

        const int numSamples = 2048;
        const auto renderA = renderNote(numSamples);

        juce::MemoryBlock saved;
        processor.getStateInformation(saved);

        // State B: a different alteration, made after saving.
        processor.getLayer(0).pitchModel.moveNode(0, 0.0, 50.0);
        processor.getLayer(0).ampModel.deleteNode(0);

        processor.setStateInformation(saved.getData(), (int) saved.getSize());

        // Re-prepare so the second render starts from a clean Idle voice
        // state, same as the first — without this, the second noteOn()
        // arrives while the first note is still decaying (2048 samples is
        // only ~46 ms, well inside the amp envelope's ~400 ms tail) and
        // triggers a choke-retrigger transition instead of a fresh Body
        // start, so the two renders would never match regardless of whether
        // the save/reload itself is correct.
        processor.prepareToPlay(44100.0, 512);
        const auto renderAReloaded = renderNote(numSamples);

        bool identical = true;
        int firstMismatch = -1;
        for (int i = 0; i < numSamples; ++i)
        {
            if (std::abs(renderA[(size_t) i] - renderAReloaded[(size_t) i]) > 1.0e-7f)
            {
                identical = false;
                firstMismatch = i;
                break;
            }
        }

        expect(identical, "Reloaded state A render should null-match the original state A render"
                           + (identical ? juce::String() : (" (first mismatch at sample " + juce::String(firstMismatch) + ")")));
    }
};

static Phase3PresetRoundTripTest phase3PresetRoundTripTest;

// Phase4ParameterLayoutTest — pins the APVTS parameter contract for the mixer
// (§3.2, §3.3): every expected ID exists, and the out-of-the-box defaults are
// what the engine and every earlier phase's render assertions assume.
//
// The expected IDs are written out as literal strings rather than obtained
// from ParamIDs' builder functions, deliberately. §2 forbids ever renaming a
// parameter ID once a preset has been saved against it; a test that asked the
// builder for the ID would follow any rename along and keep passing, proving
// nothing. Spelling them out means changing the format breaks this test, which
// is exactly the alarm we want.
class Phase4ParameterLayoutTest : public juce::UnitTest
{
public:
    Phase4ParameterLayoutTest() : juce::UnitTest("Phase4ParameterLayout") {}

    void runTest() override
    {
        testAllExpectedParametersExist();
        testDefaultsPreserveThePrePhase4Patch();
    }

private:
    // Layer count is spelled out here for the same reason as the IDs below.
    static constexpr int kExpectedLayers = 5;
    static_assert(kExpectedLayers == LayerAudibility::kNumLayers,
                  "This test's hardcoded layer count has drifted from LayerAudibility::kNumLayers");

    void testAllExpectedParametersExist()
    {
        beginTest("(a) All 21 expected parameter IDs exist, and nothing extra");

        DOOFAudioProcessor processor;

        juce::StringArray expected { "sub.gain" };
        for (int i = 1; i <= kExpectedLayers; ++i)
        {
            const auto n = juce::String(i);
            expected.add("layer" + n + ".type");
            expected.add("layer" + n + ".level");
            expected.add("layer" + n + ".mute");
            expected.add("layer" + n + ".solo");
        }

        expectEquals(expected.size(), 21, "1 master gain + 5 layers x 4 controls");

        for (const auto& id : expected)
            expect(processor.apvts.getParameter(id) != nullptr,
                   "Missing expected parameter ID: " + id);

        // Catches an accidentally *added* or duplicated parameter too, not just
        // a missing one.
        expectEquals(processor.getParameters().size(), expected.size(),
                     "Processor exposes a different number of parameters than expected");
    }

    void testDefaultsPreserveThePrePhase4Patch()
    {
        beginTest("(b) Defaults: layer 1 Sub at unity, layers 2-5 Off, master still 0.8");

        DOOFAudioProcessor processor;

        auto raw = [&processor](const juce::String& id)
        {
            auto* value = processor.apvts.getRawParameterValue(id);
            return value != nullptr ? value->load() : std::numeric_limits<float>::quiet_NaN();
        };

        // The single gain that existed before the mixer, unchanged — so every
        // Phase 1/2/3 render assertion still describes the default patch.
        expectWithinAbsoluteError(raw("sub.gain"), 0.8f, 1.0e-6f);

        expectWithinAbsoluteError(raw("layer1.type"), (float) (int) ParamIDs::LayerType::sub, 1.0e-6f);
        for (int i = 2; i <= kExpectedLayers; ++i)
            expectWithinAbsoluteError(raw("layer" + juce::String(i) + ".type"),
                                       (float) (int) ParamIDs::LayerType::off, 1.0e-6f);

        // Unity per layer, so the master gain remains the only attenuation in
        // the default path and one enabled layer sounds exactly as it used to.
        for (int i = 1; i <= kExpectedLayers; ++i)
        {
            const auto n = juce::String(i);
            expectWithinAbsoluteError(raw("layer" + n + ".level"), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError(raw("layer" + n + ".mute"),  0.0f, 1.0e-6f);
            expectWithinAbsoluteError(raw("layer" + n + ".solo"),  0.0f, 1.0e-6f);
        }
    }
};

static Phase4ParameterLayoutTest phase4ParameterLayoutTest;

// Phase4LayerConstructionTest — verifies the five-layer array itself (§3.2):
// every layer is really constructed, seeded, and publishing its own snapshot,
// including the four that default to Off.
//
// The Off layers matter as much as layer 0 here: they're seeded precisely so
// that switching one to Sub produces a kick immediately rather than silence
// from an empty envelope, and that only holds if construction seeded all five.
class Phase4LayerConstructionTest : public juce::UnitTest
{
public:
    Phase4LayerConstructionTest() : juce::UnitTest("Phase4LayerConstruction") {}

    void runTest() override
    {
        testEveryLayerIsSeededAndPublishing();
        testUndoHistoryStartsEmpty();
        testLayersAreIndependent();
    }

private:
    void testEveryLayerIsSeededAndPublishing()
    {
        beginTest("(a) All five layers are seeded with the default envelopes and publishing");

        DOOFAudioProcessor processor;
        processor.prepareToPlay(44100.0, 512);

        const int expectedPitchNodes = processor.getLayer(0).pitchModel.getNumNodes();
        const int expectedAmpNodes   = processor.getLayer(0).ampModel.getNumNodes();
        expect(expectedPitchNodes > 0, "Layer 0's pitch envelope should have been seeded");
        expect(expectedAmpNodes   > 0, "Layer 0's amp envelope should have been seeded");

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const auto& layer = processor.getLayer(i);
            const auto which = " (layer " + juce::String(i) + ")";

            expectEquals(layer.pitchModel.getNumNodes(), expectedPitchNodes,
                         "Pitch envelope not seeded identically" + which);
            expectEquals(layer.ampModel.getNumNodes(), expectedAmpNodes,
                         "Amp envelope not seeded identically" + which);

            // A layer that isn't publishing would hand its voice a null
            // snapshot and render silence even once switched to Sub.
            const auto* snapshot = layer.publisher.getSnapshot();
            expect(snapshot != nullptr, "Publisher has no snapshot" + which);

            if (snapshot != nullptr)
                expect(snapshot->pitchTable[0] > 0.0f,
                       "Seeded pitch table should start above 0 Hz" + which);
        }
    }

    void testUndoHistoryStartsEmpty()
    {
        beginTest("(b) The factory patch is not sitting in the undo history");

        DOOFAudioProcessor processor;

        // Seeding ten models goes through the normal undoable edit path, which
        // would otherwise leave ~70 transactions queued up: enough to fill the
        // whole 30-step cap with factory setup, and to let Cmd+Z at startup
        // start dismantling the default patch.
        //
        // This asks the UndoManager directly rather than calling undo() and
        // watching for a changed node count. The manager is shared across all
        // ten models, so the newest transaction after seeding belongs to the
        // *last* model seeded — an undo() would pop that one and leave layer 0
        // untouched, making a node-count check pass while the history was still
        // full. (Confirmed: the node-count version of this test did not fail
        // when clearUndoHistory() was temporarily removed.)
        expect(! processor.getLayer(0).pitchModel.canUndo(),
               "Undo history is not empty at startup, so seeding left transactions behind");
        expect(! processor.getLayer(0).pitchModel.canRedo(),
               "Redo history is not empty at startup");
    }

    void testLayersAreIndependent()
    {
        beginTest("(c) Editing one layer's envelope does not touch another's");

        DOOFAudioProcessor processor;
        processor.prepareToPlay(44100.0, 512);

        const auto valueOnLayer = [&processor](int index)
        {
            return processor.getLayer(index).pitchModel.getNode(0).value;
        };

        const double originalOnLayer1 = valueOnLayer(1);
        processor.getLayer(0).pitchModel.moveNode(0, 0.0, 999.0);

        expectWithinAbsoluteError(valueOnLayer(0), 999.0, 1.0e-9, "Layer 0's own edit did not apply");
        expectWithinAbsoluteError(valueOnLayer(1), originalOnLayer1, 1.0e-9,
                                   "Editing layer 0 also changed layer 1 - the layers share state");

        // The edited layer must republish; the untouched one must not be forced to.
        const auto* snapshot = processor.getLayer(0).publisher.getSnapshot();
        expect(snapshot != nullptr && snapshot->pitchTable[0] > 900.0f,
               "Layer 0's snapshot did not pick up its own edit");
    }
};

static Phase4LayerConstructionTest phase4LayerConstructionTest;

// Phase4MixerTest — Phase 4 §6 Verify: "Set all layers to unity and render —
// assert no unexpected clipping", plus the engine half of "solo layers 1 and 3
// — only those are audible" and "add a mute to a soloed layer — it goes silent".
//
// On "no unexpected clipping": five layers at unity necessarily sum past full
// scale, which is arithmetic, not a defect — DOOF sums layers straight, since
// dividing by the number of active layers would change every other layer's
// sound whenever one was enabled, and the master limiter that tames the sum is
// Phase 9. So what's actually asserted here is that the engine doesn't clamp,
// wrap, or go non-finite in that regime: the mixed render must equal the sum of
// the layers rendered individually, exactly the property clipping would break.
//
// Each layer is given a *different* pitch before mixing. With the identical
// seeded envelopes they ship with, "sum of the individual renders" would just be
// five times any one of them, and the test would still pass if the mixer read
// the wrong layer's voice or gain.
class Phase4MixerTest : public juce::UnitTest
{
public:
    Phase4MixerTest() : juce::UnitTest("Phase4Mixer") {}

    void runTest() override
    {
        testSuperpositionAndNoClipping();
        testMultiSoloIsolatesTheSoloedLayers();
        testMuteBeatsSoloThroughTheEngine();
        testMuteMidTailDoesNotClick();
    }

private:
    static constexpr int kNumSamples = 4096;
    static constexpr int kBlockSize  = 256;
    static constexpr double kSampleRate = 44100.0;

    // Writes a parameter by its real-world value (a choice index for type, 0/1
    // for the bools), letting the parameter do its own normalisation rather than
    // hardcoding normalised constants that would silently rot if a range or
    // choice list changed.
    static void setParamValue(DOOFAudioProcessor& processor, const juce::String& id, float value)
    {
        auto* param = processor.apvts.getParameter(id);
        jassert(param != nullptr);
        param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    static juce::String layerId(int index, const juce::String& suffix)
    {
        return "layer" + juce::String(index + 1) + "." + suffix;
    }

    // Sign changes over the opening of a render — a cheap stand-in for pitch,
    // used to tie each layer's output to the pitch that layer was actually
    // given. Counted over the first window only, where the seeded pitch
    // envelope is still near its starting node and the layers are furthest apart.
    static int zeroCrossings(const std::vector<float>& samples)
    {
        static constexpr int window = 2048;
        const int limit = juce::jmin(window, (int) samples.size());

        int count = 0;
        for (int i = 1; i < limit; ++i)
            if ((samples[(size_t) i - 1] < 0.0f) != (samples[(size_t) i] < 0.0f))
                ++count;

        return count;
    }

    static void makeAllLayersSubAtUnity(DOOFAudioProcessor& processor)
    {
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"), (float) (int) ParamIDs::LayerType::sub);
            setParamValue(processor, layerId(i, "level"), 1.0f);
            setParamValue(processor, layerId(i, "mute"),  0.0f);
            setParamValue(processor, layerId(i, "solo"),  0.0f);
        }
    }

    // Distinct starting pitch per layer, so each layer's contribution is
    // individually recognisable in the mix.
    static void giveLayersDistinctPitches(DOOFAudioProcessor& processor)
    {
        for (int i = 0; i < processor.getNumLayers(); ++i)
            processor.getLayer(i).pitchModel.moveNode(0, 0.0, 60.0 + 25.0 * i);
    }

    // prepareToPlay is called each time so the voices start Idle and the gain
    // smoothers jump straight to their targets — otherwise a ramp at the start
    // of one render but not another would show up as a mismatch that has
    // nothing to do with the mixing itself.
    static std::vector<float> render(DOOFAudioProcessor& processor)
    {
        processor.prepareToPlay(kSampleRate, 512);

        juce::AudioBuffer<float> buffer(2, kNumSamples);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        processor.processBlock(buffer, midi);

        std::vector<float> out((size_t) kNumSamples);
        const auto* channel = buffer.getReadPointer(0);
        for (int i = 0; i < kNumSamples; ++i)
            out[(size_t) i] = channel[i];
        return out;
    }

    // Renders one layer alone by soloing it, which also exercises the solo path
    // rather than sidestepping it by switching the others Off.
    static std::vector<float> renderLayerAlone(DOOFAudioProcessor& processor, int index)
    {
        for (int i = 0; i < processor.getNumLayers(); ++i)
            setParamValue(processor, layerId(i, "solo"), i == index ? 1.0f : 0.0f);

        auto out = render(processor);

        for (int i = 0; i < processor.getNumLayers(); ++i)
            setParamValue(processor, layerId(i, "solo"), 0.0f);

        return out;
    }

    // Renders block by block so a parameter can change between blocks with the note still ringing.
    // render() can't: it re-prepares each time, so the smoothers jump and skip the ramp (d) measures.
    // muteAfterSamples must land on a block boundary, as a host change would; -1 means never mute.
    static std::vector<float> renderInBlocks(DOOFAudioProcessor& processor,
                                              int layerIndex, int muteAfterSamples)
    {
        jassert(muteAfterSamples < 0 || muteAfterSamples % kBlockSize == 0);

        setParamValue(processor, layerId(layerIndex, "mute"), 0.0f);
        processor.prepareToPlay(kSampleRate, kBlockSize);

        std::vector<float> out;
        out.reserve((size_t) kNumSamples);

        juce::AudioBuffer<float> buffer(2, kBlockSize);

        for (int start = 0; start < kNumSamples; start += kBlockSize)
        {
            if (start == muteAfterSamples)
                setParamValue(processor, layerId(layerIndex, "mute"), 1.0f);

            juce::MidiBuffer midi;
            if (start == 0)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);

            processor.processBlock(buffer, midi);

            const auto* channel = buffer.getReadPointer(0);
            for (int i = 0; i < kBlockSize; ++i)
                out.push_back(channel[i]);
        }

        return out;
    }

    void testSuperpositionAndNoClipping()
    {
        beginTest("(a) Five layers at unity: mix equals the sum of the parts, stays finite");

        DOOFAudioProcessor processor;
        makeAllLayersSubAtUnity(processor);
        giveLayersDistinctPitches(processor);

        const auto mixed = render(processor);

        std::vector<std::vector<float>> individual;
        std::vector<double> summed((size_t) kNumSamples, 0.0);
        for (int layerIndex = 0; layerIndex < processor.getNumLayers(); ++layerIndex)
        {
            individual.push_back(renderLayerAlone(processor, layerIndex));
            for (int i = 0; i < kNumSamples; ++i)
                summed[(size_t) i] += individual.back()[(size_t) i];
        }

        // Each layer was given its own pitch, so no two layers may render the
        // same signal. Without this, a mixer that read one single layer's voice
        // for every layer would still satisfy the superposition check below —
        // both the mix and the sum of the parts would be wrong in the same way,
        // and cancel out. This is the assertion that pins each layer to its own
        // voice.
        // Absolute check, not a self-consistent one: each layer was given a
        // higher starting pitch than the last, so each layer-alone render must
        // show strictly more sign changes than the one before it.
        //
        // The pairwise-difference check below is not sufficient on its own. If
        // the mixer read a single layer's voice for every layer, that voice
        // would be advanced once per layer per sample, so each layer-alone
        // render would pick off a different one of five consecutive samples —
        // genuinely different signals, summing consistently, passing both the
        // superposition and pairwise checks. (Verified: that exact break went
        // undetected until this assertion existed.) Tying the output back to the
        // pitch each layer was configured with is what closes it.
        juce::String crossingReport;
        for (size_t a = 0; a < individual.size(); ++a)
            crossingReport << " layer" << (int) a << "=" << zeroCrossings(individual[a]);

        for (size_t a = 1; a < individual.size(); ++a)
            expect(zeroCrossings(individual[a]) > zeroCrossings(individual[a - 1]),
                   "Layer " + juce::String((int) a) + " should sound higher than layer "
                     + juce::String((int) a - 1) + " given the pitches assigned, but does not - "
                       "the mixer is not reading each layer's own voice ("
                     + crossingReport.trim() + ")");

        for (size_t a = 0; a < individual.size(); ++a)
            for (size_t b = a + 1; b < individual.size(); ++b)
            {
                double biggestDifference = 0.0;
                for (int i = 0; i < kNumSamples; ++i)
                    biggestDifference = juce::jmax(biggestDifference,
                                                    std::abs((double) individual[a][(size_t) i]
                                                               - (double) individual[b][(size_t) i]));

                expect(biggestDifference > 1.0e-3,
                       "Layers " + juce::String((int) a) + " and " + juce::String((int) b)
                         + " rendered the same signal despite having different pitches, so the "
                           "mixer is not reading each layer's own voice");
            }

        double worstError = 0.0;
        int worstIndex = -1;
        float peak = 0.0f;
        bool allFinite = true;

        for (int i = 0; i < kNumSamples; ++i)
        {
            if (! std::isfinite(mixed[(size_t) i]))
                allFinite = false;

            peak = juce::jmax(peak, std::abs(mixed[(size_t) i]));

            const double error = std::abs((double) mixed[(size_t) i] - summed[(size_t) i]);
            if (error > worstError) { worstError = error; worstIndex = i; }
        }

        expect(allFinite, "Mixed output contains a NaN or infinity");

        // Both paths are linear (per-layer gain, master gain and the one-pole DC
        // blocker all superpose), so the only difference is float accumulation
        // order — hence a tolerance rather than a bit-exact comparison.
        expect(worstError < 1.0e-4,
               "Mix does not equal the sum of the individual layers, so something clamped, "
               "wrapped, or read the wrong layer (worst error " + juce::String(worstError)
                 + " at sample " + juce::String(worstIndex) + ")");

        // Confirms the test is actually exercising the regime it claims to: if
        // the sum stayed under full scale there would be no clipping question to
        // answer, and (a) would be vacuous.
        expect(peak > 1.0f,
               "Five layers at unity did not exceed full scale (peak " + juce::String(peak)
                 + "), so this test is not exercising the clipping regime");
    }

    void testMultiSoloIsolatesTheSoloedLayers()
    {
        beginTest("(b) Soloing layers 1 and 3 leaves exactly those two in the mix");

        DOOFAudioProcessor processor;
        makeAllLayersSubAtUnity(processor);
        giveLayersDistinctPitches(processor);

        const auto layer1Alone = renderLayerAlone(processor, 0);
        const auto layer3Alone = renderLayerAlone(processor, 2);

        // §3.3's multi-solo: any number soloed at once.
        setParamValue(processor, layerId(0, "solo"), 1.0f);
        setParamValue(processor, layerId(2, "solo"), 1.0f);
        const auto soloed = render(processor);

        double worstError = 0.0;
        for (int i = 0; i < kNumSamples; ++i)
            worstError = juce::jmax(worstError,
                                     std::abs((double) soloed[(size_t) i]
                                                - ((double) layer1Alone[(size_t) i]
                                                    + (double) layer3Alone[(size_t) i])));

        expect(worstError < 1.0e-4,
               "Multi-solo output is not exactly layers 1 + 3, so a non-soloed layer is "
               "leaking in or a soloed one is missing (worst error " + juce::String(worstError) + ")");
    }

    void testMuteBeatsSoloThroughTheEngine()
    {
        beginTest("(c) A layer that is both soloed and muted is silent, and silences the rest");

        DOOFAudioProcessor processor;
        makeAllLayersSubAtUnity(processor);

        // Solo one layer and mute that same layer: mute wins for it (§3.3), and
        // its solo still suppresses everyone else, so the mix goes fully silent.
        setParamValue(processor, layerId(1, "solo"), 1.0f);
        setParamValue(processor, layerId(1, "mute"), 1.0f);

        const auto rendered = render(processor);

        float peak = 0.0f;
        for (int i = 0; i < kNumSamples; ++i)
            peak = juce::jmax(peak, std::abs(rendered[(size_t) i]));

        expectWithinAbsoluteError(peak, 0.0f, 1.0e-7f,
                                   "Expected silence: mute beats solo on the soloed layer, and its "
                                   "solo keeps every other layer out of the mix");
    }

    // Muting a ringing layer must not click: without the gain ramp its output would drop to zero
    // between two adjacent samples, leaving a step the size of whatever it was putting out.
    // Threshold and method are Phase 1's choke test - a step past 0.1 is audible whatever caused it.
    void testMuteMidTailDoesNotClick()
    {
        beginTest("(d) Muting mid-tail ramps down instead of clicking");

        static constexpr float kMaxJump = 0.1f;   // Phase 1's choke-test threshold
        static constexpr int   kMuteAt  = 2048;   // ~46 ms in: attack over, tail still loud

        // One audible layer, so muting it drops the whole output rather than a fifth of it.
        DOOFAudioProcessor processor;
        setParamValue(processor, layerId(0, "type"), (float) (int) ParamIDs::LayerType::sub);
        setParamValue(processor, layerId(0, "level"), 1.0f);
        for (int i = 1; i < processor.getNumLayers(); ++i)
            setParamValue(processor, layerId(i, "type"), (float) (int) ParamIDs::LayerType::off);

        const auto rendered = renderInBlocks(processor, 0, kMuteAt);

        // Near a zero crossing an unramped cut leaves no step, so the check below would pass
        // with no smoothing at all. This pins down that a hard cut here really would breach kMaxJump.
        const float atFlip = std::abs(rendered[(size_t) kMuteAt - 1]);
        expect(atFlip > kMaxJump,
               "Layer was only at " + juce::String(atFlip, 4) + " when muted, below the "
               + juce::String(kMaxJump, 2) + " click threshold, so this test would pass "
                 "without any smoothing at all - move kMuteAt");

        float worstJump = 0.0f;
        int   worstAt   = -1;

        for (size_t i = 1; i < rendered.size(); ++i)
        {
            const float jump = std::abs(rendered[i] - rendered[i - 1]);
            if (jump > worstJump) { worstJump = jump; worstAt = (int) i; }
        }

        expect(worstJump <= kMaxJump,
               "Sample jump " + juce::String(worstJump, 4) + " at sample "
                 + juce::String(worstAt) + " exceeds the " + juce::String(kMaxJump, 2)
                 + " click threshold - the mute is stepping, not ramping");

        // The mute must actually land, or the no-click result above holds trivially.
        // Measured against an unmuted baseline, not as absolute silence: the DC blocker is a
        // one-pole high-pass, so when its input stops it rings down over ~16 ms rather than dead.
        const auto baseline = renderInBlocks(processor, 0, -1);

        auto peakOverLastQuarter = [](const std::vector<float>& samples)
        {
            float peak = 0.0f;
            for (size_t i = samples.size() * 3 / 4; i < samples.size(); ++i)
                peak = juce::jmax(peak, std::abs(samples[i]));
            return peak;
        };

        const float mutedPeak    = peakOverLastQuarter(rendered);
        const float baselinePeak = peakOverLastQuarter(baseline);

        expect(mutedPeak < baselinePeak * 0.05f,
               "Muted render is still at " + juce::String(mutedPeak, 4) + " against an unmuted "
                 + juce::String(baselinePeak, 4) + " over the same window, so the mute never took "
                   "effect and the no-click result above is meaningless");
    }
};

static Phase4MixerTest phase4MixerTest;

// Phase4StateTest — Phase 4 §6 Verify: a preset carries all five layers, and presets written
// before the mixer existed still load.
//
// Every schema string below is written out as a literal rather than taken from PluginProcessor's
// PresetIDs. Those names can never be renamed once a preset has shipped (§2), and a test that
// asked the production code for the name would follow a rename along and keep passing. Spelling
// them out means changing the format breaks this test, which is the alarm we want. PresetIDs has
// internal linkage anyway, so this is also the only option.
class Phase4StateTest : public juce::UnitTest
{
public:
    Phase4StateTest() : juce::UnitTest("Phase4State") {}

    void runTest() override
    {
        testAllFiveLayersSurviveARoundTrip();
        testPhase3PresetStillLoads();
    }

private:
    static constexpr int kNumSamples = 4096;
    static constexpr double kSampleRate = 44100.0;

    static void setParamValue(DOOFAudioProcessor& processor, const juce::String& id, float value)
    {
        auto* param = processor.apvts.getParameter(id);
        jassert(param != nullptr);
        param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    static float rawParam(DOOFAudioProcessor& processor, const juce::String& id)
    {
        auto* value = processor.apvts.getRawParameterValue(id);
        return value != nullptr ? value->load() : std::numeric_limits<float>::quiet_NaN();
    }

    static juce::String layerId(int index, const juce::String& suffix)
    {
        return "layer" + juce::String(index + 1) + "." + suffix;
    }

    // prepareToPlay before every render, so both renders start from an Idle voice with the gains
    // already at target - otherwise the second noteOn would land mid-tail and choke-retrigger.
    static std::vector<float> render(DOOFAudioProcessor& processor)
    {
        processor.prepareToPlay(kSampleRate, 512);

        juce::AudioBuffer<float> buffer(2, kNumSamples);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
        processor.processBlock(buffer, midi);

        std::vector<float> out((size_t) kNumSamples);
        const auto* channel = buffer.getReadPointer(0);
        for (int i = 0; i < kNumSamples; ++i)
            out[(size_t) i] = channel[i];
        return out;
    }

    void testAllFiveLayersSurviveARoundTrip()
    {
        beginTest("(a) Save, alter every layer, reload: the render null-matches and every parameter returns");

        DOOFAudioProcessor processor;

        // State A. Three audible Sub layers at different pitches and levels, one muted, one Off
        // but soloed - so the render depends on type, level and mute all surviving the trip.
        const int types[]  { 1, 1, 1, 0, 0 };      // Sub, Sub, Sub, Off, Off
        const float levels[] { 0.7f, 0.5f, 0.3f, 0.9f, 0.2f };
        const float mutes[]  { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f };
        const float solos[]  { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        const double pitches[] { 90.0, 140.0, 200.0, 250.0, 300.0 };
        const double lengths[] { 0.20, 0.25, 0.30, 0.35, 0.40 };

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"),  (float) types[i]);
            setParamValue(processor, layerId(i, "level"), levels[i]);
            setParamValue(processor, layerId(i, "mute"),  mutes[i]);
            setParamValue(processor, layerId(i, "solo"),  solos[i]);

            processor.getLayer(i).pitchModel.moveNode(0, 0.0, pitches[i]);
            processor.getLayer(i).ampModel.setLength(lengths[i]);
        }

        const auto renderA = render(processor);

        juce::MemoryBlock saved;
        processor.getStateInformation(saved);

        // State B. Everything the preset describes is changed, so reloading has real work to do
        // and cannot pass by leaving the processor untouched.
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"),  (float) (int) ParamIDs::LayerType::off);
            setParamValue(processor, layerId(i, "level"), 0.0f);
            setParamValue(processor, layerId(i, "mute"),  1.0f);
            setParamValue(processor, layerId(i, "solo"),  1.0f);

            processor.getLayer(i).pitchModel.moveNode(0, 0.0, 777.0);
            processor.getLayer(i).ampModel.setLength(1.9);
        }

        processor.setStateInformation(saved.getData(), (int) saved.getSize());

        // Checked per layer as well as through the render: an Off layer's solo is inaudible by
        // design (section 3.3 ignores Off layers when deciding whether anything is soloed), so
        // the null-match alone could not tell whether solo came back.
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const auto where = " on layer " + juce::String(i + 1);

            expectWithinAbsoluteError(rawParam(processor, layerId(i, "type")),  (float) types[i], 1.0e-6f,
                                       "type did not survive the round trip" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "level")), levels[i], 1.0e-6f,
                                       "level did not survive the round trip" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "mute")),  mutes[i], 1.0e-6f,
                                       "mute did not survive the round trip" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "solo")),  solos[i], 1.0e-6f,
                                       "solo did not survive the round trip" + where);

            expectWithinAbsoluteError(processor.getLayer(i).pitchModel.getNode(0).value, pitches[i], 1.0e-6,
                                       "pitch envelope did not survive the round trip" + where);
            expectWithinAbsoluteError(processor.getLayer(i).ampModel.getLength(), lengths[i], 1.0e-9,
                                       "amp envelope Length did not survive the round trip" + where);
        }

        const auto renderReloaded = render(processor);

        double worstError = 0.0;
        int worstAt = -1;
        for (int i = 0; i < kNumSamples; ++i)
        {
            const double error = std::abs((double) renderA[(size_t) i] - (double) renderReloaded[(size_t) i]);
            if (error > worstError) { worstError = error; worstAt = i; }
        }

        expect(worstError < 1.0e-7,
               "Reloaded render does not null-match the original (worst error "
                 + juce::String(worstError) + " at sample " + juce::String(worstAt) + ")");

        // Guards the null-match above from passing on silence, which every mute/Off combination
        // here would happily produce if the mixer parameters had come back wrong.
        float peak = 0.0f;
        for (int i = 0; i < kNumSamples; ++i)
            peak = juce::jmax(peak, std::abs(renderA[(size_t) i]));

        expect(peak > 0.1f, "State A renders near-silence (peak " + juce::String(peak)
                              + "), so the null-match proves nothing");
    }

    // Builds a Phase 3 preset by hand rather than shipping a binary fixture: the expected bytes
    // stay readable in a diff, and the format is pinned by construction instead of by a blob
    // nobody can inspect. This is the layout preset1.doof was actually written in.
    static juce::MemoryBlock buildPhase3Preset()
    {
        juce::ValueTree root { juce::Identifier("DOOFState") };   // deliberately no version property

        juce::ValueTree apvtsState { juce::Identifier("DOOF_State") };
        juce::ValueTree gain { juce::Identifier("PARAM") };
        gain.setProperty("id", "sub.gain", nullptr);
        gain.setProperty("value", 0.55, nullptr);
        apvtsState.addChild(gain, -1, nullptr);
        root.addChild(apvtsState, -1, nullptr);

        // Both envelopes sat straight on the root, told apart only by curve.
        juce::ValueTree pitch { juce::Identifier("ENVELOPE") };
        pitch.setProperty("curve", "pitch", nullptr);
        pitch.setProperty("length", 0.2145988110107726, nullptr);
        pitch.addChild(makeNode(0.0, 111.0), -1, nullptr);
        pitch.addChild(makeNode(0.3,  44.0), -1, nullptr);
        root.addChild(pitch, -1, nullptr);

        // No length property at all, matching preset1.doof - it must fall back to kDefaultLength.
        juce::ValueTree amp { juce::Identifier("ENVELOPE") };
        amp.setProperty("curve", "amp", nullptr);
        amp.addChild(makeNode(0.0, 0.0), -1, nullptr);
        amp.addChild(makeNode(0.5, 1.0), -1, nullptr);
        root.addChild(amp, -1, nullptr);

        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*root.createXml(), block);
        return block;
    }

    static juce::ValueTree makeNode(double time, double value)
    {
        juce::ValueTree node { juce::Identifier("NODE") };
        node.setProperty("time", time, nullptr);
        node.setProperty("value", value, nullptr);
        node.setProperty("cpOutTime", time, nullptr);
        node.setProperty("cpOutValue", value, nullptr);
        node.setProperty("cpInTime", time, nullptr);
        node.setProperty("cpInValue", value, nullptr);
        return node;
    }

    void testPhase3PresetStillLoads()
    {
        beginTest("(b) A preset written before the mixer loads as the single-layer patch it described");

        DOOFAudioProcessor processor;

        // Dirtied first, so "reset to factory" is distinguishable from "never touched at all".
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"), (float) (int) ParamIDs::LayerType::sub);
            setParamValue(processor, layerId(i, "level"), 0.123f);
            setParamValue(processor, layerId(i, "solo"), 1.0f);
            processor.getLayer(i).pitchModel.moveNode(0, 0.0, 999.0);
            processor.getLayer(i).ampModel.setLength(1.234);
        }

        const auto preset = buildPhase3Preset();
        processor.setStateInformation(preset.getData(), (int) preset.getSize());

        // Layer 1 gets the preset's own envelopes.
        expectEquals(processor.getLayer(0).pitchModel.getNumNodes(), 2);
        expectWithinAbsoluteError(processor.getLayer(0).pitchModel.getNode(0).value, 111.0, 1.0e-9,
                                   "Layer 1 did not receive the preset's pitch envelope");
        expectWithinAbsoluteError(processor.getLayer(0).pitchModel.getLength(), 0.2145988110107726, 1.0e-12,
                                   "Layer 1 did not receive the preset's pitch Length");

        // The preset carries no Length for amp, so the default must apply rather than the 1.234
        // left over from the outgoing patch.
        expectWithinAbsoluteError(processor.getLayer(0).ampModel.getLength(),
                                   EnvelopeModel::kDefaultLength, 1.0e-9,
                                   "A missing Length must fall back to the default, not persist");

        expectWithinAbsoluteError(rawParam(processor, "sub.gain"), 0.55f, 1.0e-6f,
                                   "Master gain did not come from the preset");

        // The preset described one layer, so the mixer returns to factory rather than keeping the
        // outgoing patch - otherwise the same file would load differently every session.
        expectWithinAbsoluteError(rawParam(processor, "layer1.type"),
                                   (float) (int) ParamIDs::LayerType::sub, 1.0e-6f);
        expectWithinAbsoluteError(rawParam(processor, "layer1.level"), 1.0f, 1.0e-6f);
        expectWithinAbsoluteError(rawParam(processor, "layer1.solo"), 0.0f, 1.0e-6f);

        for (int i = 1; i < processor.getNumLayers(); ++i)
        {
            const auto where = " on layer " + juce::String(i + 1);

            expectWithinAbsoluteError(rawParam(processor, layerId(i, "type")),
                                       (float) (int) ParamIDs::LayerType::off, 1.0e-6f,
                                       "A pre-mixer preset must leave later layers Off" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "solo")), 0.0f, 1.0e-6f,
                                       "A pre-mixer preset must clear solo" + where);
            expectWithinAbsoluteError(processor.getLayer(i).pitchModel.getNode(0).value, 150.0, 1.0e-9,
                                       "A pre-mixer preset must reseed later layers" + where);
            expectWithinAbsoluteError(processor.getLayer(i).ampModel.getLength(),
                                       EnvelopeModel::kDefaultLength, 1.0e-9,
                                       "Reseeding must clear the stale Length" + where);
        }

        // A freshly loaded preset is not a user edit, so there must be nothing to undo back from.
        expect(! processor.getLayer(0).pitchModel.canUndo(),
               "Loading a preset left its own edits in the undo history");
    }
};

static Phase4StateTest phase4StateTest;



// ── Test runner entry point ────────────────────────────────────────────────────

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::UnitTestRunner runner;
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult(i)->failures;

    return failures > 0 ? 1 : 0;
}
