#include <juce_core/juce_core.h>
#include "SubVoice.h"
#include "EnvelopeModel.h"
#include "EnvelopeEvaluator.h"
#include "EnvelopePublisher.h"
#include "DefaultEnvelopes.h"
#include "LayerAudibility.h"

#include <cmath>
#include <vector>
#include <thread>
#include <atomic>

// ── Phase 0 ───────────────────────────────────────────────────────────────────

// SanityTest — verifies the test runner itself is wired up correctly.
// Confirms CTest can discover, build, and execute this binary, and that
// juce_core is reachable. DSP and audio-engine tests are added each phase.
class SanityTest : public juce::UnitTest
{
public:
    // Registers this test under the "Sanity" category in the JUCE test runner.
    SanityTest() : juce::UnitTest("Sanity") {}

    void runTest() override
    {
        // Verify basic integer arithmetic — confirms the test framework reports pass/fail.
        beginTest("1 + 1 == 2");
        expectEquals(1 + 1, 2);

        // Verify juce::String construction and comparison — confirms juce_core links correctly.
        beginTest("juce::String round-trip");
        juce::String s("DOOF");
        expectEquals(s, juce::String("DOOF"));
    }
};

static SanityTest sanityTest;

// ── Phase 1 ───────────────────────────────────────────────────────────────────

// Phase1VoiceTest — automated verification of the SubVoice engine per §6 Phase 1 criteria:
//   (a) RMS rises then decays toward zero.
//   (b) Fundamental frequency falls over time (zero-crossing rate).
//   (c) No NaNs or Infs in any rendered sample.
//   (d) No large sample-to-sample jump at the choke boundary.
//
// NOTE: "zero allocations in processBlock" cannot be asserted in this runner;
// verify that separately with AddressSanitizer or a custom malloc hook.
class Phase1VoiceTest : public juce::UnitTest
{
public:
    Phase1VoiceTest() : juce::UnitTest("Phase1Voice") {}

    void runTest() override
    {
        testAmpEnvelope();
        testPitchFallsOverTime();
        testNoNaNsOrInfs();
        testChokeNoLargeJump();
    }

private:
    static constexpr double kSampleRate = 44100.0;

    // Builds the same default pitch+amp envelope snapshot PluginProcessor
    // seeds in production (DefaultEnvelopes), so these regression tests
    // exercise the real out-of-the-box envelope shape.
    static EnvelopeSnapshot buildDefaultSnapshot()
    {
        EnvelopeModel pitchModel, ampModel;
        DefaultEnvelopes::seedPitch(pitchModel);
        DefaultEnvelopes::seedAmp(ampModel);
        return EnvelopeSnapshot(EnvelopeEvaluator::buildTable(pitchModel),
                                 EnvelopeEvaluator::buildTable(ampModel));
    }

    // Helper: render numSamples from a freshly prepared voice into a vector.
    // noteOnAt is the sample index at which the first noteOn fires.
    std::vector<float> render(int numSamples, int noteOnAt = 0)
    {
        auto snap = buildDefaultSnapshot();
        SubVoice v;
        v.setSnapshot(&snap);
        v.prepare(kSampleRate);
        std::vector<float> buf(static_cast<std::size_t>(numSamples), 0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            if (i == noteOnAt)
                v.noteOn(60);
            buf[static_cast<std::size_t>(i)] = v.processSample();
        }
        return buf;
    }

    // Compute RMS over buf[start, end).
    double rms(const std::vector<float>& buf, int start, int end)
    {
        double sum = 0.0;
        for (int i = start; i < end; ++i)
        {
            double s = static_cast<double>(buf[static_cast<std::size_t>(i)]);
            sum += s * s;
        }
        return std::sqrt(sum / static_cast<double>(end - start));
    }

    // Count zero crossings (sign changes between consecutive samples) in buf[start, end).
    int zeroCrossings(const std::vector<float>& buf, int start, int end)
    {
        int count = 0;
        for (int i = start + 1; i < end; ++i)
        {
            bool prevNeg = buf[static_cast<std::size_t>(i - 1)] < 0.0f;
            bool currNeg = buf[static_cast<std::size_t>(i)]     < 0.0f;
            if (prevNeg != currNeg)
                ++count;
        }
        return count;
    }

    // (a) Amp envelope shape: RMS should be higher early than mid, higher mid than late.
    // Rendered to 3 seconds so the late window (2.5–3 s) captures the fully-decayed tail.
    // The linear 2 ms attack means the first window starts after the attack peak.
    void testAmpEnvelope()
    {
        beginTest("(a) Amp envelope: RMS decays over time");

        const int total = static_cast<int>(3.0 * kSampleRate); // 3 seconds
        auto buf = render(total, 0);

        // Windows (in samples): after-attack peak, mid-decay, near-silence tail.
        const int attackEnd  = static_cast<int>(0.003 * kSampleRate); // 3 ms
        const int midStart   = static_cast<int>(0.100 * kSampleRate); // 100 ms
        const int midEnd     = static_cast<int>(0.500 * kSampleRate); // 500 ms
        const int lateStart  = static_cast<int>(2.500 * kSampleRate); // 2500 ms
        // At 2.5–3 s: amp ≈ exp(-7.1) to exp(-8.6) ≈ 8e-4 to 2e-4 — well below –60 dB.

        double earlyRms = rms(buf, attackEnd, midStart);
        double midRms   = rms(buf, midStart,  midEnd);
        double lateRms  = rms(buf, lateStart, total);

        expect(earlyRms > midRms,  "Early RMS should exceed mid RMS (envelope decaying)");
        expect(midRms   > lateRms, "Mid RMS should exceed late RMS (envelope still decaying)");
        expect(lateRms  < 0.002,   "Late RMS should be near-zero at 2.5 to 3 s (tail fully decayed)");
    }

    // (b) Pitch envelope: zero-crossing rate should be higher in an early window than
    // in a same-duration late window, confirming the frequency sweep is working.
    // Both windows are 20 ms so the comparison is rate-accurate (not sample-count-biased).
    void testPitchFallsOverTime()
    {
        beginTest("(b) Pitch envelope: frequency falls over time");

        const int total = static_cast<int>(kSampleRate * 0.5); // 500 ms is enough
        auto buf = render(total, 0);

        // Early window: 2–22 ms (start after the 2 ms attack so amplitude is non-zero).
        // At 2–22 ms pitch is near kPitchStartHz (150 Hz) → ~6 zero crossings / 20 ms.
        const int earlyStart = static_cast<int>(0.002 * kSampleRate);
        const int earlyEnd   = static_cast<int>(0.022 * kSampleRate);

        // Late window: 200–220 ms, same 20 ms duration.
        // Pitch at 200 ms ≈ 58 Hz → ~2–3 zero crossings / 20 ms.
        const int lateStart  = static_cast<int>(0.200 * kSampleRate);
        const int lateEnd    = static_cast<int>(0.220 * kSampleRate);

        int earlyZC = zeroCrossings(buf, earlyStart, earlyEnd);
        int lateZC  = zeroCrossings(buf, lateStart,  lateEnd);

        expect(earlyZC > lateZC,
               "Zero-crossing rate should be higher early (higher pitch) than late");
    }

    // (c) No NaNs or Infs in any sample across the full tail.
    void testNoNaNsOrInfs()
    {
        beginTest("(c) No NaNs or Infs");

        const int total = static_cast<int>(kSampleRate * 2.0); // 2 seconds
        auto snap = buildDefaultSnapshot();
        SubVoice v;
        v.setSnapshot(&snap);
        v.prepare(kSampleRate);
        v.noteOn(60);

        for (int i = 0; i < total; ++i)
        {
            float s = v.processSample();
            // std::isfinite returns false for both NaN and Inf.
            expect(std::isfinite(s), "Sample " + juce::String(i) + " is not finite");
            if (!std::isfinite(s))
                return; // stop early to avoid flooding failures
        }
    }

    // (d) Choke anti-click: no sample-to-sample jump above kMaxJump anywhere in a
    // 40 ms window spanning the retrigger point at ~30 ms.
    // The linear fade ensures the amplitude at choke-start ramps smoothly to 0,
    // and the new body starts at sin(0)*0 = 0, so the transition is continuous.
    void testChokeNoLargeJump()
    {
        beginTest("(d) Choke: no large sample jump at retrigger");

        static constexpr float kMaxJump = 0.1f; // threshold: ±0.1 is a clearly audible click

        const int chokeAt  = static_cast<int>(0.030 * kSampleRate); // retrigger at 30 ms
        const int renderTo = static_cast<int>(0.040 * kSampleRate); // inspect 10 ms past choke

        auto snap = buildDefaultSnapshot();
        SubVoice v;
        v.setSnapshot(&snap);
        v.prepare(kSampleRate);
        v.noteOn(60);

        float prev = 0.0f;
        for (int i = 0; i < renderTo; ++i)
        {
            if (i == chokeAt)
                v.noteOn(60); // trigger choke while voice is playing

            float s    = v.processSample();
            float jump = std::abs(s - prev);

            if (jump > kMaxJump)
            {
                expect(false, "Sample jump " + juce::String(jump, 4)
                              + " at sample " + juce::String(i) + " exceeds threshold");
                return;
            }
            prev = s;
        }
        expect(true, "No large jump detected through choke transition");
    }
};

static Phase1VoiceTest phase1VoiceTest;

// ── Phase 2 ───────────────────────────────────────────────────────────────────

// EnvelopeModelTest — automated verification of EnvelopeModel per Phase 2 §6 criteria:
//   - add/move/delete nodes and assert the tree reflects it, in time order.
//   - serialize -> deserialize and assert identical state.
//   - undo/redo a sequence of edits and assert node state matches expectations
//     at each step.
class EnvelopeModelTest : public juce::UnitTest
{
public:
    EnvelopeModelTest() : juce::UnitTest("EnvelopeModel") {}

    void runTest() override
    {
        testAddNodeOrdering();
        testAddNodeDefaultControlPoints();
        testMoveNode();
        testMoveNodeResort();
        testDeleteNode();
        testSetControlPoints();
        testSerializeRoundTrip();
        testUndoRedoAddMoveDelete();
        testGestureCoalescesMultiMoveIntoOneUndo();
        testEditsOutsideGestureEachGetOwnStep();
        testGestureUndoInterleavedAcrossModelsIsChronological();
    }

private:
    // Compares two Node values (set from exact doubles in these tests, so
    // near-equality is expected; a tight tolerance absorbs any float slack).
    static void expectNodeEquals(juce::UnitTest& t, const EnvelopeModel::Node& a,
                                                     const EnvelopeModel::Node& b)
    {
        t.expectWithinAbsoluteError(a.time,       b.time,       1.0e-9);
        t.expectWithinAbsoluteError(a.value,      b.value,      1.0e-9);
        t.expectWithinAbsoluteError(a.cpOutTime,  b.cpOutTime,  1.0e-9);
        t.expectWithinAbsoluteError(a.cpOutValue, b.cpOutValue, 1.0e-9);
        t.expectWithinAbsoluteError(a.cpInTime,   b.cpInTime,   1.0e-9);
        t.expectWithinAbsoluteError(a.cpInValue,  b.cpInValue,  1.0e-9);
    }

    // (a) Nodes added out of time order end up sorted ascending by time.
    void testAddNodeOrdering()
    {
        beginTest("(a) addNode keeps nodes ordered by time");

        EnvelopeModel model;
        model.addNode(0.10, 1.0);
        model.addNode(0.00, 0.0);
        model.addNode(0.05, 0.5);

        expectEquals(model.getNumNodes(), 3);
        expectWithinAbsoluteError(model.getNode(0).time, 0.00, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(1).time, 0.05, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(2).time, 0.10, 1.0e-9);
    }

    // (b) A freshly added node's control points default to coincident with the
    // node itself (the "gently-eased default" documented in EnvelopeModel.h).
    void testAddNodeDefaultControlPoints()
    {
        beginTest("(b) addNode defaults control points to the node's own position");

        EnvelopeModel model;
        model.addNode(0.25, 0.75);
        auto n = model.getNode(0);

        expectWithinAbsoluteError(n.cpOutTime,  n.time,  1.0e-9);
        expectWithinAbsoluteError(n.cpOutValue, n.value, 1.0e-9);
        expectWithinAbsoluteError(n.cpInTime,   n.time,  1.0e-9);
        expectWithinAbsoluteError(n.cpInValue,  n.value, 1.0e-9);
    }

    // (c) Moving a node shifts its control points by the same delta, so the
    // curve shape travels with the node rather than being left behind.
    void testMoveNode()
    {
        beginTest("(c) moveNode shifts control points by the same delta");

        EnvelopeModel model;
        model.addNode(0.10, 0.50);
        model.setControlPoints(0, 0.05, 0.40, 0.15, 0.60); // reshape away from defaults

        const int newIndex = model.moveNode(0, 0.20, 0.80); // +0.10 time, +0.30 value
        expectEquals(newIndex, 0);

        auto n = model.getNode(0);
        expectWithinAbsoluteError(n.time,       0.20, 1.0e-9);
        expectWithinAbsoluteError(n.value,      0.80, 1.0e-9);
        expectWithinAbsoluteError(n.cpOutTime,  0.15, 1.0e-9); // 0.05 + 0.10
        expectWithinAbsoluteError(n.cpOutValue, 0.70, 1.0e-9); // 0.40 + 0.30
        expectWithinAbsoluteError(n.cpInTime,   0.25, 1.0e-9); // 0.15 + 0.10
        expectWithinAbsoluteError(n.cpInValue,  0.90, 1.0e-9); // 0.60 + 0.30
    }

    // (d) Moving a node past a neighbour re-sorts it into the correct index.
    void testMoveNodeResort()
    {
        beginTest("(d) moveNode re-sorts when crossing a neighbour's time");

        EnvelopeModel model;
        model.addNode(0.00, 0.0); // index 0
        model.addNode(0.10, 1.0); // index 1
        model.addNode(0.20, 2.0); // index 2

        const int newIndex = model.moveNode(0, 0.30, 3.0); // move first node past the last
        expectEquals(newIndex, 2);
        expectEquals(model.getNumNodes(), 3);

        expectWithinAbsoluteError(model.getNode(0).time, 0.10, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(1).time, 0.20, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(2).time, 0.30, 1.0e-9);
    }

    // (e) Deleting a node removes exactly that node and keeps the rest ordered.
    void testDeleteNode()
    {
        beginTest("(e) deleteNode removes the targeted node");

        EnvelopeModel model;
        model.addNode(0.00, 0.0);
        model.addNode(0.10, 1.0);
        model.addNode(0.20, 2.0);

        model.deleteNode(1); // remove the middle node

        expectEquals(model.getNumNodes(), 2);
        expectWithinAbsoluteError(model.getNode(0).time, 0.00, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(1).time, 0.20, 1.0e-9);
    }

    // (f) setControlPoints reshapes a curve without moving the node itself.
    void testSetControlPoints()
    {
        beginTest("(f) setControlPoints reshapes independent of node position");

        EnvelopeModel model;
        model.addNode(0.10, 0.50);
        model.setControlPoints(0, 0.02, 0.10, 0.18, 0.90);

        auto n = model.getNode(0);
        expectWithinAbsoluteError(n.time,  0.10, 1.0e-9); // unchanged
        expectWithinAbsoluteError(n.value, 0.50, 1.0e-9); // unchanged
        expectWithinAbsoluteError(n.cpOutTime,  0.02, 1.0e-9);
        expectWithinAbsoluteError(n.cpOutValue, 0.10, 1.0e-9);
        expectWithinAbsoluteError(n.cpInTime,   0.18, 1.0e-9);
        expectWithinAbsoluteError(n.cpInValue,  0.90, 1.0e-9);
    }

    // (g) Serializing to XML and back (the same path getStateInformation/
    // setStateInformation use in PluginProcessor) reproduces identical state.
    void testSerializeRoundTrip()
    {
        beginTest("(g) Serialize -> deserialize reproduces identical state");

        EnvelopeModel original;
        original.addNode(0.00, 0.0);
        original.addNode(0.10, 1.0);
        original.setControlPoints(1, 0.05, 0.20, 0.15, 0.90);
        original.addNode(0.20, 0.5);

        std::unique_ptr<juce::XmlElement> xml(original.getValueTree().createXml());
        auto restoredTree = juce::ValueTree::fromXml(*xml);

        EnvelopeModel restored;
        restored.setState(restoredTree);

        expectEquals(restored.getNumNodes(), original.getNumNodes());
        for (int i = 0; i < original.getNumNodes(); ++i)
            expectNodeEquals(*this, restored.getNode(i), original.getNode(i));
    }

    // (h) undo/redo walks back and forward through add/move/delete edits,
    // checking node state after every step.
    void testUndoRedoAddMoveDelete()
    {
        beginTest("(h) undo/redo restores state at each step");

        EnvelopeModel model;

        model.addNode(0.00, 0.0);              // T1
        expectEquals(model.getNumNodes(), 1);

        model.addNode(0.10, 1.0);              // T2
        expectEquals(model.getNumNodes(), 2);

        model.moveNode(0, 0.05, 0.5);           // T3
        expectWithinAbsoluteError(model.getNode(0).time, 0.05, 1.0e-9);

        model.deleteNode(1);                    // T4
        expectEquals(model.getNumNodes(), 1);

        model.undo(); // undo T4 -> node 1 comes back
        expectEquals(model.getNumNodes(), 2);

        model.undo(); // undo T3 -> the moved node goes back to time 0.00
        expectWithinAbsoluteError(model.getNode(0).time, 0.00, 1.0e-9);

        model.undo(); // undo T2 -> back to one node
        expectEquals(model.getNumNodes(), 1);

        model.undo(); // undo T1 -> back to empty
        expectEquals(model.getNumNodes(), 0);

        model.redo(); // redo T1
        model.redo(); // redo T2
        model.redo(); // redo T3
        model.redo(); // redo T4
        expectEquals(model.getNumNodes(), 1);
        expectWithinAbsoluteError(model.getNode(0).time, 0.05, 1.0e-9);
    }

    // (i) A multi-step drag — several moveNode calls bracketed by beginGesture/
    // endGesture — undoes in one step, back to the node's pre-drag position.
    void testGestureCoalescesMultiMoveIntoOneUndo()
    {
        beginTest("(i) beginGesture/endGesture coalesces a multi-move drag into one undo step");

        EnvelopeModel model;
        model.addNode(0.0, 0.0); // T1, own step

        model.beginGesture();
        model.moveNode(0, 0.1, 1.0);
        model.moveNode(0, 0.2, 2.0);
        model.moveNode(0, 0.3, 3.0);
        model.endGesture();

        expectWithinAbsoluteError(model.getNode(0).time,  0.3, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(0).value, 3.0, 1.0e-9);

        model.undo(); // should undo the whole gesture in one step
        expectWithinAbsoluteError(model.getNode(0).time,  0.0, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(0).value, 0.0, 1.0e-9);

        model.undo(); // undo T1 -> back to empty
        expectEquals(model.getNumNodes(), 0);
    }

    // (j) Edits made outside a gesture still each get their own undo step, even
    // when they sit immediately before/after a gesture in the same history.
    void testEditsOutsideGestureEachGetOwnStep()
    {
        beginTest("(j) Edits outside a gesture keep their own undo step");

        EnvelopeModel model;
        model.addNode(0.0, 0.0);  // T1
        model.addNode(0.1, 1.0);  // T2, own step

        model.beginGesture();
        model.moveNode(1, 0.2, 2.0);
        model.moveNode(1, 0.3, 3.0);
        model.endGesture();       // T3, one coalesced step

        model.deleteNode(0);      // T4, own step
        expectEquals(model.getNumNodes(), 1);

        model.undo(); // undo T4 -> deleted node comes back
        expectEquals(model.getNumNodes(), 2);

        model.undo(); // undo T3 -> the whole gesture reverts in one step
        expectWithinAbsoluteError(model.getNode(1).time,  0.1, 1.0e-9);
        expectWithinAbsoluteError(model.getNode(1).value, 1.0, 1.0e-9);

        model.undo(); // undo T2 -> back to one node
        expectEquals(model.getNumNodes(), 1);

        model.undo(); // undo T1 -> back to empty
        expectEquals(model.getNumNodes(), 0);
    }

    // (k) With two models sharing one UndoManager (PluginProcessor's pitch/amp
    // setup), undo walks back in true chronological order regardless of which
    // model each step touched, and a gesture on one model still counts as a
    // single step in that shared history.
    void testGestureUndoInterleavedAcrossModelsIsChronological()
    {
        beginTest("(k) Undo interleaved across two models is chronological, gesture still one step");

        juce::UndoManager sharedUndo;
        EnvelopeModel pitchModel(&sharedUndo);
        EnvelopeModel ampModel(&sharedUndo);

        pitchModel.addNode(0.0, 100.0); // T1 (pitch)
        ampModel.addNode(0.0, 0.5);     // T2 (amp)

        pitchModel.beginGesture();
        pitchModel.moveNode(0, 0.1, 110.0);
        pitchModel.moveNode(0, 0.2, 120.0);
        pitchModel.endGesture();        // T3 (pitch), one coalesced step

        ampModel.addNode(0.1, 0.8);     // T4 (amp)

        expectEquals(ampModel.getNumNodes(), 2);

        sharedUndo.undo(); // undo T4 -> amp back to 1 node
        expectEquals(ampModel.getNumNodes(), 1);

        sharedUndo.undo(); // undo T3 -> pitch gesture reverts fully in one step
        expectWithinAbsoluteError(pitchModel.getNode(0).time,  0.0,   1.0e-9);
        expectWithinAbsoluteError(pitchModel.getNode(0).value, 100.0, 1.0e-9);

        sharedUndo.undo(); // undo T2 -> amp back to empty
        expectEquals(ampModel.getNumNodes(), 0);

        sharedUndo.undo(); // undo T1 -> pitch back to empty
        expectEquals(pitchModel.getNumNodes(), 0);
    }
};

static EnvelopeModelTest envelopeModelTest;

// EnvelopeEvaluatorTest — automated verification of EnvelopeEvaluator::buildTable
// per Phase 2 §6 criteria:
//   - known node sets produce sampled values matching hand-computed expectations.
//   - the curve is continuous (no jump) across node boundaries.
//   - add/move/delete a node changes the rebuilt table.
class EnvelopeEvaluatorTest : public juce::UnitTest
{
public:
    EnvelopeEvaluatorTest() : juce::UnitTest("EnvelopeEvaluator") {}

    void runTest() override
    {
        testEmptyModelIsSilent();
        testSingleNodeHoldsFlat();
        testLinearSegmentMatchesHandComputedValues();
        testHoldBeforeFirstAndAfterLastNode();
        testContinuityAtNodeBoundaries();
        testTableChangesOnAddMoveDelete();
    }

private:
    static constexpr double kDt = EnvelopeEvaluator::kTableDomainSeconds
                                 / (double) EnvelopeEvaluator::kTableSize;

    // Index of the table sample at or immediately after the given time.
    static int indexAt(double time) { return (int) std::ceil(time / kDt); }

    static bool tablesEqual(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 1.0e-6f)
                return false;
        return true;
    }

    // (a) A model with no nodes produces an all-zero (silent) table.
    void testEmptyModelIsSilent()
    {
        beginTest("(a) Empty model produces an all-zero table");

        EnvelopeModel model;
        auto table = EnvelopeEvaluator::buildTable(model);

        expectEquals((int) table.size(), EnvelopeEvaluator::kTableSize);
        for (float v : table)
            expectWithinAbsoluteError(v, 0.0f, 1.0e-9f);
    }

    // (b) A single-node model holds that node's value across the whole table.
    void testSingleNodeHoldsFlat()
    {
        beginTest("(b) Single-node model holds flat at that value");

        EnvelopeModel model;
        model.addNode(1.0, 0.7);
        auto table = EnvelopeEvaluator::buildTable(model);

        expectWithinAbsoluteError(table[0],    0.7f, 1.0e-6f);
        expectWithinAbsoluteError(table[512],  0.7f, 1.0e-6f);
        expectWithinAbsoluteError(table[1024], 0.7f, 1.0e-6f);
        expectWithinAbsoluteError(table[4095], 0.7f, 1.0e-6f);
    }

    // (c) An exactly-linear segment (control points on the straight line)
    // produces sampled values matching value == time, hand-computed at a few
    // known indices, with the hold beginning exactly at the last node's index.
    void testLinearSegmentMatchesHandComputedValues()
    {
        beginTest("(c) Linear segment matches hand-computed values");

        EnvelopeModel model;
        model.addNode(0.0, 0.0);
        model.addNode(1.0, 1.0);
        model.setControlPoints(0, 1.0 / 3, 1.0 / 3, 0.0, 0.0);
        model.setControlPoints(1, 1.0, 1.0, 2.0 / 3, 2.0 / 3);

        auto table = EnvelopeEvaluator::buildTable(model);

        expectWithinAbsoluteError(table[0],    0.0f,        1.0e-6f);
        expectWithinAbsoluteError(table[512],  0.5f,        1.0e-6f); // t = 0.5
        expectWithinAbsoluteError(table[1023], 0.999023f,   1.0e-5f); // t = 1023/4096*4
        expectWithinAbsoluteError(table[1024], 1.0f,        1.0e-6f); // t = 1.0, hold begins
        expectWithinAbsoluteError(table[4095], 1.0f,        1.0e-6f); // still holding
    }

    // (d) Time before the first node holds at the first node's value; time
    // after the last node holds at the last node's value.
    void testHoldBeforeFirstAndAfterLastNode()
    {
        beginTest("(d) Holds flat before the first node and after the last node");

        EnvelopeModel model;
        model.addNode(1.0, 0.2);
        model.addNode(2.0, 0.8);

        auto table = EnvelopeEvaluator::buildTable(model);

        const int firstIndex = indexAt(1.0); // 1024
        const int lastIndex  = indexAt(2.0); // 2048

        expectWithinAbsoluteError(table[0],                          0.2f, 1.0e-6f);
        expectWithinAbsoluteError(table[(size_t) firstIndex - 1],    0.2f, 1.0e-6f);
        expectWithinAbsoluteError(table[(size_t) lastIndex],         0.8f, 1.0e-6f);
        expectWithinAbsoluteError(table[4095],                       0.8f, 1.0e-6f);
    }

    // (e) At each internal node boundary, the table value on either side of
    // the boundary index matches that node's own value closely — no jump
    // from an index-arithmetic error between adjacent segments.
    void testContinuityAtNodeBoundaries()
    {
        beginTest("(e) No jump at node boundaries between differently-shaped segments");

        EnvelopeModel model;
        model.addNode(0.0, 0.0);
        model.addNode(1.0, 0.5);
        model.addNode(2.0, 1.0);
        // Reshape each segment differently so a boundary bug can't hide behind
        // both segments happening to agree by construction.
        model.setControlPoints(0, 0.1, 0.9, 0.0, 0.0);
        model.setControlPoints(1, 1.2, 0.6, 0.9, 0.1);
        model.setControlPoints(2, 2.0, 1.0, 1.8, 0.4);

        auto table = EnvelopeEvaluator::buildTable(model);

        for (double nodeTime : { 1.0, 2.0 })
        {
            const int idx = indexAt(nodeTime);
            const float expected = (nodeTime == 1.0) ? 0.5f : 1.0f;

            expectWithinAbsoluteError(table[(size_t) idx],     expected, 0.01f);
            expectWithinAbsoluteError(table[(size_t) idx - 1], expected, 0.01f);
        }
    }

    // (f) Adding, moving, and deleting a node each change the rebuilt table.
    void testTableChangesOnAddMoveDelete()
    {
        beginTest("(f) add/move/delete each change the rebuilt table");

        EnvelopeModel model;
        model.addNode(0.0, 0.0);
        model.addNode(1.0, 1.0);
        auto baseline = EnvelopeEvaluator::buildTable(model);

        model.addNode(0.5, 0.9); // sharp spike in the middle of the segment
        auto afterAdd = EnvelopeEvaluator::buildTable(model);
        expect(!tablesEqual(baseline, afterAdd), "Adding a node should change the table");

        model.moveNode(1, 0.5, 0.2); // move the spike node's value down
        auto afterMove = EnvelopeEvaluator::buildTable(model);
        expect(!tablesEqual(afterAdd, afterMove), "Moving a node should change the table");

        model.deleteNode(1);
        auto afterDelete = EnvelopeEvaluator::buildTable(model);
        expect(!tablesEqual(afterMove, afterDelete), "Deleting a node should change the table");
        expect(tablesEqual(baseline, afterDelete),
               "Deleting the added node should restore the original table");
    }
};

static EnvelopeEvaluatorTest envelopeEvaluatorTest;

// EnvelopePublisherTest — automated verification of EnvelopePublisher per
// Phase 2 §6 criteria:
//   - a concurrency test: one thread hammers edits while another spins
//     reading the snapshot pointer; assert no crash and no torn/garbage
//     reads. (Two deterministic single-threaded tests of the basic publish
//     contract are included alongside, since the concurrency test alone
//     doesn't pin down *what* should be published, only that it's safe.)
//
// Note on what this proves: a passing concurrency test demonstrates no
// observed corruption across many iterations on this machine, this run — it
// is not a formal proof of the absence of a data race. Real assurance for
// that comes from running this binary under ThreadSanitizer, which is a
// manual verification step outside what a unit test itself can do.
class EnvelopePublisherTest : public juce::UnitTest
{
public:
    EnvelopePublisherTest() : juce::UnitTest("EnvelopePublisher") {}

    void runTest() override
    {
        testPublishesInitialSnapshot();
        testRepublishesOnEdit();
        testConcurrentEditsAndReads();
    }

private:
    // (a) A freshly constructed publisher immediately has a non-null,
    // all-zero (silent) snapshot — both models start out empty.
    void testPublishesInitialSnapshot()
    {
        beginTest("(a) Publishes a non-null, silent initial snapshot");

        EnvelopeModel pitchModel, ampModel;
        EnvelopePublisher publisher(pitchModel, ampModel);

        const auto* snap = publisher.getSnapshot();
        expect(snap != nullptr, "Initial snapshot should never be null");
        expectEquals((int) snap->pitchTable.size(), EnvelopeEvaluator::kTableSize);
        expectEquals((int) snap->ampTable.size(),   EnvelopeEvaluator::kTableSize);
        expectWithinAbsoluteError(snap->pitchTable[0], 0.0f, 1.0e-9f);
        expectWithinAbsoluteError(snap->ampTable[0],   0.0f, 1.0e-9f);
    }

    // (b) Editing either model synchronously republishes: the pointer changes
    // and the new snapshot's tables reflect the edit.
    void testRepublishesOnEdit()
    {
        beginTest("(b) Editing a model republishes a new snapshot reflecting the edit");

        EnvelopeModel pitchModel, ampModel;
        EnvelopePublisher publisher(pitchModel, ampModel);
        const auto* initial = publisher.getSnapshot();

        pitchModel.addNode(0.0, 100.0);
        const auto* afterPitchEdit = publisher.getSnapshot();
        expect(afterPitchEdit != initial, "Snapshot pointer should change after a pitch edit");
        expectWithinAbsoluteError(afterPitchEdit->pitchTable[0], 100.0f, 1.0e-3f);

        ampModel.addNode(0.0, 0.5);
        const auto* afterAmpEdit = publisher.getSnapshot();
        expect(afterAmpEdit != afterPitchEdit, "Snapshot pointer should change after an amp edit");
        expectWithinAbsoluteError(afterAmpEdit->ampTable[0], 0.5f, 1.0e-3f);
        // The pitch table carries over unaffected by the amp-only edit.
        expectWithinAbsoluteError(afterAmpEdit->pitchTable[0], 100.0f, 1.0e-3f);
    }

    // (c) One thread hammers add/move/delete edits on both models while
    // another spins on getSnapshot(), reading table values every iteration.
    // Passes if every observed value stays finite across many iterations.
    void testConcurrentEditsAndReads()
    {
        beginTest("(c) Concurrent edits and reads: no crash, no non-finite values");

        static constexpr int kNumEdits = 3000;

        EnvelopeModel pitchModel, ampModel;
        EnvelopePublisher publisher(pitchModel, ampModel);

        std::atomic<bool> writerDone { false };
        std::atomic<bool> sawBadValue { false };
        std::atomic<int>  readIterations { 0 };

        std::thread reader([&]
        {
            while (!writerDone.load(std::memory_order_acquire))
            {
                const auto* snap = publisher.getSnapshot();
                if (snap == nullptr)
                {
                    sawBadValue.store(true, std::memory_order_relaxed);
                    break;
                }

                for (int idx : { 0, 500, 1024, 2048, 4095 })
                {
                    const float pv = snap->pitchTable[(size_t) idx];
                    const float av = snap->ampTable[(size_t) idx];
                    if (!std::isfinite(pv) || !std::isfinite(av))
                    {
                        sawBadValue.store(true, std::memory_order_relaxed);
                        break;
                    }
                }

                readIterations.fetch_add(1, std::memory_order_relaxed);
            }
        });

        // Writer: cycles add/move/delete, keeping pitch and amp models in
        // lockstep (same sequence of calls) so node indices stay valid for both.
        int numNodes = 0;
        for (int i = 0; i < kNumEdits; ++i)
        {
            const int op = i % 3;
            if (op == 0 || numNodes == 0)
            {
                pitchModel.addNode((double) i * 0.0001, (double) (i % 100));
                ampModel.addNode((double) i * 0.0001, (double) (i % 50) * 0.01);
                ++numNodes;
            }
            else if (op == 1)
            {
                const int idx = i % numNodes;
                pitchModel.moveNode(idx, (double) i * 0.0001 + 0.5, (double) (i % 77));
                ampModel.moveNode(idx, (double) i * 0.0001 + 0.5, (double) (i % 33) * 0.01);
            }
            else
            {
                const int idx = i % numNodes;
                pitchModel.deleteNode(idx);
                ampModel.deleteNode(idx);
                --numNodes;
            }
        }

        writerDone.store(true, std::memory_order_release);
        reader.join();

        expect(!sawBadValue.load(), "Reader observed a non-finite value in a published snapshot");
        expect(readIterations.load() > 0, "Reader thread should have completed at least one read");
    }
};

static EnvelopePublisherTest envelopePublisherTest;

// LayerAudibilityTest — Phase 4 §6 Verify: "Unit-test the audibility function
// against all 16 combinations of the §3.3 truth table."
//
// §3.3's table itself is 4 flag-combinations x 2 solo-states = 8 cells. The
// 16 come from the fourth input the table doesn't tabulate but the rule still
// depends on: whether the layer is Off (§3.2's per-layer type). So the input
// space is (isOff, mute, solo, anySoloed) = 2^4, and every one is asserted
// below.
//
// The expected column is a literal transcription of the doc, not computed
// from anything — if it were derived by formula it would only restate the
// implementation and could pass while both were wrong in the same way. Two
// rows (solo set while anySoloed is false) are unreachable through the real
// UI, since setting a layer's solo makes anyLayerSoloed() true by definition;
// they're asserted anyway so the function is total over its inputs.
class LayerAudibilityTest : public juce::UnitTest
{
public:
    LayerAudibilityTest() : juce::UnitTest("LayerAudibility") {}

    void runTest() override
    {
        testAllSixteenCombinations();
        testAnyLayerSoloed();
        testFlagsOverloadAgrees();
    }

private:
    // (a) Every combination of the four inputs, against §3.3 as written.
    void testAllSixteenCombinations()
    {
        // No "§" in any string literal reaching juce::String: JUCE asserts on
        // non-ASCII char* input (juce_String.cpp's CharPointer_ASCII check),
        // since it can't know the source encoding. Comments are unaffected.
        beginTest("(a) All 16 combinations match the section 3.3 truth table");

        struct Row { bool isOff, mute, solo, anySoloed, expected; };

        // Rows 1-8: layer in the mix (isOff == false) — §3.3's table verbatim,
        // read left column first: neither / solo / mute / solo+mute, each with
        // no layer soloed then >=1 layer soloed.
        // Rows 9-16: same eight with the layer Off — silent in every case,
        // since an Off layer contributes nothing to the mix at all.
        static constexpr Row rows[] =
        {
            { false, false, false, false, true  }, // neither,     no solo  -> audible
            { false, false, false, true,  false }, // neither,     soloing  -> silent
            { false, false, true,  false, true  }, // solo,        no solo  -> audible
            { false, false, true,  true,  true  }, // solo,        soloing  -> audible
            { false, true,  false, false, false }, // mute,        no solo  -> silent
            { false, true,  false, true,  false }, // mute,        soloing  -> silent
            { false, true,  true,  false, false }, // solo+mute,   no solo  -> silent (mute wins)
            { false, true,  true,  true,  false }, // solo+mute,   soloing  -> silent (mute wins)

            { true,  false, false, false, false },
            { true,  false, false, true,  false },
            { true,  false, true,  false, false },
            { true,  false, true,  true,  false },
            { true,  true,  false, false, false },
            { true,  true,  false, true,  false },
            { true,  true,  true,  false, false },
            { true,  true,  true,  true,  false },
        };

        // A compile-time property of the table, so it fails the build rather
        // than the test run if a row is ever dropped.
        static_assert(sizeof(rows) / sizeof(rows[0]) == 16,
                      "The truth table must cover all 2^4 combinations of the four inputs");

        for (const auto& row : rows)
        {
            const bool actual = LayerAudibility::isAudible(row.isOff, row.mute, row.solo, row.anySoloed);
            expect(actual == row.expected,
                   "isAudible(isOff=" + juce::String((int) row.isOff)
                     + ", mute="      + juce::String((int) row.mute)
                     + ", solo="      + juce::String((int) row.solo)
                     + ", anySoloed=" + juce::String((int) row.anySoloed)
                     + ") should be " + (row.expected ? "audible" : "silent"));
        }
    }

    // (b) anyLayerSoloed over the whole 5-layer array, including §3.3's
    // "multi-solo: any number soloed at once" and the Off-layer exclusion.
    void testAnyLayerSoloed()
    {
        beginTest("(b) anyLayerSoloed: none / one / several / soloed-but-Off");

        std::array<LayerAudibility::LayerFlags, LayerAudibility::kNumLayers> layers {};
        expect(! LayerAudibility::anyLayerSoloed(layers), "A fresh array has nothing soloed");

        layers[2].solo = true;
        expect(LayerAudibility::anyLayerSoloed(layers), "One soloed layer counts");

        layers[0].solo = true;
        layers[4].solo = true;
        expect(LayerAudibility::anyLayerSoloed(layers), "Multi-solo counts (any number soloed at once)");

        // Off layers produce no sound, so their solo flag must not silence the
        // layers that do — see anyLayerSoloed()'s own comment for why this is
        // our reading of a case §3.3 leaves open.
        for (auto& layer : layers)
            layer.isOff = true;
        expect(! LayerAudibility::anyLayerSoloed(layers),
               "Layers that are soloed but Off must not count as soloed");

        // A single in-mix soloed layer is enough, even with Off soloed layers present.
        layers[3].isOff = false;
        layers[3].solo  = true;
        expect(LayerAudibility::anyLayerSoloed(layers),
               "One in-mix soloed layer counts even alongside soloed-but-Off layers");

        // Mute is irrelevant here: a muted-but-soloed layer still silences
        // everyone else (it just isn't audible itself — row 7/8 of table (a)).
        std::array<LayerAudibility::LayerFlags, LayerAudibility::kNumLayers> muted {};
        muted[1].solo = true;
        muted[1].mute = true;
        expect(LayerAudibility::anyLayerSoloed(muted),
               "A soloed layer still counts as soloed even when also muted");
    }

    // (c) The LayerFlags overload is a pass-through, not a second copy of the
    // rule that could drift from the four-bool version.
    void testFlagsOverloadAgrees()
    {
        beginTest("(c) LayerFlags overload agrees with the four-bool version everywhere");

        for (int i = 0; i < 16; ++i)
        {
            LayerAudibility::LayerFlags flags;
            flags.isOff = (i & 8) != 0;
            flags.mute  = (i & 4) != 0;
            flags.solo  = (i & 2) != 0;
            const bool anySoloed = (i & 1) != 0;

            expect(LayerAudibility::isAudible(flags, anySoloed)
                     == LayerAudibility::isAudible(flags.isOff, flags.mute, flags.solo, anySoloed),
                   "Overload disagreed at combination index " + juce::String(i));
        }
    }
};

static LayerAudibilityTest layerAudibilityTest;

// ── Test runner entry point ────────────────────────────────────────────────────

// Runs all registered juce::UnitTest subclasses and returns a non-zero exit code
// on any failure so CTest reports the test as failed.
//
// ScopedJuceInitialiser_GUI creates (and, on scope exit, tears down) the
// juce::MessageManager singleton. EnvelopePublisher's retirement mechanism
// uses juce::Timer, which asserts a MessageManager exists — this is JUCE's
// own documented pattern for console apps that touch such classes.
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
