#include "PluginProcessor.h"
#include "ParamIDs.h"
#include "LayerAudibility.h"
#include "LayerViewPrefs.h"
#include "LayerStrip.h"
#include "EnvelopeCanvas.h"
#include "PluginEditor.h"

#include <cmath>
#include <cstring>
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
        testViewPrefsSurviveARoundTrip();
        testVersion2PresetLoadsWithDefaultViewPrefs();
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

    // Same hand-built approach as the Phase 3 blob below, one schema version on: LAYER children,
    // but none of the view-preference properties version 3 adds.
    static juce::MemoryBlock buildVersion2Preset()
    {
        juce::ValueTree root { juce::Identifier("DOOFState") };
        root.setProperty("version", 2, nullptr);

        juce::ValueTree apvtsState { juce::Identifier("DOOF_State") };
        juce::ValueTree gain { juce::Identifier("PARAM") };
        gain.setProperty("id", "sub.gain", nullptr);
        gain.setProperty("value", 0.65, nullptr);
        apvtsState.addChild(gain, -1, nullptr);
        root.addChild(apvtsState, -1, nullptr);

        for (int i = 0; i < LayerAudibility::kNumLayers; ++i)
        {
            juce::ValueTree layerNode { juce::Identifier("LAYER") };
            layerNode.setProperty("index", i, nullptr);

            juce::ValueTree pitch { juce::Identifier("ENVELOPE") };
            pitch.setProperty("curve", "pitch", nullptr);
            pitch.addChild(makeNode(0.0, 120.0 + 5.0 * i), -1, nullptr);
            pitch.addChild(makeNode(0.3,  40.0), -1, nullptr);
            layerNode.addChild(pitch, -1, nullptr);

            juce::ValueTree amp { juce::Identifier("ENVELOPE") };
            amp.setProperty("curve", "amp", nullptr);
            amp.addChild(makeNode(0.0, 0.0), -1, nullptr);
            amp.addChild(makeNode(0.5, 1.0), -1, nullptr);
            layerNode.addChild(amp, -1, nullptr);

            root.addChild(layerNode, -1, nullptr);
        }

        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*root.createXml(), block);
        return block;
    }

    void testViewPrefsSurviveARoundTrip()
    {
        beginTest("(c) Each layer's log scale and editing curve survive save and reload");

        DOOFAudioProcessor processor;

        // A different combination per layer, so a swap between layers cannot pass unnoticed and
        // neither can a single field being written for all five.
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            processor.getLayerViewPrefs(i).pitchLogScale = (i % 2 == 0);
            processor.getLayerViewPrefs(i).editingPitch  = (i < 2);
        }

        juce::MemoryBlock saved;
        processor.getStateInformation(saved);

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            processor.getLayerViewPrefs(i).pitchLogScale = false;
            processor.getLayerViewPrefs(i).editingPitch  = false;
        }

        processor.setStateInformation(saved.getData(), (int) saved.getSize());

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const auto where = " on layer " + juce::String(i + 1);
            expect(processor.getLayerViewPrefs(i).pitchLogScale == (i % 2 == 0),
                   "Log scale did not survive the round trip" + where);
            expect(processor.getLayerViewPrefs(i).editingPitch == (i < 2),
                   "Editing curve did not survive the round trip" + where);
        }
    }

    void testVersion2PresetLoadsWithDefaultViewPrefs()
    {
        beginTest("(d) A version 2 preset loads, taking the default view for every layer");

        DOOFAudioProcessor processor;

        // Dirtied first, so "took the default" is distinguishable from "was never touched".
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            processor.getLayerViewPrefs(i).pitchLogScale = true;
            processor.getLayerViewPrefs(i).editingPitch  = false;
        }

        const auto preset = buildVersion2Preset();
        processor.setStateInformation(preset.getData(), (int) preset.getSize());

        const LayerViewPrefs expected;

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const auto where = " on layer " + juce::String(i + 1);

            // The envelopes must still arrive, or this would pass on a preset that failed to load
            // at all rather than on one that loaded without view preferences.
            expectWithinAbsoluteError(processor.getLayer(i).pitchModel.getNode(0).value,
                                       120.0 + 5.0 * i, 1.0e-9,
                                       "A version 2 preset's envelopes did not load" + where);

            expect(processor.getLayerViewPrefs(i).pitchLogScale == expected.pitchLogScale,
                   "A version 2 preset should leave log scale at its default" + where);
            expect(processor.getLayerViewPrefs(i).editingPitch == expected.editingPitch,
                   "A version 2 preset should leave the editing curve at its default" + where);
        }

        expectWithinAbsoluteError(rawParam(processor, "sub.gain"), 0.65f, 1.0e-6f,
                                   "A version 2 preset's master gain did not load");
    }

    void testPhase3PresetStillLoads()
    {
        beginTest("(e) A preset written before the mixer loads as the single-layer patch it described");

        DOOFAudioProcessor processor;

        // Dirtied first, so "reset to factory" is distinguishable from "never touched at all".
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"), (float) (int) ParamIDs::LayerType::sub);
            setParamValue(processor, layerId(i, "level"), 0.123f);
            setParamValue(processor, layerId(i, "solo"), 1.0f);
            processor.getLayer(i).pitchModel.moveNode(0, 0.0, 999.0);
            processor.getLayer(i).ampModel.setLength(1.234);
            processor.getLayerViewPrefs(i).pitchLogScale = true;
            processor.getLayerViewPrefs(i).editingPitch  = false;
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

        // The view preferences go back to factory for the same reason the mixer parameters do:
        // the preset predates them entirely, so keeping the outgoing patch's would make the same
        // file load differently depending on what was open before it. Nothing resets these for
        // us - unlike the APVTS parameters, which JUCE defaults on replaceState.
        const LayerViewPrefs expectedPrefs;
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const auto where = " on layer " + juce::String(i + 1);
            expect(processor.getLayerViewPrefs(i).pitchLogScale == expectedPrefs.pitchLogScale,
                   "A pre-mixer preset must reset log scale" + where);
            expect(processor.getLayerViewPrefs(i).editingPitch == expectedPrefs.editingPitch,
                   "A pre-mixer preset must reset the editing curve" + where);
        }

        // A freshly loaded preset is not a user edit, so there must be nothing to undo back from.
        expect(! processor.getLayer(0).pitchModel.canUndo(),
               "Loading a preset left its own edits in the undo history");
    }
};

static Phase4StateTest phase4StateTest;

// Phase4LayerStripTest — that each strip drives its own layer's parameters, in both directions.
//
// The bug worth guarding against is an off-by-one: a strip is constructed with a zero-based index
// but the parameter IDs are one-based, so a strip wired to the neighbouring layer would look
// entirely normal on screen and only misbehave. Every check below therefore confirms both that
// the intended layer moved and that no other layer did.
class Phase4LayerStripTest : public juce::UnitTest
{
public:
    Phase4LayerStripTest() : juce::UnitTest("Phase4LayerStrip") {}

    void runTest() override
    {
        testStripsShowCurrentStateOnConstruction();
        testParameterMovesItsOwnStrip();
        testStripMovesItsOwnParameters();
        testSoloLightsAndDimsTheOtherStrips();
    }

private:
    static juce::String layerId(int index, const juce::String& suffix)
    {
        return "layer" + juce::String(index + 1) + "." + suffix;
    }

    static float rawParam(DOOFAudioProcessor& processor, const juce::String& id)
    {
        auto* value = processor.apvts.getRawParameterValue(id);
        return value != nullptr ? value->load() : std::numeric_limits<float>::quiet_NaN();
    }

    static void setParamValue(DOOFAudioProcessor& processor, const juce::String& id, float value)
    {
        auto* param = processor.apvts.getParameter(id);
        jassert(param != nullptr);
        param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    // Reaches a control by the component ID the strip publishes, so this test does not force
    // LayerStrip to expose its children just to be testable.
    template <typename ControlType>
    static ControlType* control(LayerStrip& strip, const char* componentID)
    {
        return dynamic_cast<ControlType*>(strip.findChildWithID(componentID));
    }

    static std::vector<std::unique_ptr<LayerStrip>> makeStrips(DOOFAudioProcessor& processor)
    {
        std::vector<std::unique_ptr<LayerStrip>> strips;
        for (int i = 0; i < processor.getNumLayers(); ++i)
            strips.push_back(std::make_unique<LayerStrip>(processor.apvts, i));
        return strips;
    }

    // The editor is created after the state already exists - on plugin open, on preset load, on
    // reopening a saved session - so a strip has to show the current values immediately rather
    // than only once something moves. A combo box populated after its attachment was made, for
    // instance, ends up blank until the parameter next changes, which nothing else here notices.
    void testStripsShowCurrentStateOnConstruction()
    {
        beginTest("(a) A strip shows its layer's current values the moment it is built");

        DOOFAudioProcessor processor;

        // Set before the strips exist, and deliberately not the defaults, so this cannot pass by
        // a control simply happening to start where the parameter did.
        setParamValue(processor, layerId(2, "type"),  (float) (int) ParamIDs::LayerType::click);
        setParamValue(processor, layerId(2, "level"), 0.61f);
        setParamValue(processor, layerId(2, "mute"),  1.0f);

        auto strips = makeStrips(processor);

        auto* type  = control<juce::ComboBox>  (*strips[2], LayerStrip::typeBoxID);
        auto* level = control<juce::Slider>    (*strips[2], LayerStrip::levelSliderID);
        auto* mute  = control<juce::TextButton>(*strips[2], LayerStrip::muteButtonID);

        expect(type != nullptr && level != nullptr && mute != nullptr, "A strip is missing a control");
        if (type == nullptr || level == nullptr || mute == nullptr)
            return;

        expectEquals(type->getSelectedItemIndex(), (int) ParamIDs::LayerType::click,
                     "Type combo did not show the existing value on construction");
        expectWithinAbsoluteError(level->getValue(), 0.61, 1.0e-5,
                                   "Level slider did not show the existing value on construction");
        expect(mute->getToggleState(), "Mute button did not show the existing value on construction");

        // The out-of-the-box patch must show up correctly too, since that is what a user opening
        // the plugin for the first time actually sees.
        auto* firstType = control<juce::ComboBox>(*strips[0], LayerStrip::typeBoxID);
        expectEquals(firstType->getSelectedItemIndex(), (int) ParamIDs::LayerType::sub,
                     "Layer 1's type combo does not show Sub on a fresh instance");

        for (int i = 1; i < processor.getNumLayers(); ++i)
            if (i != 2)
                expectEquals(control<juce::ComboBox>(*strips[(size_t) i], LayerStrip::typeBoxID)
                               ->getSelectedItemIndex(),
                             (int) ParamIDs::LayerType::off,
                             "Layer " + juce::String(i + 1) + "'s type combo does not show Off");
    }

    void testParameterMovesItsOwnStrip()
    {
        beginTest("(b) Moving a layer's parameter moves that strip's controls and no other's");

        DOOFAudioProcessor processor;
        auto strips = makeStrips(processor);

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            setParamValue(processor, layerId(i, "type"),  (float) (int) ParamIDs::LayerType::click);
            setParamValue(processor, layerId(i, "level"), 0.33f);
            setParamValue(processor, layerId(i, "mute"),  1.0f);
            setParamValue(processor, layerId(i, "solo"),  1.0f);

            for (int j = 0; j < processor.getNumLayers(); ++j)
            {
                const auto where = " (moved layer " + juce::String(i + 1)
                                     + ", inspected strip " + juce::String(j + 1) + ")";

                auto* type  = control<juce::ComboBox>  (*strips[(size_t) j], LayerStrip::typeBoxID);
                auto* level = control<juce::Slider>    (*strips[(size_t) j], LayerStrip::levelSliderID);
                auto* mute  = control<juce::TextButton>(*strips[(size_t) j], LayerStrip::muteButtonID);
                auto* solo  = control<juce::TextButton>(*strips[(size_t) j], LayerStrip::soloButtonID);

                expect(type != nullptr && level != nullptr && mute != nullptr && solo != nullptr,
                       "A strip is missing one of its controls" + where);
                if (type == nullptr || level == nullptr || mute == nullptr || solo == nullptr)
                    return;

                if (i == j)
                {
                    expectEquals(type->getSelectedItemIndex(), (int) ParamIDs::LayerType::click,
                                 "Type combo did not follow its parameter" + where);
                    expectWithinAbsoluteError(level->getValue(), 0.33, 1.0e-5,
                                               "Level slider did not follow its parameter" + where);
                    expect(mute->getToggleState(), "Mute button did not follow its parameter" + where);
                    expect(solo->getToggleState(), "Solo button did not follow its parameter" + where);
                }
                else
                {
                    expect(type->getSelectedItemIndex() != (int) ParamIDs::LayerType::click,
                           "Strip followed another layer's type, so it is wired to the wrong layer" + where);
                    expect(! mute->getToggleState(),
                           "Strip followed another layer's mute, so it is wired to the wrong layer" + where);
                    expect(! solo->getToggleState(),
                           "Strip followed another layer's solo, so it is wired to the wrong layer" + where);
                }
            }

            // Restored so the next iteration's "every other strip is at its default" still holds.
            setParamValue(processor, layerId(i, "type"),
                          (float) (i == 0 ? (int) ParamIDs::LayerType::sub
                                          : (int) ParamIDs::LayerType::off));
            setParamValue(processor, layerId(i, "level"), 1.0f);
            setParamValue(processor, layerId(i, "mute"),  0.0f);
            setParamValue(processor, layerId(i, "solo"),  0.0f);
        }
    }

    void testStripMovesItsOwnParameters()
    {
        beginTest("(c) Operating a strip's controls moves that layer's parameters and no other's");

        DOOFAudioProcessor processor;
        auto strips = makeStrips(processor);

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            auto* type  = control<juce::ComboBox>  (*strips[(size_t) i], LayerStrip::typeBoxID);
            auto* level = control<juce::Slider>    (*strips[(size_t) i], LayerStrip::levelSliderID);
            auto* solo  = control<juce::TextButton>(*strips[(size_t) i], LayerStrip::soloButtonID);

            expect(type != nullptr && level != nullptr && solo != nullptr, "A strip is missing a control");
            if (type == nullptr || level == nullptr || solo == nullptr)
                return;

            // sendNotificationSync, so the attachment has written through before we read back.
            type->setSelectedItemIndex((int) ParamIDs::LayerType::click, juce::sendNotificationSync);
            level->setValue(0.42, juce::sendNotificationSync);
            solo->setToggleState(true, juce::sendNotificationSync);

            const auto where = " (drove strip " + juce::String(i + 1) + ")";

            expectWithinAbsoluteError(rawParam(processor, layerId(i, "type")),
                                       (float) (int) ParamIDs::LayerType::click, 1.0e-6f,
                                       "Type combo did not reach its parameter" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "level")), 0.42f, 1.0e-5f,
                                       "Level slider did not reach its parameter" + where);
            expectWithinAbsoluteError(rawParam(processor, layerId(i, "solo")), 1.0f, 1.0e-6f,
                                       "Solo button did not reach its parameter" + where);

            for (int j = 0; j < processor.getNumLayers(); ++j)
                if (j != i)
                    expectWithinAbsoluteError(rawParam(processor, layerId(j, "level")), 1.0f, 1.0e-6f,
                                               "Driving one strip changed layer " + juce::String(j + 1)
                                                 + "'s level" + where);

            type->setSelectedItemIndex(i == 0 ? (int) ParamIDs::LayerType::sub
                                              : (int) ParamIDs::LayerType::off, juce::sendNotificationSync);
            level->setValue(1.0, juce::sendNotificationSync);
            solo->setToggleState(false, juce::sendNotificationSync);
        }
    }

    // §6's GUI check: "solo layers 1 and 3 - only those are audible, the rest visibly dim".
    // Driven through refreshAppearance() rather than the poll timer, since a unit test has no
    // message loop running; the timer's only job is to call this, which (a) already proves is
    // wired by showing the correct appearance the moment a strip is built.
    void testSoloLightsAndDimsTheOtherStrips()
    {
        beginTest("(d) Soloing layers 1 and 3 lights those two and dims the other Sub layers");

        DOOFAudioProcessor processor;

        // All five audible to start, so any dimming that appears is caused by solo alone.
        for (int i = 0; i < processor.getNumLayers(); ++i)
            setParamValue(processor, layerId(i, "type"), (float) (int) ParamIDs::LayerType::sub);

        auto strips = makeStrips(processor);
        auto refreshAll = [&strips] { for (auto& strip : strips) strip->refreshAppearance(); };

        refreshAll();
        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            expect(! strips[(size_t) i]->isSoloLit(),
                   "Layer " + juce::String(i + 1) + " is lit with nothing soloed");
            expect(! strips[(size_t) i]->isDimmed(),
                   "Layer " + juce::String(i + 1) + " is dimmed with nothing soloed");
        }

        setParamValue(processor, layerId(0, "solo"), 1.0f);
        setParamValue(processor, layerId(2, "solo"), 1.0f);
        refreshAll();

        for (int i = 0; i < processor.getNumLayers(); ++i)
        {
            const bool soloed = (i == 0 || i == 2);
            const auto where  = " on layer " + juce::String(i + 1);

            expect(strips[(size_t) i]->isSoloLit() == soloed,
                   juce::String(soloed ? "A soloed layer is not lit" : "A non-soloed layer is lit") + where);
            expect(strips[(size_t) i]->isDimmed() == ! soloed,
                   juce::String(soloed ? "A soloed layer is dimmed"
                                       : "A layer silenced by another's solo is not dimmed") + where);
        }

        // Muting a soloed layer silences it, but it is silent for its own reason and keeps its
        // solo, so it stays lit and must not dim - dimming is reserved for having no local cause.
        setParamValue(processor, layerId(0, "mute"), 1.0f);
        refreshAll();

        expect(strips[0]->isSoloLit(), "A soloed layer stopped being lit once it was also muted");
        expect(! strips[0]->isDimmed(), "A muted layer was dimmed, but its mute is the visible cause");

        // An Off layer is not in the mix, so it neither lights nor dims however solo is set.
        setParamValue(processor, layerId(4, "type"), (float) (int) ParamIDs::LayerType::off);
        setParamValue(processor, layerId(4, "solo"), 1.0f);
        refreshAll();

        expect(! strips[4]->isSoloLit(), "An Off layer lit up when soloed");
        expect(! strips[4]->isDimmed(), "An Off layer was dimmed");

        // With every solo cleared the whole strip returns to plain, so dimming is genuinely tied
        // to the solo state and not latched by the first refresh that set it.
        for (int i = 0; i < processor.getNumLayers(); ++i)
            setParamValue(processor, layerId(i, "solo"), 0.0f);
        refreshAll();

        for (int i = 0; i < processor.getNumLayers(); ++i)
            expect(! strips[(size_t) i]->isDimmed() && ! strips[(size_t) i]->isSoloLit(),
                   "Layer " + juce::String(i + 1) + " did not return to plain once solo was cleared");
    }
};

static Phase4LayerStripTest phase4LayerStripTest;

// Phase4CanvasRetargetTest — selecting a layer points the canvas at that layer's curves, and
// doing so mid-drag does not leave an envelope model stuck inside an undo transaction.
//
// That second half is the reason setModels cancels first. A gesture is begun in mouseDown on one
// model and ended in mouseUp on whatever the canvas is pointing at by then, so a naive swap
// orphans the open transaction on the outgoing model - after which every later edit on that layer
// silently coalesces into it, and one undo throws away an unbounded amount of work.
class Phase4CanvasRetargetTest : public juce::UnitTest
{
public:
    Phase4CanvasRetargetTest() : juce::UnitTest("Phase4CanvasRetarget") {}

    void runTest() override
    {
        testSelectingALayerRetargetsTheCanvas();
        testSwappingMidDragLeavesNoOpenTransaction();
        testSwitchingLayersWhileANoteRingsDoesNotDisturbTheAudio();
    }

private:
    // The canvas maps pitch onto the range [20, 220] over a 472 px height, so the seeded first
    // node at (t=0, 150 Hz) lands here. Computed rather than hunted for, so a mapping change
    // shows up as this test missing the node instead of quietly hitting something else.
    static juce::Point<float> pitchNodeZeroPixel() { return { 0.0f, 165.2f }; }

    static juce::MouseEvent mouseEventAt(juce::Component* c, juce::Point<float> position,
                                          int clicks = 1, bool wasDragged = false)
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position,
                                 juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c, c, juce::Time::getCurrentTime(), position,
                                 juce::Time::getCurrentTime(), clicks, wasDragged);
    }

    template <typename ChildType>
    static juce::Array<ChildType*> childrenOfType(juce::Component& parent)
    {
        juce::Array<ChildType*> found;
        for (auto* child : parent.getChildren())
            if (auto* typed = dynamic_cast<ChildType*>(child))
                found.add(typed);
        return found;
    }

    void testSelectingALayerRetargetsTheCanvas()
    {
        beginTest("(a) Selecting a layer points the canvas at that layer's curves");

        DOOFAudioProcessor processor;
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        editor->setBounds(0, 0, 800, 600);

        auto canvases = childrenOfType<EnvelopeCanvas>(*editor);
        auto strips   = childrenOfType<LayerStrip>(*editor);

        expect(canvases.size() == 1 && strips.size() == processor.getNumLayers(),
               "Editor does not hold one canvas and one strip per layer");
        if (canvases.size() != 1 || strips.size() != processor.getNumLayers())
            return;

        const int before1 = processor.getLayer(0).pitchModel.getNumNodes();
        const int before3 = processor.getLayer(2).pitchModel.getNumNodes();

        // Select layer 3 the way a user would, then add a node on the canvas.
        strips[2]->mouseDown(mouseEventAt(strips[2], {}));
        canvases[0]->mouseDoubleClick(mouseEventAt(canvases[0], { 300.0f, 200.0f }, 2));

        expectEquals(processor.getLayer(2).pitchModel.getNumNodes(), before3 + 1,
                     "Editing the canvas after selecting layer 3 did not reach layer 3");
        expectEquals(processor.getLayer(0).pitchModel.getNumNodes(), before1,
                     "Editing the canvas after selecting layer 3 still reached layer 1");
    }

    // §6's "click between layers while a note rings - no audio dropout". Stated as a null test
    // rather than as a click/glitch threshold: selecting a layer is a GUI action that must not
    // reach the audio thread at all, so the rendered block should come out bit-identical to the
    // same render with nobody touching the interface. Anything weaker would tolerate a small
    // disturbance, and there is no reason to allow one.
    void testSwitchingLayersWhileANoteRingsDoesNotDisturbTheAudio()
    {
        beginTest("(c) Clicking between layers while a note rings leaves the audio untouched");

        static constexpr int blockSize  = 256;
        static constexpr int numBlocks  = 16;
        static constexpr double sampleRate = 44100.0;

        // withSwitching renders the same note while clicking through the layer strips between
        // blocks, exactly as a user auditioning layers mid-tail would.
        auto render = [](bool withSwitching)
        {
            DOOFAudioProcessor processor;
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            editor->setBounds(0, 0, 800, 600);

            auto strips = childrenOfType<LayerStrip>(*editor);

            processor.prepareToPlay(sampleRate, blockSize);

            std::vector<float> out;
            juce::AudioBuffer<float> buffer(2, blockSize);

            for (int block = 0; block < numBlocks; ++block)
            {
                if (withSwitching && strips.size() > 0)
                {
                    auto* strip = strips[block % strips.size()];
                    strip->mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                                                       {}, juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                                       strip, strip, juce::Time::getCurrentTime(), {},
                                                       juce::Time::getCurrentTime(), 1, false));
                }

                juce::MidiBuffer midi;
                if (block == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);

                processor.processBlock(buffer, midi);

                const auto* channel = buffer.getReadPointer(0);
                for (int i = 0; i < blockSize; ++i)
                    out.push_back(channel[i]);
            }

            return out;
        };

        const auto quiet    = render(false);
        const auto switched = render(true);

        expectEquals((int) switched.size(), (int) quiet.size(), "Renders are different lengths");
        if (switched.size() != quiet.size())
            return;

        double worstDifference = 0.0;
        int worstAt = -1;
        for (size_t i = 0; i < quiet.size(); ++i)
        {
            const double difference = std::abs((double) switched[i] - (double) quiet[i]);
            if (difference > worstDifference) { worstDifference = difference; worstAt = (int) i; }
        }

        expect(worstDifference == 0.0,
               "Switching layers changed the audio (worst difference " + juce::String(worstDifference)
                 + " at sample " + juce::String(worstAt) + "), so a GUI action is reaching the "
                   "audio thread");

        // Guards against both renders being silent, which would satisfy the comparison above
        // while proving nothing about dropouts.
        float peak = 0.0f;
        for (const auto sample : quiet)
            peak = juce::jmax(peak, std::abs(sample));

        expect(peak > 0.1f, "The reference render is near-silent (peak " + juce::String(peak)
                              + "), so there was no ringing note to interrupt");
    }

    void testSwappingMidDragLeavesNoOpenTransaction()
    {
        beginTest("(b) Switching layers mid-drag leaves the outgoing model's undo history intact");

        DOOFAudioProcessor processor;
        std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
        editor->setBounds(0, 0, 800, 600);

        auto canvases = childrenOfType<EnvelopeCanvas>(*editor);
        auto strips   = childrenOfType<LayerStrip>(*editor);
        if (canvases.size() != 1 || strips.size() != processor.getNumLayers())
            return;

        auto& pitch = processor.getLayer(0).pitchModel;
        const double originalValue = pitch.getNode(0).value;

        // Begin a node drag on layer 1 and leave it open.
        canvases[0]->mouseDown(mouseEventAt(canvases[0], pitchNodeZeroPixel()));
        canvases[0]->mouseDrag(mouseEventAt(canvases[0], { 40.0f, 220.0f }, 1, true));

        // Without this the rest of the test is vacuous: if the press missed the node handle no
        // gesture was ever opened, and there would be nothing for the swap to mishandle.
        expect(std::abs(pitch.getNode(0).value - originalValue) > 1.0e-9,
               "The drag never moved a node, so no gesture was open and this test proves nothing");

        // Switch layers with the drag still in progress.
        strips[2]->mouseDown(mouseEventAt(strips[2], {}));

        // Two further edits on the layer just left. If its transaction were still open they would
        // both fall inside it, and one undo would discard both plus the drag.
        const int afterSwap = pitch.getNumNodes();
        pitch.addNode(0.11, 100.0);
        pitch.addNode(0.12, 101.0);
        expectEquals(pitch.getNumNodes(), afterSwap + 2, "The two edits did not both land");

        pitch.undo();
        expectEquals(pitch.getNumNodes(), afterSwap + 1,
                     "One undo removed more than the last edit, so the drag's transaction was "
                     "left open when the canvas switched layers");
    }
};

static Phase4CanvasRetargetTest phase4CanvasRetargetTest;

// Phase4ContextualPanelTest — §4.6: the contextual panel swaps with the selected layer's type.
// Sub shows the canvas and its toolbar; Click and Off replace both with a placeholder.
//
// Subtest (c) is the one worth having. The type can move with no click anywhere in this editor -
// host automation and preset loads both do it - so the panel is driven by a poll, and a poll that
// was never started fails in exactly the way the click path cannot reveal.
class Phase4ContextualPanelTest : public juce::UnitTest
{
public:
    Phase4ContextualPanelTest() : juce::UnitTest("Phase4ContextualPanel") {}

    void runTest() override
    {
        testPanelFollowsTheSelectedLayersType();
        testToolbarHidesWithTheCanvas();
        testToolbarFollowsTheSelectedLayer();
        testLengthFieldsFollowTheSelectedLayer();
        testAutomatingTheTypeSwapsThePanelWithoutAClick();
    }

private:
    static void setParamValue(DOOFAudioProcessor& processor, const juce::String& id, float value)
    {
        auto* param = processor.apvts.getParameter(id);
        jassert(param != nullptr);
        param->setValueNotifyingHost(param->convertTo0to1(value));
    }

    template <typename ChildType>
    static juce::Array<ChildType*> childrenOfType(juce::Component& parent)
    {
        juce::Array<ChildType*> found;
        for (auto* child : parent.getChildren())
            if (auto* typed = dynamic_cast<ChildType*>(child))
                found.add(typed);
        return found;
    }

    struct Editor
    {
        DOOFAudioProcessor processor;
        std::unique_ptr<juce::AudioProcessorEditor> editor;
        EnvelopeCanvas* canvas = nullptr;
        juce::Label* placeholder = nullptr;
        juce::Array<LayerStrip*> strips;
    };

    // Built after the caller has set any parameters it needs, since the editor reads the state
    // it is constructed against.
    static void open(Editor& e)
    {
        e.editor.reset(e.processor.createEditor());
        e.editor->setBounds(0, 0, 800, 600);
        e.canvas = childrenOfType<EnvelopeCanvas>(*e.editor).getFirst();
        e.strips = childrenOfType<LayerStrip>(*e.editor);
        e.placeholder = dynamic_cast<juce::Label*>(
            e.editor->findChildWithID(DOOFAudioProcessorEditor::placeholderID));
    }

    static void clickStrip(Editor& e, int index)
    {
        auto* strip = e.strips[index];
        strip->mouseDown(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {},
                                           juce::ModifierKeys(), 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           strip, strip, juce::Time::getCurrentTime(), {},
                                           juce::Time::getCurrentTime(), 1, false));
    }

    void testPanelFollowsTheSelectedLayersType()
    {
        beginTest("(a) Sub shows the canvas; Click and Off replace it with a placeholder");

        Editor e;
        setParamValue(e.processor, "layer2.type", (float) (int) ParamIDs::LayerType::click);
        open(e);

        expect(e.canvas != nullptr && e.placeholder != nullptr && e.strips.size() == 5,
               "Editor is missing the canvas, the placeholder or the strips");
        if (e.canvas == nullptr || e.placeholder == nullptr || e.strips.size() != 5)
            return;

        // Layer 1 is Sub out of the box, and is selected on open.
        expect(e.canvas->isVisible(), "The canvas is hidden for a Sub layer");
        expect(! e.placeholder->isVisible(), "The placeholder is showing over a Sub layer");

        clickStrip(e, 1);
        expect(! e.canvas->isVisible(), "The canvas is still showing for a Click layer");
        expect(e.placeholder->isVisible(), "No placeholder for a Click layer");
        expect(e.placeholder->getText().containsIgnoreCase("click"),
               "The Click placeholder does not mention the layer type, it says: "
                 + e.placeholder->getText());

        clickStrip(e, 2);
        expect(! e.canvas->isVisible(), "The canvas is still showing for an Off layer");
        expect(e.placeholder->getText().containsIgnoreCase("off"),
               "The Off placeholder does not say the layer is off, it says: "
                 + e.placeholder->getText());

        clickStrip(e, 0);
        expect(e.canvas->isVisible(), "The canvas did not come back on returning to a Sub layer");
        expect(! e.placeholder->isVisible(), "The placeholder outlived the return to a Sub layer");
    }

    void testToolbarHidesWithTheCanvas()
    {
        beginTest("(b) The canvas toolbar hides with the canvas, but the global controls stay");

        Editor e;
        open(e);
        if (e.canvas == nullptr)
            return;

        // Every button and combo in the editor, by their visible text, before and after.
        auto visibleTexts = [&e]
        {
            juce::StringArray names;
            for (auto* child : e.editor->getChildren())
                if (child->isVisible())
                    if (auto* button = dynamic_cast<juce::Button*>(child))
                        names.add(button->getButtonText());
            return names;
        };

        const auto whenSub = visibleTexts();
        expect(whenSub.contains("Log Scale (Pitch)"), "The log toggle is missing for a Sub layer");
        expect(whenSub.contains("Undo") && whenSub.contains("Save..."),
               "The global controls are missing for a Sub layer");

        clickStrip(e, 2); // an Off layer
        const auto whenOff = visibleTexts();

        expect(! whenOff.contains("Log Scale (Pitch)"),
               "The log toggle still edits a curve that is no longer on screen");

        // These are not per-layer, so hiding them would be wrong: undo spans every layer's edits
        // and the preset buttons act on the whole patch.
        expect(whenOff.contains("Undo") && whenOff.contains("Redo"),
               "Undo/redo were hidden with the canvas, but they are global");
        expect(whenOff.contains("Save...") && whenOff.contains("Load..."),
               "The preset buttons were hidden with the canvas, but they are global");
    }

    // Finds a control by the text it displays, since the editor publishes a component ID only for
    // the placeholder. Buttons and combo boxes are named distinctly enough for this to be exact.
    template <typename ControlType>
    static ControlType* controlNamed(juce::Component& parent, const juce::String& name)
    {
        for (auto* child : parent.getChildren())
            if (auto* typed = dynamic_cast<ControlType*>(child))
                if (typed->getName() == name || typed->getComponentID() == name)
                    return typed;
        return nullptr;
    }

    void testToolbarFollowsTheSelectedLayer()
    {
        beginTest("(d) The log toggle and editing curve are remembered per layer");

        Editor e;
        // Every layer Sub, so the toolbar is visible whichever one is selected.
        for (int i = 0; i < e.processor.getNumLayers(); ++i)
            setParamValue(e.processor, "layer" + juce::String(i + 1) + ".type",
                          (float) (int) ParamIDs::LayerType::sub);
        open(e);

        auto* logToggle = controlNamed<juce::ToggleButton>(*e.editor, "Log Scale (Pitch)");
        expect(logToggle != nullptr, "Could not find the log toggle");
        if (logToggle == nullptr)
            return;

        // Layer 1: turn log on, the way a user would.
        logToggle->setToggleState(true, juce::sendNotificationSync);
        expect(e.processor.getLayerViewPrefs(0).pitchLogScale,
               "Toggling log did not reach the selected layer's preferences");

        // Layer 2 has never been touched, so it must show the default rather than layer 1's.
        clickStrip(e, 1);
        expect(! logToggle->getToggleState(),
               "Switching layers carried the previous layer's log setting across");
        expect(! e.processor.getLayerViewPrefs(1).pitchLogScale,
               "Merely selecting a layer wrote a preference onto it");

        // The write-back is the hazard: pushing a preference into the toggle fires its callback
        // unless suppressed, which would have layer 2 overwrite itself with layer 1's value.
        expect(e.processor.getLayerViewPrefs(0).pitchLogScale,
               "Selecting layer 2 clobbered layer 1's stored log setting");

        // And back again restores it.
        clickStrip(e, 0);
        expect(logToggle->getToggleState(),
               "Returning to layer 1 did not restore its log setting");
    }

    void testLengthFieldsFollowTheSelectedLayer()
    {
        beginTest("(e) The Length fields read and write the selected layer's models");

        Editor e;
        for (int i = 0; i < e.processor.getNumLayers(); ++i)
            setParamValue(e.processor, "layer" + juce::String(i + 1) + ".type",
                          (float) (int) ParamIDs::LayerType::sub);

        // Distinct lengths set before the editor exists, so the fields must read them on open.
        e.processor.getLayer(0).pitchModel.setLength(0.11);
        e.processor.getLayer(2).pitchModel.setLength(0.33);
        open(e);

        auto* pitchLength = controlNamed<juce::Label>(*e.editor, "");
        juce::Array<juce::Label*> editableLabels;
        for (auto* child : e.editor->getChildren())
            if (auto* label = dynamic_cast<juce::Label*>(child))
                if (label->isEditable())
                    editableLabels.add(label);

        expect(editableLabels.size() == 2, "Expected exactly two editable Length fields, found "
                                             + juce::String(editableLabels.size()));
        if (editableLabels.size() != 2)
            return;

        pitchLength = editableLabels[0];

        expectWithinAbsoluteError(pitchLength->getText().getDoubleValue(), 0.11, 1.0e-3,
                                   "The pitch Length field did not show layer 1's length on open");

        clickStrip(e, 2);
        expectWithinAbsoluteError(pitchLength->getText().getDoubleValue(), 0.33, 1.0e-3,
                                   "The pitch Length field did not follow the selection to layer 3");

        // Editing the field must reach layer 3, and must leave layer 1 alone.
        pitchLength->setText("0.44", juce::sendNotificationSync);
        expectWithinAbsoluteError(e.processor.getLayer(2).pitchModel.getLength(), 0.44, 1.0e-9,
                                   "Editing the Length field did not reach the selected layer");
        expectWithinAbsoluteError(e.processor.getLayer(0).pitchModel.getLength(), 0.11, 1.0e-9,
                                   "Editing the Length field reached layer 1 as well");
    }

    void testAutomatingTheTypeSwapsThePanelWithoutAClick()
    {
        beginTest("(f) Automating the selected layer's type swaps the panel with no click");

        Editor e;
        open(e);
        if (e.canvas == nullptr || e.placeholder == nullptr)
            return;

        expect(e.canvas->isVisible(), "The canvas should start visible on the default Sub layer");

        // Exactly what a host automating the parameter does: nothing touches the editor.
        setParamValue(e.processor, "layer1.type", (float) (int) ParamIDs::LayerType::off);

        expect(e.canvas->isVisible(),
               "The panel changed before the poll could have run, so this test is not measuring "
               "the poll at all");

        // Long enough for a 30 Hz timer to have fired several times.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(200);

        expect(! e.canvas->isVisible(),
               "The canvas is still showing after the layer was automated to Off, so the poll "
               "never ran");
        expect(e.placeholder->isVisible() && e.placeholder->getText().containsIgnoreCase("off"),
               "The placeholder did not appear after the layer was automated to Off");
    }
};

static Phase4ContextualPanelTest phase4ContextualPanelTest;

// Phase5MidiDispatchTest — Phase 5 §6 Verify: "Render and assert the click onset is sample-aligned
// to note-on." Pins the sample-accurate dispatch added in Step 1a, which replaced a loop that
// drained the whole MidiBuffer before rendering anything and so fired every note-on at sample 0 —
// up to 11 ms early on a 512-sample block. Inaudible on the sub's slow attack, which is how it
// survived four phases; audible timing slop on a click transient.
//
// Deliberately not phrased as "the first non-zero sample is at N". SubVoice::startBody sets
// phase = 0, so sin(0) = 0 makes the sample at the note-on itself exactly zero and the first
// audible one land at N+1 — an assertion on the first non-zero index would encode that off-by-one
// and would break the moment a click type starts on a non-zero sample. The two halves asserted
// here say the same thing exactly and without depending on any voice's starting waveform:
//   1. every sample before the event is exactly zero, and
//   2. from the event onward the render is bit-identical to the same note taken at sample 0.
class Phase5MidiDispatchTest : public juce::UnitTest
{
public:
    Phase5MidiDispatchTest() : juce::UnitTest("Phase5MidiDispatch") {}

    void runTest() override
    {
        testNoteOnSoundsAtItsSamplePosition();
        testLaterEventDoesNotReachBackwards();
    }

private:
    static constexpr double kSampleRate = 44100.0;
    static constexpr int kBlockSize = 512;
    static constexpr int kTail = 4096;

    // Renders `totalSamples` of the default patch in kBlockSize blocks, placing a note-on at the
    // given absolute sample offset. prepareToPlay is called per render, so the voices start Idle
    // and the gain smoothers sit on their targets — a ramp in one render but not another would
    // show up as a mismatch that has nothing to do with dispatch timing.
    static std::vector<float> renderWithNoteAt(int offset, int totalSamples)
    {
        DOOFAudioProcessor processor;
        processor.prepareToPlay(kSampleRate, kBlockSize);

        std::vector<float> out;
        out.reserve((size_t) totalSamples);

        juce::AudioBuffer<float> buffer(2, kBlockSize);
        for (int blockStart = 0; blockStart < totalSamples; blockStart += kBlockSize)
        {
            juce::MidiBuffer midi;
            if (offset >= blockStart && offset < blockStart + kBlockSize)
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), offset - blockStart);

            processor.processBlock(buffer, midi);

            const auto* channel = buffer.getReadPointer(0);
            for (int i = 0; i < kBlockSize; ++i)
                out.push_back(channel[i]);
        }

        return out;
    }

    // First index at which the two spans differ bit-for-bit, or -1 if they are identical.
    // Compared as raw bytes because bit-identical is the actual claim, and it also keeps the
    // compiler's float-equality warning out of the build.
    static int firstDifference(const float* a, const float* b, int count)
    {
        for (int i = 0; i < count; ++i)
            if (std::memcmp(a + i, b + i, sizeof(float)) != 0)
                return i;

        return -1;
    }

    // The core check: a note-on at sample N is silent before N and, from N onward, produces
    // exactly the render it would have produced had it arrived at sample 0.
    void testNoteOnSoundsAtItsSamplePosition()
    {
        beginTest("A note-on at sample N sounds at sample N, not at the start of the block");

        const auto reference = renderWithNoteAt(0, kTail);

        // 1 and 511 sit at the extremes of a block; 37 and 128 are ordinary interior positions.
        // 1 is included even though it can only prove one sample of silence, because it is the
        // offset most likely to be lost to an off-by-one in the cursor arithmetic.
        for (const int offset : { 1, 37, 128, 511 })
        {
            const auto shifted = renderWithNoteAt(offset, kTail + kBlockSize);

            const std::vector<float> silence((size_t) offset, 0.0f);
            const int soundedEarly = firstDifference(shifted.data(), silence.data(), offset);
            expect(soundedEarly < 0,
                   "Note-on at sample " + juce::String(offset) + " produced output at sample "
                       + juce::String(soundedEarly) + ", before the event");

            const int diverged = firstDifference(shifted.data() + offset, reference.data(), kTail);
            expect(diverged < 0,
                   "Note-on at sample " + juce::String(offset)
                       + " does not render identically to the same note at sample 0; they first "
                         "differ "
                       + juce::String(diverged) + " samples after the note-on");
        }
    }

    // A second event later in the same block must not disturb what was already rendered before it.
    // Under the old drain-the-buffer-first dispatch both note-ons landed on sample 0, so the first
    // note never sounded at all: the second arrived while the voice was still at envTime 0 and
    // choked it immediately. Rendering in spans is what makes the first note's span independent.
    void testLaterEventDoesNotReachBackwards()
    {
        beginTest("A second note-on later in the block leaves the samples before it untouched");

        const auto single = renderWithNoteAt(0, kTail);

        for (const int secondOffset : { 64, 300 })
        {
            DOOFAudioProcessor processor;
            processor.prepareToPlay(kSampleRate, kBlockSize);

            juce::AudioBuffer<float> buffer(2, kBlockSize);
            juce::MidiBuffer midi;
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), secondOffset);
            processor.processBlock(buffer, midi);

            const int diverged = firstDifference(buffer.getReadPointer(0), single.data(), secondOffset);
            expect(diverged < 0,
                   "A note-on at sample " + juce::String(secondOffset)
                       + " changed sample " + juce::String(diverged)
                       + ", which was rendered before it arrived");

            // Without this the test above would still pass on a buffer that had gone silent, which
            // is exactly the failure the old dispatch produced.
            expect(single[(size_t) secondOffset / 2] != 0.0f,
                   "The first note is silent halfway to the second event, so the check above is "
                   "comparing silence against silence and proving nothing");
        }
    }
};

static Phase5MidiDispatchTest phase5MidiDispatchTest;




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
