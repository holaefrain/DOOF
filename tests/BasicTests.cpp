#include <juce_core/juce_core.h>
#include "SubVoice.h"
#include "ClickVoice.h"
#include "EnvelopeModel.h"
#include "EnvelopeEvaluator.h"
#include "EnvelopePublisher.h"
#include "DefaultEnvelopes.h"
#include "LayerAudibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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
        testAppearanceOverAllCombinations();
        testAppearanceIsSelfConsistent();
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

    // (d) The GUI appearance rule, over all 16 combinations, written out rather than derived.
    // A table computed from appearanceFor's own logic would only restate the implementation.
    void testAppearanceOverAllCombinations()
    {
        beginTest("(d) All 16 combinations produce the section 3.3 lit/dimmed appearance");

        struct Row { bool isOff, mute, solo, anySoloed, lit, dimmed; };

        // dimmed is reserved for a layer with no local reason to be silent: muted and Off layers
        // already show why through their own controls, so they never dim.
        static constexpr Row rows[] =
        {
            { false, false, false, false, false, false }, // audible, nothing soloed
            { false, false, false, true,  false, true  }, // the only dimmed case there is
            { false, false, true,  false, true,  false }, // soloed, nothing else soloed
            { false, false, true,  true,  true,  false }, // soloed among others
            { false, true,  false, false, false, false }, // muted: silent, but not dimmed
            { false, true,  false, true,  false, false },
            { false, true,  true,  false, true,  false }, // solo+mute: silent, still reads as soloed
            { false, true,  true,  true,  true,  false },

            { true,  false, false, false, false, false }, // Off never lights and never dims
            { true,  false, false, true,  false, false },
            { true,  false, true,  false, false, false }, // soloed but Off: not lit, see appearanceFor
            { true,  false, true,  true,  false, false },
            { true,  true,  false, false, false, false },
            { true,  true,  false, true,  false, false },
            { true,  true,  true,  false, false, false },
            { true,  true,  true,  true,  false, false },
        };

        static_assert(sizeof(rows) / sizeof(rows[0]) == 16,
                      "The appearance table must cover all 2^4 combinations of the four inputs");

        for (const auto& row : rows)
        {
            LayerAudibility::LayerFlags flags;
            flags.isOff = row.isOff;
            flags.mute  = row.mute;
            flags.solo  = row.solo;

            const auto actual = LayerAudibility::appearanceFor(flags, row.anySoloed);
            const auto where = " at (isOff=" + juce::String((int) row.isOff)
                                 + ", mute="  + juce::String((int) row.mute)
                                 + ", solo="  + juce::String((int) row.solo)
                                 + ", anySoloed=" + juce::String((int) row.anySoloed) + ")";

            expect(actual.lit == row.lit,
                   "lit was " + juce::String((int) actual.lit) + ", expected "
                     + juce::String((int) row.lit) + where);
            expect(actual.dimmed == row.dimmed,
                   "dimmed was " + juce::String((int) actual.dimmed) + ", expected "
                     + juce::String((int) row.dimmed) + where);
        }
    }

    // (e) A layer is never both lit and dimmed, and a dimmed layer is always genuinely silent.
    // Cheap to state, and it pins the two fields to each other rather than only to the table.
    void testAppearanceIsSelfConsistent()
    {
        beginTest("(e) Appearance never claims a layer is both soloed and silenced by others");

        for (int i = 0; i < 16; ++i)
        {
            LayerAudibility::LayerFlags flags;
            flags.isOff = (i & 8) != 0;
            flags.mute  = (i & 4) != 0;
            flags.solo  = (i & 2) != 0;
            const bool anySoloed = (i & 1) != 0;

            const auto appearance = LayerAudibility::appearanceFor(flags, anySoloed);

            expect(! (appearance.lit && appearance.dimmed),
                   "Both lit and dimmed at combination index " + juce::String(i));

            if (appearance.dimmed)
                expect(! LayerAudibility::isAudible(flags, anySoloed),
                       "A dimmed layer is still audible at combination index " + juce::String(i));
        }
    }
};

static LayerAudibilityTest layerAudibilityTest;

// ── Phase 5 ───────────────────────────────────────────────────────────────────

// Phase5ClickVoiceTest — the transient half of a kick, per §6 Phase 5.
//   (a) Onset lands on the trigger sample itself, for all four types.
//   (b) The decay control is a real duration: silence by then, sound before it, at any sample rate.
//   (c) Two identical triggers render bit-identically.
//   (d) A retrigger adds no discontinuity beyond a fresh onset's own.
//   (e) Raising tone measurably raises high-frequency content.
//
// Lives in the lean DOOFTests target rather than DOOFIntegrationTests, which is the whole reason
// ClickVoice.h is restricted to juce_core — these are the tests worth iterating on quickly while
// tuning the click's sound in 6a.
class Phase5ClickVoiceTest : public juce::UnitTest
{
public:
    Phase5ClickVoiceTest() : juce::UnitTest("Phase5ClickVoice") {}

    void runTest() override
    {
        testOnsetIsImmediate();
        testDecayIsARealDuration();
        testIdenticalTriggersAreIdentical();
        testRetriggerAddsNoDiscontinuity();
        testToneRaisesHighFrequencyContent();
    }

private:
    static constexpr double kSampleRate = 44100.0;

    // Every type, so no check below can silently cover only the one that happens to be first.
    static constexpr ClickVoice::Type kAllTypes[] = {
        ClickVoice::Type::tick, ClickVoice::Type::noise,
        ClickVoice::Type::snap, ClickVoice::Type::thump
    };

    // Readable names for failure messages; indexed by the enum's own numbering, which Step 3a
    // freezes into the parameter's choice list.
    static juce::String nameOf(ClickVoice::Type type)
    {
        switch (type)
        {
            case ClickVoice::Type::tick:  return "tick";
            case ClickVoice::Type::noise: return "noise";
            case ClickVoice::Type::snap:  return "snap";
            case ClickVoice::Type::thump: return "thump";
        }
        return "?";
    }

    // Renders one hit from a freshly prepared voice. Triggered at sample 0, since the voice's own
    // onset timing is what is under test here — placing it inside a block is the processor's job
    // and is covered by Phase5MidiDispatchTest in the integration target.
    static std::vector<float> render(ClickVoice::Type type, float tone, double decaySeconds,
                                     int numSamples, double sr = kSampleRate)
    {
        ClickVoice voice;
        voice.prepare(sr);
        voice.setType(type);
        voice.setTone(tone);
        voice.setDecaySeconds(decaySeconds);
        voice.noteOn(60);

        std::vector<float> out((std::size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
            out[(std::size_t) i] = voice.processSample();

        return out;
    }

    // Largest jump between consecutive samples over buf[start, end), measured against the sample
    // before start so the very first transition is included rather than skipped.
    static float maxJump(const std::vector<float>& buf, int start, int end)
    {
        float largest = 0.0f;
        for (int i = start; i < end; ++i)
        {
            const float prev = (i > 0) ? buf[(std::size_t) i - 1] : 0.0f;
            largest = std::max(largest, std::abs(buf[(std::size_t) i] - prev));
        }
        return largest;
    }

    // (a) A click's energy must arrive on the trigger sample, not one or more samples later.
    // Sample-accurate dispatch (Step 1) puts the note-on at the right sample; this is the other
    // half of that promise — that the voice actually sounds on the sample it was given.
    void testOnsetIsImmediate()
    {
        beginTest("(a) Every click type sounds on the sample it is triggered");

        for (const auto type : kAllTypes)
        {
            const auto hit = render(type, 0.5f, 0.008, 64);

            expect(hit[0] != 0.0f,
                   nameOf(type) + " is silent on its own trigger sample, so its onset is late");
        }
    }

    // (b) decay is documented as a duration, not a time constant, so both halves are asserted:
    // there is still sound shortly before it and exact silence from it onward. Checked at two
    // sample rates, since the fall is derived from the rate and a missing division would only
    // show up away from 44.1 kHz.
    void testDecayIsARealDuration()
    {
        beginTest("(b) The decay control is a real duration at any sample rate");

        for (const double sr : { 44100.0, 96000.0 })
        {
            for (const double decay : { 0.002, 0.020 })
            {
                // The first sample whose timestamp reaches decaySeconds — ceil, not truncate,
                // since a sample at index floor(decay * sr) can still be a hair before the end.
                const int decaySample = (int) std::ceil(decay * sr);
                const auto hit = render(ClickVoice::Type::noise, 0.5f, decay,
                                        decaySample + (int) (sr * 0.01), sr);

                // Just before the end: the hit must not have stopped early.
                bool soundedLate = false;
                for (int i = decaySample - (int) (sr * 0.0005); i < decaySample; ++i)
                    if (hit[(std::size_t) i] != 0.0f)
                        soundedLate = true;

                expect(soundedLate,
                       "A " + juce::String(decay * 1000.0, 1) + " ms click at "
                           + juce::String(sr, 0) + " Hz is already silent before its decay ends");

                // From the end onward: exact silence, not merely quiet.
                int rangAfter = -1;
                for (int i = decaySample; i < (int) hit.size(); ++i)
                    if (hit[(std::size_t) i] != 0.0f)
                    {
                        rangAfter = i;
                        break;
                    }

                expect(rangAfter < 0,
                       "A " + juce::String(decay * 1000.0, 1) + " ms click at "
                           + juce::String(sr, 0) + " Hz was still sounding at sample "
                           + juce::String(rangAfter) + ", past its decay");
            }
        }
    }

    // (c) The noise generator is re-seeded to a fixed value on every hit precisely so this holds.
    // A real click sample is identical every trigger, and without this property none of the
    // engine's null tests could survive a click layer being switched on.
    void testIdenticalTriggersAreIdentical()
    {
        beginTest("(c) Two identical triggers render bit-identically");

        for (const auto type : kAllTypes)
        {
            const auto first  = render(type, 0.5f, 0.008, 1024);
            const auto second = render(type, 0.5f, 0.008, 1024);

            int diverged = -1;
            for (std::size_t i = 0; i < first.size(); ++i)
                if (std::memcmp(&first[i], &second[i], sizeof(float)) != 0)
                {
                    diverged = (int) i;
                    break;
                }

            expect(diverged < 0,
                   nameOf(type) + " renders differently on a second identical trigger, first at "
                       + "sample " + juce::String(diverged));
        }
    }

    // (d) Deliberately NOT the sub's "no jump above 0.1" (Phase1VoiceTest (d)). That threshold
    // works for SubVoice because its new body starts at sin(0) * 0 = 0, so a retrigger really is
    // continuous. A click is the opposite: its onset is a designed discontinuity — thump's is
    // exactly 1.0 — so a fixed threshold would either fail on every type or have to be set so
    // loose it proved nothing.
    //
    // Nor is it enough to compare the largest jump around a retrigger against a fresh onset's own.
    // That version of this test was written first and bite-testing killed it: replacing the fade
    // with an instant cut of the outgoing hit did not fail it, because the new hit's onset is an
    // order of magnitude larger than the tail being cut and simply hides it.
    //
    // So the outgoing hit is isolated instead. The voice sums its slots linearly and (c) proved
    // the new hit renders identically every time, so subtracting a fresh hit rendered alone leaves
    // exactly the outgoing hit's faded contribution — the thing ClickVoice::noteOn's parallel
    // ramp actually governs. Two claims about it, and a cut or a missing ramp each break one:
    //   - it is still sounding just after the retrigger, rather than having vanished, and
    //   - it does reach exact silence shortly after, rather than ringing on underneath.
    void testRetriggerAddsNoDiscontinuity()
    {
        beginTest("(d) The outgoing hit ramps out across a retrigger instead of being cut");

        const int retriggerAt = 100;
        const int window = 1024;

        // Comfortably longer than the voice's internal ramp, which is not visible from here.
        const int fadeMustFinishBy = (int) (kSampleRate * 0.010);

        // Far longer than a click would really be set to, and deliberately so: with a realistic
        // 8 ms decay the outgoing hit reaches the end of its own envelope before the ramp window
        // closes, and the "does it actually stop" check below can never fail. Bite-testing found
        // that — removing the ramp entirely still passed. A long decay leaves the hit with plenty
        // of life left, so the only thing that can silence it is the ramp.
        const double decay = 0.050;

        // A short window either side of the retrigger. 16 samples is long enough for a stable
        // level reading on the noise types and short enough that the hit's own decay barely
        // moves across it.
        const int levelWindow = 16;

        for (const auto type : kAllTypes)
        {
            // One uninterrupted render serves as both references, because the outgoing hit and
            // the new one are the same hit: solo[k] is the new hit k samples after it starts, and
            // solo[retriggerAt + k] is what the outgoing hit would have been at that same instant
            // had nothing interrupted it.
            const auto solo = render(type, 0.5f, decay, retriggerAt + window);

            ClickVoice voice;
            voice.prepare(kSampleRate);
            voice.setType(type);
            voice.setTone(0.5f);
            voice.setDecaySeconds(decay);
            voice.noteOn(60);

            std::vector<float> out((std::size_t) (retriggerAt + window), 0.0f);
            for (int i = 0; i < (int) out.size(); ++i)
            {
                if (i == retriggerAt)
                    voice.noteOn(60);

                out[(std::size_t) i] = voice.processSample();
            }

            // Everything from the retrigger on, minus the new hit, is the outgoing hit alone.
            std::vector<float> residual((std::size_t) window, 0.0f);
            for (int k = 0; k < window; ++k)
                residual[(std::size_t) k] = out[(std::size_t) (retriggerAt + k)]
                                              - solo[(std::size_t) k];

            // Compared against the uninterrupted hit at the same instant, not against its level
            // before the retrigger. An earlier draft did the latter and failed on tick for a
            // reason that had nothing to do with ramping: tick's ringing is 68 dB down by the
            // retrigger point, so its own decay across a 16-sample window swamped the ramp.
            // Measuring against the same instant removes the decay from the comparison entirely.
            float faded = 0.0f, unfaded = 0.0f;
            for (int k = 0; k < levelWindow; ++k)
            {
                faded   = std::max(faded,   std::abs(residual[(std::size_t) k]));
                unfaded = std::max(unfaded, std::abs(solo[(std::size_t) (retriggerAt + k)]));
            }

            expect(unfaded > 0.0f,
                   nameOf(type) + " is already silent when the retrigger arrives, so this check "
                   + "has nothing to measure");

            // The ramp has removed under 10% this early, so anything near half means it is ramping.
            // What this rules out is the level going to nothing, which is what a cut does.
            expect(faded > unfaded * 0.5f,
                   nameOf(type) + " loses the outgoing hit at retrigger (" + juce::String(faded, 6)
                       + " against an uninterrupted " + juce::String(unfaded, 6)
                       + ") - it is being cut, not ramped");

            int stillRingingAt = -1;
            for (int k = fadeMustFinishBy; k < window; ++k)
                if (residual[(std::size_t) k] != 0.0f)
                {
                    stillRingingAt = k;
                    break;
                }

            expect(stillRingingAt < 0,
                   nameOf(type) + " still has the outgoing hit sounding "
                       + juce::String(stillRingingAt) + " samples after the retrigger, so it is "
                       + "never being ramped out at all");
        }
    }

    // (e) §6's spectral split, measured without an FFT so it can live in the lean target. The FFT
    // proper is 6a's job in the integration target.
    //
    // Measured as mean |x[n] - x[n-1]| divided by mean |x[n]| — how fast the signal moves relative
    // to how big it is, which is a serviceable stand-in for a spectral centroid. The ratio matters:
    // the raw first difference alone is NOT monotonic in tone, because a brighter setting also
    // decays faster, and the two effects cancel almost exactly for tick. Measured, not assumed —
    // an earlier draft of this test used the raw difference and could not have passed.
    void testToneRaisesHighFrequencyContent()
    {
        beginTest("(e) Raising tone raises high-frequency content");

        for (const auto type : kAllTypes)
        {
            double previous = -1.0;

            for (const float tone : { 0.1f, 0.5f, 0.9f })
            {
                const auto hit = render(type, tone, 0.008, (int) (kSampleRate * 0.02));

                double sumAbs = 0.0, sumDelta = 0.0;
                for (std::size_t i = 0; i < hit.size(); ++i)
                {
                    sumAbs += std::abs((double) hit[i]);
                    if (i > 0)
                        sumDelta += std::abs((double) hit[i] - (double) hit[i - 1]);
                }

                expect(sumAbs > 0.0, nameOf(type) + " rendered silence at tone "
                                         + juce::String(tone, 1) + ", so this check is vacuous");
                if (sumAbs <= 0.0)
                    return;

                const double brightness = sumDelta / sumAbs;

                expect(brightness > previous,
                       nameOf(type) + " is no brighter at tone " + juce::String(tone, 1)
                           + " (" + juce::String(brightness, 4) + ") than at the setting below it ("
                           + juce::String(previous, 4) + ")");

                previous = brightness;
            }
        }
    }
};

static Phase5ClickVoiceTest phase5ClickVoiceTest;

// Phase5ClickSamplePlaybackTest — ClickVoice playing a decoded sample slot rather than a
// synthesised type.
//   (a) hasContentFor answers per slot: synthesised types always, sample slots only when loaded.
//   (b) A sample sounds on its trigger sample and plays to its own end, then exact silence.
//   (c) The decay control does not truncate a sample.
//   (d) Rate conversion keeps a sample's pitch and length in both directions.
//   (e) The rate conversion interpolates linearly rather than dropping or repeating samples.
//   (f) Tone is a plain low-pass on a sample: it darkens, and it keeps the low end.
//   (g) An outgoing sample ramps out across a retrigger instead of being cut.
//
// Built from hand-made ClickSample structs rather than decoded WAVs. That is the whole point of
// ClickSample being a bare pointer + length: the playback path stays testable in this lean target,
// and decoding is ClickSampleLibraryTest's job in the integration target.
class Phase5ClickSamplePlaybackTest : public juce::UnitTest
{
public:
    Phase5ClickSamplePlaybackTest() : juce::UnitTest("Phase5ClickSamplePlayback") {}

    void runTest() override
    {
        testHasContentForEachSlot();
        testPlaysFromTriggerToItsOwnEnd();
        testDecayDoesNotTruncateASample();
        testRateConversionKeepsPitchAndLength();
        testRateConversionInterpolatesLinearly();
        testToneIsAPlainLowPass();
        testRetriggerRampsOutAnOutgoingSample();
    }

private:
    static constexpr double kSampleRate = 44100.0;

    // The choice index of Sample 1, written out from ClickVoice's own constants so this file needs
    // nothing from ParamIDs.h (which the lean target does not need to pull in).
    static constexpr int kFirstSampleSlot = ClickVoice::kNumTypes;

    // Four slots with the given content in the first and the rest empty — the layout
    // ClickSampleLibrary hands every voice. Points into `data`, which must outlive the result.
    static std::array<ClickSample, ClickVoice::kNumSampleSlots>
        slotsWith(const std::vector<float>& data, double sourceRate)
    {
        std::array<ClickSample, ClickVoice::kNumSampleSlots> slots {};
        slots[0].data = data.data();
        slots[0].numSamples = (int) data.size();
        slots[0].sourceSampleRate = sourceRate;
        return slots;
    }

    // Renders one hit of Sample 1 from a freshly prepared voice, triggered at sample 0.
    static std::vector<float> render(const std::vector<float>& data, double sourceRate,
                                     double deviceRate, int numSamples,
                                     float tone = 1.0f, double decaySeconds = 0.008)
    {
        const auto slots = slotsWith(data, sourceRate);

        ClickVoice voice;
        voice.prepare(deviceRate);
        voice.setSampleSlots(slots.data());
        voice.setSlot(kFirstSampleSlot);
        voice.setTone(tone);
        voice.setDecaySeconds(decaySeconds);
        voice.noteOn(60);

        std::vector<float> out((std::size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
            out[(std::size_t) i] = voice.processSample();

        return out;
    }

    // Index of the last non-zero sample, or -1 for a render that is silent throughout.
    static int lastSounding(const std::vector<float>& buf)
    {
        for (int i = (int) buf.size() - 1; i >= 0; --i)
            if (buf[(std::size_t) i] != 0.0f)
                return i;

        return -1;
    }

    static std::vector<float> sine(double freqHz, double rate, int numSamples, double phase = 0.0)
    {
        std::vector<float> out((std::size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            out[(std::size_t) i] = (float) std::sin(phase + juce::MathConstants<double>::twoPi
                                                                * freqHz * i / rate);
        return out;
    }

    // (a) The processor decides between mixing the click voice and mixing silence by asking this,
    // so a wrong answer is either an empty slot falling back to some other sound or a loaded one
    // never being heard.
    void testHasContentForEachSlot()
    {
        beginTest("(a) hasContentFor: synthesised types always, sample slots only when loaded");

        ClickVoice noSamples;
        noSamples.prepare(kSampleRate);

        for (int slot = 0; slot < ClickVoice::kNumSlots; ++slot)
            expect(noSamples.hasContentFor(slot) == (slot < ClickVoice::kNumTypes),
                   "With no samples at all, slot " + juce::String(slot) + " reports the wrong "
                       "content");

        const std::vector<float> data(64, 0.5f);
        auto slots = slotsWith(data, kSampleRate);

        // One sample long is not loadable: interpolation needs a next sample to read toward, and
        // isLoaded() says so. A slot holding one must read as empty, not as playable.
        slots[2].data = data.data();
        slots[2].numSamples = 1;
        slots[2].sourceSampleRate = kSampleRate;

        ClickVoice voice;
        voice.prepare(kSampleRate);
        voice.setSampleSlots(slots.data());

        const bool expected[ClickVoice::kNumSlots] { true, true, true, true, true, false, false, false };
        for (int slot = 0; slot < ClickVoice::kNumSlots; ++slot)
            expect(voice.hasContentFor(slot) == expected[slot],
                   "Slot " + juce::String(slot) + " reports content "
                       + (voice.hasContentFor(slot) ? "present" : "absent")
                       + " with only Sample 1 loaded");

        // Off either end of the choice list. The processor never sends these, but a bad index must
        // read as nothing to play rather than reaching past the slot array.
        expect(! voice.hasContentFor(-1), "Index -1 reports content");
        expect(! voice.hasContentFor(ClickVoice::kNumSlots), "Index past the last slot reports content");
    }

    // (b) Same onset promise as the synthesised types (Phase5ClickVoiceTest (a)), plus the length
    // contract: at matching rates a file of N samples renders N - 1 output samples. The last
    // input sample is reached only as the far end of the final interpolation, never on its own.
    void testPlaysFromTriggerToItsOwnEnd()
    {
        beginTest("(b) A sample sounds on its trigger sample and plays to its own end");

        // Cosine rather than sine so the very first input sample is non-zero; a test of the onset
        // cannot start from content that is itself silent at sample 0.
        const int length = 1000;
        const auto data = sine(1000.0, kSampleRate, length, juce::MathConstants<double>::halfPi);

        const auto hit = render(data, kSampleRate, kSampleRate, length + 500);

        expect(hit[0] != 0.0f, "A sample is silent on its own trigger sample, so its onset is late");

        expectEquals(lastSounding(hit), length - 2,
                     "A " + juce::String(length) + "-sample file did not play exactly to its own end");

        // The shortest file isLoaded() accepts: one interpolation step, one output sample.
        const std::vector<float> shortest { 1.0f, 1.0f };
        const auto blip = render(shortest, kSampleRate, kSampleRate, 64);

        expect(blip[0] != 0.0f, "A two-sample file rendered nothing");
        expectEquals(lastSounding(blip), 0, "A two-sample file rendered past its single step");
    }

    // (c) The decay envelope is deliberately not applied to a sample (see renderSample): its
    // envelope is part of its content, and applying an 8 ms decay would truncate nearly every real
    // click sample with no visible reason why.
    void testDecayDoesNotTruncateASample()
    {
        beginTest("(c) The decay control does not truncate a sample");

        // 20 ms, longer than the shortest decay below and shorter than the longest.
        const auto data = sine(500.0, kSampleRate, (int) (kSampleRate * 0.02));
        const int numSamples = (int) data.size() + 256;

        const auto shortDecay = render(data, kSampleRate, kSampleRate, numSamples, 1.0f, 0.001);
        const auto longDecay  = render(data, kSampleRate, kSampleRate, numSamples, 1.0f, 0.050);

        int diverged = -1;
        for (std::size_t i = 0; i < shortDecay.size(); ++i)
            if (std::memcmp(&shortDecay[i], &longDecay[i], sizeof(float)) != 0)
            {
                diverged = (int) i;
                break;
            }

        expect(diverged < 0, "A sample renders differently at 1 ms and 50 ms decay, first at sample "
                                 + juce::String(diverged) + " - decay is reaching a sample");

        expectEquals(lastSounding(shortDecay), (int) data.size() - 2,
                     "A 1 ms decay cut a 20 ms sample short");
    }

    // (d) A file recorded at one rate played on a device at another must keep its pitch and its
    // duration. Both directions, because they fail differently: upsampling exercises the fractional
    // read head, downsampling exercises an increment above 1 and the end-of-buffer guard.
    void testRateConversionKeepsPitchAndLength()
    {
        beginTest("(d) Rate conversion keeps a sample's pitch and length, up and down");

        struct Case { double source, device; };
        for (const auto c : { Case { 44100.0, 96000.0 }, Case { 96000.0, 44100.0 },
                              Case { 48000.0, 44100.0 } })
        {
            const double toneHz = 200.0;
            const int sourceLength = (int) (c.source * 0.5);
            const auto data = sine(toneHz, c.source, sourceLength);

            const double increment = c.source / c.device;
            const int expectedLength = (int) std::ceil((sourceLength - 1) / increment);
            const auto hit = render(data, c.source, c.device, expectedLength + 512);

            const juce::String label = juce::String(c.source, 0) + " Hz file at "
                                         + juce::String(c.device, 0) + " Hz";

            // ±1 because the read head accumulates a floating increment, and where exactly it
            // crosses the last valid position can fall either side of the exact quotient.
            const int last = lastSounding(hit);
            expect(std::abs((last + 1) - expectedLength) <= 1,
                   label + " rendered " + juce::String(last + 1) + " samples, expected "
                       + juce::String(expectedLength) + " - its duration changed");

            // Positive-going zero crossings over the body of the hit, away from the onset and the
            // filter's settling. Each one is a cycle, so cycles per second is the pitch heard.
            const int from = (int) (c.device * 0.01), to = (int) (c.device * 0.49);
            int crossings = 0;
            for (int i = from + 1; i < to; ++i)
                if (hit[(std::size_t) i - 1] < 0.0f && hit[(std::size_t) i] >= 0.0f)
                    ++crossings;

            const double measuredHz = crossings / ((to - from) / c.device);

            // 96 cycles in the window, so one miscounted crossing is about 1%. A missing or
            // inverted rate conversion is off by the full ratio - at least 8.8% for 48k into 44.1k,
            // the closest pair here, and over 100% for the others.
            expect(std::abs(measuredHz - toneHz) < toneHz * 0.02,
                   label + " plays at " + juce::String(measuredHz, 1) + " Hz instead of "
                       + juce::String(toneHz, 0) + " Hz - its pitch changed");
        }
    }

    // (e) What makes it linear rather than drop-sample. A ramp is the probe: linear interpolation
    // of a ramp is another ramp, so every output step is identical. Drop-sample or nearest-
    // neighbour turns it into a staircase - steps of zero alternating with whole input steps - and
    // (d)'s pitch check cannot tell those apart.
    //
    // The tone filter sits between the interpolation and the output, but a one-pole driven by a
    // ramp settles to the same ramp minus a fixed lag, so once it has settled the output steps
    // equal the input steps exactly. That is why the first few samples are skipped.
    void testRateConversionInterpolatesLinearly()
    {
        beginTest("(e) Rate conversion interpolates linearly between source samples");

        const float slope = 0.001f;
        std::vector<float> ramp(400);
        for (int i = 0; i < (int) ramp.size(); ++i)
            ramp[(std::size_t) i] = slope * (float) i;

        // 2:1 lands on every half-sample; 44.1 into 48 never lands on the same fraction twice.
        for (const double sourceRate : { 22050.0, 44100.0 })
        {
            const double deviceRate = (sourceRate == 22050.0) ? 44100.0 : 48000.0;
            const auto hit = render(ramp, sourceRate, deviceRate, 600);

            const double expectedStep = slope * sourceRate / deviceRate;
            const int settled = 50;
            const int end = lastSounding(hit);

            double worst = 0.0;
            for (int i = settled; i <= end; ++i)
                worst = std::max(worst, std::abs((double) hit[(std::size_t) i]
                                                 - (double) hit[(std::size_t) i - 1] - expectedStep));

            // Against a step of a few times 1e-4: drop-sample is off by the whole step. The
            // tolerance only absorbs float rounding of values up to 0.4.
            expect(worst < expectedStep * 0.01,
                   "Output steps stray " + juce::String(worst, 8) + " from the "
                       + juce::String(expectedStep, 8) + " a linear interpolation of a ramp gives, at "
                       + juce::String(sourceRate, 0) + " into " + juce::String(deviceRate, 0) + " Hz");
        }
    }

    // (f) Two halves. Tone must still mean brightness on a sample, measured the same way as
    // Phase5ClickVoiceTest (e). And it must be a low-pass only, not the band the synthesised types
    // use: subtracting a lower corner would gut a recording's low end, which a DC probe makes
    // unmissable — a low-pass passes it whole, a band passes none of it.
    void testToneIsAPlainLowPass()
    {
        beginTest("(f) Tone darkens a sample, and keeps its low end");

        // Fixed seed, so the content is the same every run.
        juce::Random rng(0x5A3B1E);
        std::vector<float> noise((std::size_t) (kSampleRate * 0.02));
        for (auto& s : noise)
            s = rng.nextFloat() * 2.0f - 1.0f;

        double previous = -1.0;
        for (const float tone : { 0.1f, 0.5f, 0.9f })
        {
            const auto hit = render(noise, kSampleRate, kSampleRate, (int) noise.size(), tone);

            double sumAbs = 0.0, sumDelta = 0.0;
            for (std::size_t i = 1; i < hit.size(); ++i)
            {
                sumAbs += std::abs((double) hit[i]);
                sumDelta += std::abs((double) hit[i] - (double) hit[i - 1]);
            }

            const double brightness = sumDelta / sumAbs;
            expect(brightness > previous,
                   "A sample is no brighter at tone " + juce::String(tone, 1) + " ("
                       + juce::String(brightness, 4) + ") than at the setting below it ("
                       + juce::String(previous, 4) + ")");
            previous = brightness;
        }

        // The darkest setting is the hardest case: its corner is 200 Hz, so a band two octaves
        // below would sit at 50 Hz and still remove DC entirely.
        const std::vector<float> dc(2000, 0.5f);
        const auto held = render(dc, kSampleRate, kSampleRate, (int) dc.size(), 0.0f);

        expect(std::abs(held[1500] - 0.5f) < 1.0e-4f,
               "A constant sample settles at " + juce::String(held[1500], 6)
                   + " instead of 0.5 at the darkest tone - the tone filter is removing low end");
    }

    // (g) The retrigger ramp lives in ClickVoice::processSample and does not care what a hit is
    // playing, but a sample is the one kind of hit that has no decay of its own to end it early,
    // so it is where a missing ramp would ring longest. Same residual method as
    // Phase5ClickVoiceTest (d), for the same reasons — see the comment there.
    void testRetriggerRampsOutAnOutgoingSample()
    {
        beginTest("(g) An outgoing sample ramps out across a retrigger instead of being cut");

        // 100 ms of steady tone, so at the retrigger it has plenty of life left and nothing but the
        // ramp can silence it.
        const auto data = sine(1000.0, kSampleRate, (int) (kSampleRate * 0.1));
        const auto slots = slotsWith(data, kSampleRate);

        const int retriggerAt = 100;
        const int window = 1024;
        const int fadeMustFinishBy = (int) (kSampleRate * 0.010);
        const int levelWindow = 16;

        const auto solo = render(data, kSampleRate, kSampleRate, retriggerAt + window);

        ClickVoice voice;
        voice.prepare(kSampleRate);
        voice.setSampleSlots(slots.data());
        voice.setSlot(kFirstSampleSlot);
        voice.setTone(1.0f);
        voice.noteOn(60);

        std::vector<float> out((std::size_t) (retriggerAt + window), 0.0f);
        for (int i = 0; i < (int) out.size(); ++i)
        {
            if (i == retriggerAt)
                voice.noteOn(60);

            out[(std::size_t) i] = voice.processSample();
        }

        std::vector<float> residual((std::size_t) window, 0.0f);
        for (int k = 0; k < window; ++k)
            residual[(std::size_t) k] = out[(std::size_t) (retriggerAt + k)] - solo[(std::size_t) k];

        float faded = 0.0f, unfaded = 0.0f;
        for (int k = 0; k < levelWindow; ++k)
        {
            faded   = std::max(faded,   std::abs(residual[(std::size_t) k]));
            unfaded = std::max(unfaded, std::abs(solo[(std::size_t) (retriggerAt + k)]));
        }

        expect(faded > unfaded * 0.5f,
               "The outgoing sample vanishes at retrigger (" + juce::String(faded, 6)
                   + " against an uninterrupted " + juce::String(unfaded, 6) + ") - it is being cut");

        int stillRingingAt = -1;
        for (int k = fadeMustFinishBy; k < window; ++k)
            if (residual[(std::size_t) k] != 0.0f)
            {
                stillRingingAt = k;
                break;
            }

        expect(stillRingingAt < 0,
               "The outgoing sample is still sounding " + juce::String(stillRingingAt)
                   + " samples after the retrigger, so it is never being ramped out");
    }
};

static Phase5ClickSamplePlaybackTest phase5ClickSamplePlaybackTest;

// ── Phase 6 ───────────────────────────────────────────────────────────────────

// Shared by the Phase 6 sub tests below: build a voice's snapshot from real models, render it, and
// measure what frequency it actually played.
namespace Phase6Fixtures
{
    constexpr double kSampleRate = 44100.0;

    // The same three tables EnvelopePublisher bakes, so a test exercises the production evaluator
    // rather than a hand-made table that could disagree with it.
    inline EnvelopeSnapshot snapshotOf(const EnvelopeModel& pitch, const EnvelopeModel& amp)
    {
        return EnvelopeSnapshot(EnvelopeEvaluator::buildTable(pitch),
                                EnvelopeEvaluator::buildTable(amp),
                                EnvelopeEvaluator::buildKeytrackTable(pitch),
                                EnvelopeEvaluator::sustainLoopSpan(pitch));
    }

    // A flat, full-level amp envelope, so a frequency measurement is never fighting a decay.
    inline void flatAmp(EnvelopeModel& amp) { amp.addNode(0.0, 1.0); }

    // One note from a freshly prepared voice, triggered at sample 0.
    inline std::vector<float> render(const EnvelopeSnapshot& snapshot, int note, int numSamples,
                                     int keyRoot = 60, float tuneCents = 0.0f)
    {
        SubVoice voice;
        voice.prepare(kSampleRate);
        voice.setSnapshot(&snapshot);
        voice.setTuning(keyRoot, tuneCents);
        voice.noteOn(note);

        std::vector<float> out((std::size_t) numSamples);
        for (auto& s : out)
            s = voice.processSample();
        return out;
    }

    // Frequency over [fromSec, toSec), from positive-going zero crossings located to a fraction of
    // a sample by linear interpolation: (crossings - 1) whole cycles between the first and the
    // last. Precise to well under 0.01% on a steady sine, where counting crossings alone would be
    // off by up to one cycle in the window.
    inline double measureFrequency(const std::vector<float>& buf, double fromSec, double toSec,
                                   double sr = kSampleRate)
    {
        const int from = (int) (fromSec * sr), to = (int) (toSec * sr);
        double first = -1.0, last = -1.0;
        int crossings = 0;

        for (int i = juce::jmax(1, from); i < to; ++i)
        {
            const double a = buf[(std::size_t) i - 1], b = buf[(std::size_t) i];
            if (a < 0.0 && b >= 0.0)
            {
                const double at = (i - 1) + (-a / (b - a));
                if (first < 0.0)
                    first = at;
                last = at;
                ++crossings;
            }
        }

        if (crossings < 2)
            return 0.0;

        return (crossings - 1) * sr / (last - first);
    }
}

// Phase6KeytrackTest — selective keytracking and master tuning (§3.5).
//   (a) The keytrack flag is a real, undoable node property that travels with its node.
//   (b) The weight table: 1 on flagged nodes, 0 on unflagged, a time blend between, held at ends.
//   (c) A keytracked node follows the played note by exactly its distance from the key root.
//   (d) An unflagged node stays where it was drawn whatever is played.
//   (e) Mixed: a flagged attack moves while the unflagged foundation stays put.
//   (f) Master tuning moves everything, flagged or not, and is latched per note.
class Phase6KeytrackTest : public juce::UnitTest
{
public:
    Phase6KeytrackTest() : juce::UnitTest("Phase6Keytrack") {}

    void runTest() override
    {
        testFlagIsAnUndoableNodeProperty();
        testWeightTable();
        testFlaggedNodeFollowsTheNote();
        testUnflaggedNodeStaysPut();
        testFlaggedAttackMovesFoundationStays();
        testMasterTuning();
    }

private:
    static constexpr double kSr = Phase6Fixtures::kSampleRate;

    // Relative error a measured frequency is allowed. The table holds pitch as float and the
    // estimator interpolates crossings, so a correct result lands within a few parts per million;
    // a one-semitone mistake is 6%.
    static constexpr double kTolerance = 1.0e-4;

    void expectFrequency(double measured, double expected, const juce::String& what)
    {
        expect(std::abs(measured - expected) <= expected * kTolerance,
               what + ": measured " + juce::String(measured, 4) + " Hz, expected "
                   + juce::String(expected, 4) + " Hz");
    }

    // (a)
    void testFlagIsAnUndoableNodeProperty()
    {
        beginTest("(a) The keytrack flag is an undoable node property that moves with its node");

        juce::UndoManager undo;
        EnvelopeModel model(&undo);
        model.addNode(0.0, 150.0);
        model.addNode(0.1, 80.0);
        model.addNode(0.3, 50.0);

        expect(! model.getNode(1).keytrack, "A new node starts keytracked");

        model.setKeytrack(1, true);
        expect(model.getNode(1).keytrack, "setKeytrack did not flag the node");

        // Moving it past its neighbour re-sorts it; the flag must go with it, not stay at index 1.
        const int moved = model.moveNode(1, 0.4, 70.0);
        expectEquals(moved, 2, "The node did not re-sort past its neighbour");
        expect(model.getNode(2).keytrack, "The flag did not travel with its node through a re-sort");
        expect(! model.getNode(1).keytrack, "The flag stayed behind at the node's old index");

        // Undo the move, then the flag - each its own step.
        model.undo();
        expect(model.getNode(1).keytrack, "Undoing the move lost the flag");
        model.undo();
        expect(! model.getNode(1).keytrack, "Undoing setKeytrack did not clear the flag");

        // A node saved before Phase 6 has no property at all, and must read as not keytracked.
        auto state = model.getValueTree().createCopy();
        state.getChild(0).removeProperty(EnvelopeIDs::keytrack, nullptr);
        model.setState(state);
        expect(! model.getNode(0).keytrack, "A node with no keytrack property reads as keytracked");

        // And the flag survives the state round trip a preset makes.
        model.setKeytrack(2, true);
        EnvelopeModel reloaded;
        reloaded.setState(model.getValueTree().createCopy());
        expect(reloaded.getNode(2).keytrack, "The flag did not survive a state round trip");
    }

    // (b)
    void testWeightTable()
    {
        beginTest("(b) Weight table: 1 on flagged, 0 on unflagged, a time blend between");

        const double dt = EnvelopeEvaluator::kTableDomainSeconds / EnvelopeEvaluator::kTableSize;
        auto at = [dt](const std::vector<float>& table, double t) { return table[(std::size_t) std::lround(t / dt)]; };

        EnvelopeModel none;
        none.addNode(0.0, 100.0);
        none.addNode(0.5, 50.0);
        const auto zeros = EnvelopeEvaluator::buildKeytrackTable(none);
        expect(std::all_of(zeros.begin(), zeros.end(), [](float w) { return w == 0.0f; }),
               "Nothing is flagged, yet the weight table is not all zero");

        // Flagged at 0.1 and 0.2, unflagged at 0.6.
        EnvelopeModel mixed;
        mixed.addNode(0.1, 120.0);
        mixed.addNode(0.2, 120.0);
        mixed.addNode(0.6, 60.0);
        mixed.setKeytrack(0, true);
        mixed.setKeytrack(1, true);
        const auto table = EnvelopeEvaluator::buildKeytrackTable(mixed);

        expectWithinAbsoluteError(at(table, 0.05), 1.0f, 1.0e-6f, "Not held at the first node's flag before it");
        expectWithinAbsoluteError(at(table, 0.15), 1.0f, 1.0e-6f, "Not fully keytracked between two flagged nodes");
        expectWithinAbsoluteError(at(table, 0.40), 0.5f, 2.0e-3f, "Not halfway, halfway from flagged to unflagged");
        expectWithinAbsoluteError(at(table, 1.00), 0.0f, 1.0e-6f, "Not held at the last node's flag after it");
    }

    // (c) Flat at 100 Hz, the single node flagged, so the whole sound follows the keyboard.
    void testFlaggedNodeFollowsTheNote()
    {
        beginTest("(c) A keytracked node moves by exactly the note's distance from the key root");

        EnvelopeModel pitch, amp;
        pitch.addNode(0.0, 100.0);
        pitch.setKeytrack(0, true);
        Phase6Fixtures::flatAmp(amp);
        const auto snapshot = Phase6Fixtures::snapshotOf(pitch, amp);

        struct Case { int note, root; double expected; };
        for (const auto c : { Case { 60, 60, 100.0 }, Case { 72, 60, 200.0 }, Case { 48, 60, 50.0 },
                              Case { 67, 60, 100.0 * std::exp2(7.0 / 12.0) },
                              Case { 36, 36, 100.0 },   // a different root plays as drawn on itself
                              Case { 48, 36, 200.0 } })
        {
            const auto out = Phase6Fixtures::render(snapshot, c.note, (int) (kSr * 0.5), c.root);
            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.05, 0.45), c.expected,
                            "Note " + juce::String(c.note) + " against root " + juce::String(c.root));
        }
    }

    // (d)
    void testUnflaggedNodeStaysPut()
    {
        beginTest("(d) An unflagged node plays as drawn whatever note is played");

        EnvelopeModel pitch, amp;
        pitch.addNode(0.0, 100.0);
        Phase6Fixtures::flatAmp(amp);
        const auto snapshot = Phase6Fixtures::snapshotOf(pitch, amp);

        for (const int note : { 24, 48, 60, 72, 96 })
        {
            const auto out = Phase6Fixtures::render(snapshot, note, (int) (kSr * 0.5));
            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.05, 0.45), 100.0,
                            "Unflagged node at note " + juce::String(note));
        }
    }

    // (e) The shape §6's verify describes: a flagged plateau on top, an unflagged foundation under
    // it. Each plateau is two nodes at the same value, so there is a stretch of steady pitch to
    // measure rather than a single instant.
    void testFlaggedAttackMovesFoundationStays()
    {
        beginTest("(e) The flagged plateau moves with the note; the unflagged foundation does not");

        EnvelopeModel pitch, amp;
        pitch.addNode(0.0,  120.0);
        pitch.addNode(0.15, 120.0);
        pitch.addNode(0.25, 60.0);
        pitch.addNode(1.0,  60.0);
        pitch.setKeytrack(0, true);
        pitch.setKeytrack(1, true);
        Phase6Fixtures::flatAmp(amp);
        const auto snapshot = Phase6Fixtures::snapshotOf(pitch, amp);

        for (const int offset : { 0, 12, -12, 5 })
        {
            const auto out = Phase6Fixtures::render(snapshot, 60 + offset, (int) (kSr * 1.0));
            const double ratio = std::exp2(offset / 12.0);

            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.01, 0.14), 120.0 * ratio,
                            "Flagged plateau at offset " + juce::String(offset));
            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.30, 0.95), 60.0,
                            "Unflagged foundation at offset " + juce::String(offset));
        }
    }

    // (f) Tuning applies to every node, flagged or not - it moves the reference, not the keyboard.
    // And it is latched at note-on, so turning it mid-note waits for the next hit.
    void testMasterTuning()
    {
        beginTest("(f) Master tuning moves every node by its cents, latched per note");

        EnvelopeModel pitch, amp;
        pitch.addNode(0.0,  120.0);
        pitch.addNode(0.15, 120.0);
        pitch.addNode(0.25, 60.0);
        pitch.addNode(1.0,  60.0);
        pitch.setKeytrack(0, true);
        pitch.setKeytrack(1, true);
        Phase6Fixtures::flatAmp(amp);
        const auto snapshot = Phase6Fixtures::snapshotOf(pitch, amp);

        for (const float cents : { 100.0f, -50.0f, 13.0f })
        {
            const auto out = Phase6Fixtures::render(snapshot, 72, (int) (kSr * 1.0), 60, cents);
            const double tune = std::exp2(cents / 1200.0);

            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.01, 0.14), 240.0 * tune,
                            "Flagged plateau at " + juce::String(cents, 0) + " cents");
            expectFrequency(Phase6Fixtures::measureFrequency(out, 0.30, 0.95), 60.0 * tune,
                            "Unflagged foundation at " + juce::String(cents, 0) + " cents");
        }

        // Retuned after the note started: the note keeps the tuning it began with.
        SubVoice voice;
        voice.prepare(kSr);
        voice.setSnapshot(&snapshot);
        voice.setTuning(60, 0.0f);
        voice.noteOn(60);
        voice.setTuning(60, 100.0f);

        std::vector<float> out((std::size_t) (kSr * 1.0));
        for (auto& s : out)
            s = voice.processSample();

        expectFrequency(Phase6Fixtures::measureFrequency(out, 0.30, 0.95), 60.0,
                        "Retuning mid-note reached the note already playing");
    }
};

static Phase6KeytrackTest phase6KeytrackTest;

// Phase6SustainTest — sustain mode (§3.5: "loop between keytracked nodes while held, then finish
// the tail on release").
//   (a) The loop runs from the first keytracked node to the last, and needs two of them.
//   (b) Held, the envelope keeps looping long past where it would have ended, and every pass
//       replays the same pitch trajectory.
//   (c) Released, it plays on from where it was, through the loop end, and finishes the tail.
//   (d) Sustain off, or released before the loop end, a note-off changes nothing at all.
//   (e) The wrap is click-free even though the loop's two ends differ in level.
//   (f) Monophonic note-offs: an older key's release never cuts the newer note short.
//   (g) With fewer than two keytracked nodes, sustain mode does nothing.
class Phase6SustainTest : public juce::UnitTest
{
public:
    Phase6SustainTest() : juce::UnitTest("Phase6Sustain") {}

    void runTest() override
    {
        testLoopSpan();
        testHeldNoteLoops();
        testReleaseFinishesTheTail();
        testNoteOffChangesNothingWithoutALoop();
        testWrapIsClickFree();
        testMonophonicNoteOffs();
        testNeedsTwoKeytrackedNodes();
    }

private:
    static constexpr double kSr = Phase6Fixtures::kSampleRate;

    // The loop runs 0.05 -> 0.25 s, between the two flagged nodes. Amp is 1.0 at the loop start
    // and 0.5 at its end, so a wrap without the crossfade steps the level by half - the case the
    // crossfade exists for. After the loop the tail settles at 50 Hz and fades to silence at 1 s.
    struct Patch
    {
        EnvelopeModel pitch, amp;

        explicit Patch(bool flagBoth = true)
        {
            pitch.addNode(0.0,  200.0);
            pitch.addNode(0.05, 150.0);
            pitch.addNode(0.25, 100.0);
            pitch.addNode(0.6,  50.0);
            pitch.addNode(1.0,  50.0);
            pitch.setKeytrack(1, true);
            if (flagBoth)
                pitch.setKeytrack(2, true);

            amp.addNode(0.0,  1.0);
            amp.addNode(0.05, 1.0);
            amp.addNode(0.25, 0.5);
            amp.addNode(0.6,  0.5);
            amp.addNode(1.0,  0.0);
        }
    };

    static constexpr juce::int64 kLoopStart = 2205;  // 0.05 s at 44.1 kHz
    static constexpr juce::int64 kLoopEnd   = 11025; // 0.25 s
    static constexpr juce::int64 kLoopLen   = kLoopEnd - kLoopStart;

    struct Event { int sample; bool on; int note; };

    // Renders `events` (in sample order) from a freshly prepared voice.
    static std::vector<float> render(const EnvelopeSnapshot& snapshot, bool sustain,
                                     std::vector<Event> events, int numSamples)
    {
        SubVoice voice;
        voice.prepare(kSr);
        voice.setSnapshot(&snapshot);
        voice.setTuning(60, 0.0f);
        voice.setSustain(sustain);

        std::vector<float> out((std::size_t) numSamples);
        std::size_t next = 0;
        for (int i = 0; i < numSamples; ++i)
        {
            for (; next < events.size() && events[next].sample == i; ++next)
            {
                if (events[next].on) voice.noteOn(events[next].note);
                else                 voice.noteOff(events[next].note);
            }
            out[(std::size_t) i] = voice.processSample();
        }
        return out;
    }

    static double rms(const std::vector<float>& buf, int from, int to)
    {
        double sum = 0.0;
        for (int i = from; i < to; ++i)
            sum += (double) buf[(std::size_t) i] * buf[(std::size_t) i];
        return std::sqrt(sum / (to - from));
    }

    static int lastNonZero(const std::vector<float>& buf)
    {
        for (int i = (int) buf.size() - 1; i >= 0; --i)
            if (buf[(std::size_t) i] != 0.0f)
                return i;
        return -1;
    }

    static int firstDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
                return (int) i;
        return -1;
    }

    // (a)
    void testLoopSpan()
    {
        beginTest("(a) The loop runs from the first keytracked node to the last, and needs two");

        EnvelopeModel model;
        model.addNode(0.0, 200.0);
        model.addNode(0.1, 150.0);
        model.addNode(0.3, 100.0);
        model.addNode(0.5, 60.0);

        expect(! EnvelopeEvaluator::sustainLoopSpan(model).isValid(), "No flagged node, yet a loop");

        model.setKeytrack(1, true);
        expect(! EnvelopeEvaluator::sustainLoopSpan(model).isValid(), "One flagged node, yet a loop");

        model.setKeytrack(3, true);
        auto span = EnvelopeEvaluator::sustainLoopSpan(model);
        expect(span.isValid(), "Two flagged nodes, but no loop");
        expectWithinAbsoluteError(span.start, 0.1, 1.0e-12, "Loop does not start at the first flagged node");
        expectWithinAbsoluteError(span.end,   0.5, 1.0e-12, "Loop does not end at the last flagged node");

        // A flagged node between them widens nothing - the span is first to last.
        model.setKeytrack(2, true);
        span = EnvelopeEvaluator::sustainLoopSpan(model);
        expectWithinAbsoluteError(span.end, 0.5, 1.0e-12, "A middle flagged node moved the loop end");
    }

    // (b)
    void testHeldNoteLoops()
    {
        beginTest("(b) A held note keeps looping, replaying the same pitch trajectory each pass");

        Patch patch;
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);
        const int length = (int) (kSr * 2.0);

        const auto held   = render(snapshot, true,  { { 0, true, 60 } }, length);
        const auto plain  = render(snapshot, false, { { 0, true, 60 } }, length);

        // Unlooped, the amp envelope reaches zero at 1 s and the voice stops.
        expect(lastNonZero(plain) < (int) (kSr * 1.01),
               "Without sustain the note sounds past its envelope's end, at sample "
                   + juce::String(lastNonZero(plain)));

        expect(rms(held, (int) (kSr * 1.8), (int) (kSr * 1.95)) > 0.3,
               "A held sustain note has stopped by 1.8 s; it is not looping");

        // An early and a late stretch of the loop, on pass 1 and on pass 8. The pitch sweeps
        // 150 -> 100 Hz across the loop, so the early stretch must sit clearly above the late one
        // on both passes, and each must match its counterpart on the other pass.
        //
        // 1% rather than the 0.01% the steady-pitch tests hold to: zero crossings on a sweep land
        // wherever the oscillator's phase puts them, and that phase is different on every pass, so
        // a 40 ms window's estimate moves by a few tenths of a percent with it.
        auto passWindow = [&](int pass, double fromEnv, double toEnv)
        {
            const double offset = pass * (double) kLoopLen / kSr;
            return Phase6Fixtures::measureFrequency(held, fromEnv + offset, toEnv + offset);
        };

        for (const auto stretch : { std::pair<double, double> { 0.06, 0.10 }, { 0.18, 0.22 } })
        {
            const double first = passWindow(0, stretch.first, stretch.second);
            const double eighth = passWindow(7, stretch.first, stretch.second);

            expect(std::abs(eighth - first) < first * 0.01,
                   "Envelope " + juce::String(stretch.first, 2) + "-" + juce::String(stretch.second, 2)
                       + " s plays " + juce::String(eighth, 2) + " Hz on pass 8 but "
                       + juce::String(first, 2) + " Hz on pass 1 - the loop is not replaying the same stretch");
        }

        for (const int pass : { 0, 7 })
            expect(passWindow(pass, 0.06, 0.10) > passWindow(pass, 0.18, 0.22) + 20.0,
                   "Pass " + juce::String(pass + 1) + " does not sweep down through the loop");
    }

    // (c) Released mid-pass at 1.33 s, the loop position is 0.13 s into the envelope (pass 6), so
    // the rest of the loop, then the 0.25 -> 1.0 s tail, still has to play.
    void testReleaseFinishesTheTail()
    {
        beginTest("(c) Released, the note plays on through the loop end and finishes the tail");

        Patch patch;
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);

        const int release = (int) (kSr * 1.33);
        const auto out = render(snapshot, true, { { 0, true, 60 }, { release, false, 60 } },
                                release + (int) (kSr * 1.5));

        const int positionAtRelease = (int) (kLoopStart + (release - kLoopEnd) % kLoopLen);

        // Not cut: the next 0.1 s is still loud.
        expect(rms(out, release, release + (int) (kSr * 0.1)) > 0.3, "Releasing cut the note off");

        // The tail's 50 Hz plateau, envelope time 0.65 -> 0.9 s.
        const int tailFrom = release + (int) (kSr * 0.65) - positionAtRelease;
        const int tailTo   = release + (int) (kSr * 0.90) - positionAtRelease;
        const double tailHz = Phase6Fixtures::measureFrequency(out, tailFrom / kSr, tailTo / kSr);
        expect(std::abs(tailHz - 50.0) < 0.05, "The tail after release plays " + juce::String(tailHz, 3)
                                                   + " Hz, not the 50 Hz foundation");

        // And it ends where the envelope does: 1.0 s of envelope time, give or take the idle level.
        const int expectedEnd = release + (int) kSr - positionAtRelease;
        const int end = lastNonZero(out);
        expect(std::abs(end - expectedEnd) < (int) (kSr * 0.001),
               "The released note ends at sample " + juce::String(end) + ", expected about "
                   + juce::String(expectedEnd) + " where its tail runs out");
    }

    // (d) The two cases where sustain must leave no trace at all, bit for bit.
    void testNoteOffChangesNothingWithoutALoop()
    {
        beginTest("(d) Without sustain, or released before the loop end, a note-off changes nothing");

        Patch patch;
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);
        const int length = (int) (kSr * 1.2);

        const auto untouched = render(snapshot, false, { { 0, true, 60 } }, length);

        const auto offWithoutSustain = render(snapshot, false, { { 0, true, 60 }, { 4410, false, 60 } }, length);
        expectEquals(firstDifference(untouched, offWithoutSustain), -1,
                     "A note-off changed the render with sustain off");

        // Released at 0.2 s, before the crossfade into the first wrap starts (0.24 s).
        const auto earlyRelease = render(snapshot, true, { { 0, true, 60 }, { 8820, false, 60 } }, length);
        expectEquals(firstDifference(untouched, earlyRelease), -1,
                     "A note released before the loop end still looped");
    }

    // (e) A plain jump from the loop end back to its start would step the level from 0.5 to 1.0.
    // The biggest sample-to-sample change a 150 Hz sine at full level can make on its own is
    // 2 pi 150 / 44100 = 0.0214, so anything clearly above that inside the loop is a click.
    void testWrapIsClickFree()
    {
        beginTest("(e) Wrapping from the loop end to its start does not click");

        Patch patch;
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);
        const auto held = render(snapshot, true, { { 0, true, 60 } }, (int) (kSr * 2.0));

        const double ceiling = juce::MathConstants<double>::twoPi * 150.0 / kSr * 1.05;

        double worst = 0.0;
        int worstAt = -1;
        for (int i = (int) kLoopStart + 1; i < (int) held.size(); ++i)
        {
            const double jump = std::abs((double) held[(std::size_t) i] - held[(std::size_t) i - 1]);
            if (jump > worst) { worst = jump; worstAt = i; }
        }

        expect(worst < ceiling, "A jump of " + juce::String(worst, 4) + " at sample "
                                    + juce::String(worstAt) + " - more than a 150 Hz sine can move ("
                                    + juce::String(ceiling, 4) + ") - the wrap is clicking");
    }

    // (f)
    void testMonophonicNoteOffs()
    {
        beginTest("(f) An older key's release never cuts the newer note short");

        Patch patch;
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);
        const int length = (int) (kSr * 2.5);

        // 60 down, 64 down at 0.5 s (chokes 60), 60 released at 0.8 s: 64 must still be looping.
        const auto overlapped = render(snapshot, true,
                                       { { 0, true, 60 }, { 22050, true, 64 }, { 35280, false, 60 } }, length);
        expect(rms(overlapped, (int) (kSr * 2.2), (int) (kSr * 2.4)) > 0.3,
               "Releasing the older key stopped the newer note's loop");

        // 64 released while still inside the choke that leads into it: it starts already released.
        const auto releasedInChoke = render(snapshot, true,
                                            { { 0, true, 60 }, { 22050, true, 64 }, { 22060, false, 64 } }, length);
        expect(lastNonZero(releasedInChoke) < (int) (kSr * 1.6),
               "A note released during the choke into it went on looping, to sample "
                   + juce::String(lastNonZero(releasedInChoke)));
    }

    // (g)
    void testNeedsTwoKeytrackedNodes()
    {
        beginTest("(g) With one keytracked node, sustain mode does nothing");

        Patch patch(false);
        const auto snapshot = Phase6Fixtures::snapshotOf(patch.pitch, patch.amp);
        const int length = (int) (kSr * 1.2);

        const auto withSustain    = render(snapshot, true,  { { 0, true, 60 } }, length);
        const auto withoutSustain = render(snapshot, false, { { 0, true, 60 } }, length);

        expectEquals(firstDifference(withSustain, withoutSustain), -1,
                     "Sustain changed a patch with only one keytracked node");
    }
};

static Phase6SustainTest phase6SustainTest;

// Phase6HarmonicsTest — the harmonic series above the Sub's sine (§3.1: Gain / Ratio / Decay + presets).
//   (a) Gain 0 is the plain sine, bit for bit.
//   (b) Each preset puts energy in exactly its own harmonics.
//   (c) The first harmonic present sits at Gain, and each further one at Ratio times the last.
//   (d) Decay is a real duration: afterwards the output is the plain sine again, bit for bit.
//   (e) Harmonics near Nyquist are dropped rather than folding back as aliases.
//   (f) Latched at note-on.
//
// Levels are read with a Hann-windowed Goertzel filter over windows holding a whole number of
// cycles of a flat 100 Hz tone, so every harmonic lands exactly on a bin. The window matters: the
// harmonics decay within the measurement, and a decaying tone measured through a plain rectangular
// window smears into its neighbours' bins at around -50 dB - enough to make an absent harmonic look
// present. Hann's sidelobes are below -90 dB ten bins out, where the next harmonic sits.
class Phase6HarmonicsTest : public juce::UnitTest
{
public:
    Phase6HarmonicsTest() : juce::UnitTest("Phase6Harmonics") {}

    void runTest() override
    {
        testZeroGainIsThePlainSine();
        testPresetsSelectTheirHarmonics();
        testGainAndRatioSetTheLevels();
        testDecayIsARealDuration();
        testNoAliasing();
        testLatchedAtNoteOn();
    }

private:
    static constexpr double kSr = Phase6Fixtures::kSampleRate;
    using Set = SubVoice::HarmonicSet;

    struct Settings { float gain = 0.5f, ratio = 0.5f; double decay = 2.0; Set set = Set::all; };

    static EnvelopeSnapshot flatTone(double hz)
    {
        EnvelopeModel pitch, amp;
        pitch.addNode(0.0, hz);
        Phase6Fixtures::flatAmp(amp);
        return Phase6Fixtures::snapshotOf(pitch, amp);
    }

    static std::vector<float> render(const EnvelopeSnapshot& snapshot, Settings h, int numSamples)
    {
        SubVoice voice;
        voice.prepare(kSr);
        voice.setSnapshot(&snapshot);
        voice.setTuning(60, 0.0f);
        voice.setHarmonics(h.gain, h.ratio, h.decay, h.set);
        voice.noteOn(60);

        std::vector<float> out((std::size_t) numSamples);
        for (auto& s : out)
            s = voice.processSample();
        return out;
    }

    // Periodic Hann weight for sample n of a count-sample window.
    static double hann(int n, int count)
    {
        return 0.5 * (1.0 - std::cos(juce::MathConstants<double>::twoPi * n / count));
    }

    // Amplitude of the component at `hz` over [from, from + count), by Hann-windowed Goertzel.
    // Scaled by the window's sum, so a steady on-bin tone of amplitude A reads exactly A.
    static double amplitudeAt(const std::vector<float>& buf, double hz, int from, int count)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / kSr;
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0, windowSum = 0.0;

        for (int n = 0; n < count; ++n)
        {
            const double weight = hann(n, count);
            windowSum += weight;

            const double s0 = buf[(std::size_t) (from + n)] * weight + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt(std::max(power, 0.0)) / windowSum;
    }

    // 10 cycles of 100 Hz exactly.
    static constexpr int kWindow = 4410;

    static int firstDifference(const std::vector<float>& a, const std::vector<float>& b)
    {
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
                return (int) i;
        return -1;
    }

    // (a)
    void testZeroGainIsThePlainSine()
    {
        beginTest("(a) Gain 0 renders the plain sine, bit for bit");

        const auto snapshot = flatTone(100.0);
        Settings off;
        off.gain = 0.0f;

        SubVoice voice;
        voice.prepare(kSr);
        voice.setSnapshot(&snapshot);
        voice.noteOn(60); // never told about harmonics at all

        std::vector<float> untouched((std::size_t) kWindow);
        for (auto& s : untouched)
            s = voice.processSample();

        expectEquals(firstDifference(render(snapshot, off, kWindow), untouched), -1,
                     "Harmonics at gain 0 changed the sound");
    }

    // (b)
    void testPresetsSelectTheirHarmonics()
    {
        beginTest("(b) Each preset puts energy in exactly its own harmonics");

        const auto snapshot = flatTone(100.0);

        struct Case { Set set; const char* name; std::vector<int> present; };
        for (const auto& c : { Case { Set::all,     "All",     { 2, 3, 4, 5, 6, 7, 8 } },
                               Case { Set::odd,     "Odd",     { 3, 5, 7 } },
                               Case { Set::even,    "Even",    { 2, 4, 6, 8 } },
                               Case { Set::octaves, "Octaves", { 2, 4, 8 } } })
        {
            Settings h;
            h.set = c.set;
            h.ratio = 0.8f; // gentle falloff, so the 8th is still well above the noise floor
            const auto out = render(snapshot, h, kWindow);

            for (int harmonic = 2; harmonic <= SubVoice::kMaxHarmonic; ++harmonic)
            {
                const double level = amplitudeAt(out, 100.0 * harmonic, 0, kWindow);
                const bool shouldBe = std::find(c.present.begin(), c.present.end(), harmonic) != c.present.end();

                if (shouldBe)
                    expect(level > 0.05, juce::String(c.name) + ": harmonic " + juce::String(harmonic)
                                             + " is missing (" + juce::String(level, 5) + ")");
                else
                    expect(level < 1.0e-4, juce::String(c.name) + ": harmonic " + juce::String(harmonic)
                                               + " is present (" + juce::String(level, 5) + ") but not in the set");
            }
        }
    }

    // (c) Measured against the fundamental in the same window, so the shared decay envelope and
    // the amp envelope cancel out of the ratio entirely.
    void testGainAndRatioSetTheLevels()
    {
        beginTest("(c) First harmonic at Gain against the fundamental, each next one Ratio times the last");

        const auto snapshot = flatTone(100.0);

        for (const auto& c : { Settings { 0.5f, 0.5f, 2.0, Set::all },
                               Settings { 0.3f, 0.25f, 2.0, Set::odd },
                               Settings { 0.8f, 0.7f, 2.0, Set::octaves } })
        {
            const auto out = render(snapshot, c, kWindow);

            // The series fades as e^(-kt), k = ln(1e4) / decay, while the fundamental does not, so
            // the first harmonic reads as gain times that envelope's Hann-weighted average over
            // the window. Every later harmonic shares the envelope, so it cancels from their ratios.
            const double k = std::log(1.0e4) / c.decay;
            double weighted = 0.0, weights = 0.0;
            for (int n = 0; n < kWindow; ++n)
            {
                weighted += hann(n, kWindow) * std::exp(-k * n / kSr);
                weights  += hann(n, kWindow);
            }
            const double meanEnvelope = weighted / weights;

            const double fundamental = amplitudeAt(out, 100.0, 0, kWindow);
            expectWithinAbsoluteError(fundamental, 1.0, 1.0e-3, "The fundamental is not at full level");

            std::vector<int> set;
            for (int harmonic = 2; harmonic <= SubVoice::kMaxHarmonic; ++harmonic)
                if (amplitudeAt(out, 100.0 * harmonic, 0, kWindow) > 1.0e-4)
                    set.push_back(harmonic);

            double expected = c.gain * meanEnvelope;
            for (const int harmonic : set)
            {
                const double level = amplitudeAt(out, 100.0 * harmonic, 0, kWindow) / fundamental;
                expect(std::abs(level - expected) < expected * 1.0e-3 + 1.0e-6,
                       "Harmonic " + juce::String(harmonic) + " at gain " + juce::String(c.gain, 2)
                           + ", ratio " + juce::String(c.ratio, 2) + ": " + juce::String(level, 6)
                           + " against an expected " + juce::String(expected, 6));
                expected *= c.ratio;
            }
        }
    }

    // (d)
    void testDecayIsARealDuration()
    {
        beginTest("(d) After Decay the output is the plain sine again, bit for bit");

        const auto snapshot = flatTone(100.0);
        Settings plain;
        plain.gain = 0.0f;

        for (const double decay : { 0.02, 0.15 })
        {
            Settings h;
            h.decay = decay;
            const int length = (int) (kSr * (decay + 0.1));

            const auto with = render(snapshot, h, length);
            const auto without = render(snapshot, plain, length);
            const int end = (int) std::ceil(decay * kSr);

            const int differs = firstDifference(with, without);
            expect(differs >= 0 && differs < end / 4, "Harmonics are not audible at the start of a "
                                                         + juce::String(decay * 1000.0, 0) + " ms decay");

            int lastDiff = -1;
            for (int i = 0; i < length; ++i)
                if (std::memcmp(&with[(std::size_t) i], &without[(std::size_t) i], sizeof(float)) != 0)
                    lastDiff = i;

            expect(lastDiff < end, "Harmonics are still sounding at sample " + juce::String(lastDiff)
                                       + ", past their " + juce::String(decay * 1000.0, 0)
                                       + " ms decay (sample " + juce::String(end) + ")");
            expect(lastDiff > end - (int) (kSr * 0.01), "Harmonics stopped well before their decay ended");
        }
    }

    // (e) A 3 kHz fundamental at 44.1 kHz: the 7th (21 kHz) and 8th (24 kHz) are above the 19.8 kHz
    // ceiling. The 8th would fold back to 20.1 kHz, so that frequency is checked as well.
    void testNoAliasing()
    {
        beginTest("(e) Harmonics above the ceiling are dropped, not aliased");

        const auto snapshot = flatTone(3000.0);
        Settings h;
        h.ratio = 1.0f; // no falloff: an alias would be as loud as the harmonic it came from
        const int count = 4410; // 300 cycles of 3 kHz exactly
        const auto out = render(snapshot, h, count);

        expect(amplitudeAt(out, 15000.0, 0, count) > 0.1, "The 5th harmonic (15 kHz) is missing");
        for (const double hz : { 21000.0, 44100.0 - 24000.0 })
            expect(amplitudeAt(out, hz, 0, count) < 1.0e-4,
                   "Energy at " + juce::String(hz, 0) + " Hz - a harmonic above the ceiling is sounding or aliasing");
    }

    // (f)
    void testLatchedAtNoteOn()
    {
        beginTest("(f) Harmonic settings are latched at note-on");

        const auto snapshot = flatTone(100.0);

        SubVoice voice;
        voice.prepare(kSr);
        voice.setSnapshot(&snapshot);
        voice.setHarmonics(0.0f, 0.5f, 2.0, Set::all);
        voice.noteOn(60);
        voice.setHarmonics(0.9f, 0.5f, 2.0, Set::all);

        std::vector<float> out((std::size_t) kWindow);
        for (auto& s : out)
            s = voice.processSample();

        expect(amplitudeAt(out, 200.0, 0, kWindow) < 1.0e-4,
               "Raising gain mid-note added harmonics to the note already playing");
    }
};

static Phase6HarmonicsTest phase6HarmonicsTest;

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
