#include <juce_core/juce_core.h>
#include "SubVoice.h"
#include "EnvelopeModel.h"

#include <cmath>
#include <vector>

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

    // Helper: render numSamples from a freshly prepared voice into a vector.
    // noteOnAt is the sample index at which the first noteOn fires.
    std::vector<float> render(int numSamples, int noteOnAt = 0)
    {
        SubVoice v;
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
        expect(lateRms  < 0.002,   "Late RMS should be near-zero at 2.5–3 s (tail fully decayed)");
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
        SubVoice v;
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

        SubVoice v;
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
};

static EnvelopeModelTest envelopeModelTest;

// ── Test runner entry point ────────────────────────────────────────────────────

// Runs all registered juce::UnitTest subclasses and returns a non-zero exit code
// on any failure so CTest reports the test as failed.
int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult(i)->failures;

    return failures > 0 ? 1 : 0;
}
