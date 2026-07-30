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
                                   "Editing layer 0 also changed layer 1 — the layers share state");

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
    }

private:
    static constexpr int kNumSamples = 4096;
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
                     + juce::String((int) a - 1) + " given the pitches assigned, but does not — "
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
};

static Phase4MixerTest phase4MixerTest;

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
