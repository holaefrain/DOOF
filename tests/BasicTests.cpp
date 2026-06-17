#include <juce_core/juce_core.h>
#include "SubVoice.h"

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
