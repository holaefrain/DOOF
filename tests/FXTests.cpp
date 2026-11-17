#include "FXEngine.h"
#include "FXModule.h"
#include "MacroAssignments.h"
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <functional>
#include <vector>

// FXTests — Phase 7's FX section, headless: the module catalogue, the slot (bypass, mix, module
// handoff), every module's measured response against its spec, oversampling, control smoothing, the
// engine's routing, route alignment and macros, and the macro assignment model. Everything here
// runs below the processor; the processor-level and GUI checks are in IntegrationTests.

namespace FXFixtures
{
    constexpr double kRate  = 44100.0;
    constexpr int    kBlock = 512;

    // A module of `type`, prepared at the fixture rate.
    inline std::unique_ptr<FXModule> makeModule(FX::ModuleType type, int oversamplingLog2 = 0)
    {
        auto module = FX::createModule(type, oversamplingLog2);
        module->prepare(kRate, kBlock);
        return module;
    }

    // A module's six controls at its defaults (unused ones at 0.5, as an empty slot leaves them).
    inline FX::SlotValues defaults(FX::ModuleType type)
    {
        FX::SlotValues values {};
        const auto& info = FX::getInfo(type);
        for (size_t k = 0; k < values.size(); ++k)
            values[k] = FX::defaultNormalised(info.params[k]);
        return values;
    }

    // Sets one control by its real value ("12 dB", "1000 Hz"), through the catalogue's own mapping.
    inline void set(FX::SlotValues& values, FX::ModuleType type, int index, float realValue)
    {
        values[(size_t) index] = FX::toNormalised(FX::getInfo(type).params[(size_t) index], realValue);
    }

    struct Stereo
    {
        std::vector<float> left, right;
        explicit Stereo(size_t n = 0) : left(n, 0.0f), right(n, 0.0f) {}
        size_t size() const { return left.size(); }
    };

    inline Stereo mono(const std::vector<float>& signal)
    {
        Stereo s(signal.size());
        s.left = signal;
        s.right = signal;
        return s;
    }

    // Runs a module over a whole signal in fixture-sized blocks, setting its controls every block.
    // `changeAt` lets a test change the controls from a given block on.
    inline Stereo run(FXModule& module, Stereo signal, const FX::SlotValues& values,
                      const FX::BlockContext& context = {},
                      const std::function<void(int block, FX::SlotValues&)>& changeAt = nullptr)
    {
        auto current = values;
        for (size_t start = 0, block = 0; start < signal.size(); start += kBlock, ++block)
        {
            if (changeAt)
                changeAt((int) block, current);

            const int n = (int) std::min<size_t>(kBlock, signal.size() - start);
            module.setParameters(current, context);
            module.process(signal.left.data() + start, signal.right.data() + start, n);
        }
        return signal;
    }

    inline std::vector<float> sine(double hz, double amplitude, int length, double sampleRate = kRate)
    {
        std::vector<float> s((size_t) length);
        for (int i = 0; i < length; ++i)
            s[(size_t) i] = (float) (amplitude * std::sin(juce::MathConstants<double>::twoPi * hz * i / sampleRate));
        return s;
    }

    // The amplitude of the component at exactly `hz`, from a Hann-windowed Goertzel over
    // [start, start + length). Evaluated at the frequency itself, so there is no bin scalloping, and
    // Hann keeps neighbouring components from leaking in.
    inline double amplitudeAt(const std::vector<float>& x, double hz, int start, int length, double sampleRate = kRate)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / sampleRate;
        const double coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0, windowSum = 0.0;

        for (int n = 0; n < length; ++n)
        {
            const double window = 0.5 * (1.0 - std::cos(juce::MathConstants<double>::twoPi * n / (length - 1)));
            const double v = x[(size_t) (start + n)] * window + coeff * s1 - s2;
            s2 = s1;
            s1 = v;
            windowSum += window;
        }

        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        return 2.0 * std::sqrt(juce::jmax(0.0, power)) / windowSum;
    }

    inline double toDb(double ratio) { return 20.0 * std::log10(juce::jmax(1.0e-12, ratio)); }

    // Steady-state gain of a module at one frequency, in dB: a sine through it for 0.75 s, measured
    // over the last half second against the same measurement of the input.
    inline double gainDb(FX::ModuleType type, const FX::SlotValues& values, double hz, double amplitude = 0.25,
                         int oversamplingLog2 = 0)
    {
        auto module = makeModule(type, oversamplingLog2);
        const int length = (int) (0.75 * kRate);
        const auto input = sine(hz, amplitude, length);
        const auto output = run(*module, mono(input), values);
        const int window = (int) (0.5 * kRate);
        return toDb(amplitudeAt(output.left, hz, length - window, window) / amplitudeAt(input, hz, length - window, window));
    }

    inline float maxAbs(const std::vector<float>& x, size_t start = 0, size_t end = SIZE_MAX)
    {
        float peak = 0.0f;
        for (size_t i = start; i < std::min(end, x.size()); ++i)
            peak = std::max(peak, std::abs(x[i]));
        return peak;
    }

    inline float maxStep(const std::vector<float>& x, size_t start = 1, size_t end = SIZE_MAX)
    {
        float step = 0.0f;
        for (size_t i = std::max<size_t>(1, start); i < std::min(end, x.size()); ++i)
            step = std::max(step, std::abs(x[i] - x[i - 1]));
        return step;
    }

    inline bool identical(const std::vector<float>& a, const std::vector<float>& b, size_t offset = 0)
    {
        // b[i] is compared against a[i - offset]; the first `offset` samples of b must be silent.
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < b.size(); ++i)
        {
            const float expected = i < offset ? 0.0f : a[i - offset];
            if (! juce::exactlyEqual(expected, b[i]))
                return false;
        }
        return true;
    }

    // Every module type but None.
    inline std::vector<FX::ModuleType> allModules()
    {
        std::vector<FX::ModuleType> types;
        for (int t = 1; t < FX::kNumModuleTypes; ++t)
            types.push_back((FX::ModuleType) t);
        return types;
    }

    // A recognisable test signal: a decaying 55 Hz "kick" plus a 3 kHz click, so both ends of the
    // spectrum are exercised.
    inline std::vector<float> kickLike(int length)
    {
        std::vector<float> s((size_t) length);
        for (int i = 0; i < length; ++i)
        {
            const double t = i / kRate;
            s[(size_t) i] = (float) (0.8 * std::exp(-t * 6.0) * std::sin(juce::MathConstants<double>::twoPi * 55.0 * t)
                                   + 0.3 * std::exp(-t * 200.0) * std::sin(juce::MathConstants<double>::twoPi * 3000.0 * t));
        }
        return s;
    }
}

using namespace FXFixtures;

// ── Catalogue ────────────────────────────────────────────────────────────────

class Phase7CatalogueTest : public juce::UnitTest
{
public:
    Phase7CatalogueTest() : juce::UnitTest("Phase7Catalogue") {}

    void runTest() override
    {
        beginTest("(a) Fourteen module types, permanently: None plus §3.7's thirteen");
        {
            // Spelled out: these are stored choice indices, and the list can never grow or reorder.
            const juce::StringArray expected { "None", "Clip", "Tube", "Wave", "Tape", "Drive", "Bit Crusher",
                                               "Ring Mod", "Filter", "EQ", "Compressor", "Dual Comp", "Delay", "Reverb" };
            expect(FX::moduleTypeChoices() == expected, "Module list: " + FX::moduleTypeChoices().joinIntoString(", "));
            expectEquals(FX::kNumModuleTypes, 14);

            for (const auto& spec : FX::getInfo(FX::ModuleType::none).params)
                expect(! spec.isUsed(), "None has no controls");

            for (auto type : allModules())
            {
                int used = 0;
                for (const auto& spec : FX::getInfo(type).params)
                    used += spec.isUsed() ? 1 : 0;
                expect(used >= 3, juce::String(FX::getInfo(type).name) + " should use at least three controls");
            }

            expectEquals(FX::kNumSlots, 18, "6 slots x (Insert A, Insert B, Master) - user decision 2026-11-17");
            expect(FX::routeChoices() == juce::StringArray { "Dry", "Insert A", "Insert B", "Both" });
            expect(FX::oversamplingChoices() == juce::StringArray { "Off", "2x", "4x", "8x" });
        }

        beginTest("(b) Every control maps, round-trips and reads back its own text");
        for (auto type : allModules())
        {
            const auto& info = FX::getInfo(type);
            for (const auto& spec : info.params)
            {
                if (! spec.isUsed())
                    continue;

                const juce::String where = juce::String(info.name) + " " + spec.name;
                const float def = FX::defaultNormalised(spec);
                expect(def >= 0.0f && def <= 1.0f, where + " default out of range");
                expectWithinAbsoluteError(FX::toValue(spec, def), spec.defaultValue,
                                          spec.isDiscrete() ? 0.0f : std::abs(spec.defaultValue) * 1.0e-4f + 1.0e-4f,
                                          where + " default does not map back to itself");
                expectWithinAbsoluteError(FX::toValue(spec, 0.0f), spec.minValue, 1.0e-4f * (1.0f + std::abs(spec.minValue)), where);
                expectWithinAbsoluteError(FX::toValue(spec, 1.0f), spec.maxValue, 1.0e-4f * (1.0f + std::abs(spec.maxValue)), where);

                for (float n = 0.0f; n <= 1.0f; n += 0.0625f)
                {
                    const auto text = FX::toText(spec, n);
                    expect(text.isNotEmpty(), where + " has no text");

                    // Typing back what is displayed lands on the same value, to the precision the
                    // text shows: whole percent, a tenth of a dB or ratio, three figures otherwise.
                    const float again = FX::toValue(spec, FX::fromText(spec, text));
                    const float value = FX::toValue(spec, n);
                    const float resolution = spec.unit == FX::Unit::percent ? 0.5f
                                           : (spec.unit == FX::Unit::decibels || spec.unit == FX::Unit::ratio) ? 0.05f
                                           : juce::jmax(std::abs(value) * 0.006f, 0.006f);
                    const float tolerance = spec.isDiscrete() ? 0.0f : resolution + 1.0e-4f;
                    expectWithinAbsoluteError(again, value, tolerance, where + ": '" + text + "' read back as " + juce::String(again));
                }
            }

            expect(info.defaultMix > 0.0f && info.defaultMix <= 1.0f);
        }

        beginTest("(c) Display text reads in each control's own unit");
        {
            const auto& filter = FX::getInfo(FX::ModuleType::filter).params;
            expectEquals(FX::toText(filter[1], FX::toNormalised(filter[1], 2000.0f)), juce::String("2.00 kHz"));
            expectEquals(FX::toText(filter[1], FX::toNormalised(filter[1], 250.0f)), juce::String("250 Hz"));
            expectEquals(FX::toText(filter[0], 0.0f), juce::String("Low Pass"));
            expectEquals(FX::toText(filter[3], 1.0f), juce::String("24 dB"));

            const auto& comp = FX::getInfo(FX::ModuleType::compressor).params;
            expectEquals(FX::toText(comp[0], FX::toNormalised(comp[0], -12.0f)), juce::String("-12.0 dB"));
            expectEquals(FX::toText(comp[1], FX::toNormalised(comp[1], 4.0f)), juce::String("4.0:1"));
            expectEquals(FX::toText(comp[2], FX::toNormalised(comp[2], 2.5f)), juce::String("2.50 ms"));

            const auto& crusher = FX::getInfo(FX::ModuleType::bitCrusher).params;
            expectEquals(FX::toText(crusher[0], FX::toNormalised(crusher[0], 8.0f)), juce::String("8 bit"));

            const auto& delay = FX::getInfo(FX::ModuleType::delay).params;
            expectEquals(FX::toText(delay[0], FX::toNormalised(delay[0], 1500.0f)), juce::String("1.50 s"));
            expectEquals(FX::toText(delay[5], FX::toNormalised(delay[5], 7.0f)), juce::String("1/8"));

            const auto& reverb = FX::getInfo(FX::ModuleType::reverb).params;
            expectEquals(FX::toText(reverb[0], FX::toNormalised(reverb[0], 1.2f)), juce::String("1.20 s"));

            // Typed entry accepts the obvious spellings.
            expectWithinAbsoluteError(FX::toValue(filter[1], FX::fromText(filter[1], "2k")), 2000.0f, 0.5f);
            expectWithinAbsoluteError(FX::toValue(delay[0], FX::fromText(delay[0], "1.5 s")), 1500.0f, 0.5f);
            expectWithinAbsoluteError(FX::toValue(filter[0], FX::fromText(filter[0], "notch")), 3.0f, 0.0f);
        }

        beginTest("(d) Slot indexing, and tempo divisions in beats");
        {
            expectEquals(FX::slotIndex(FX::Channel::insertA, 0), 0);
            expectEquals(FX::slotIndex(FX::Channel::insertB, 0), 6);
            expectEquals(FX::slotIndex(FX::Channel::master, 5), 17);
            expect(FX::channelOf(13) == FX::Channel::master || FX::channelOf(13) == FX::Channel::insertB);
            expect(FX::channelOf(12) == FX::Channel::master);
            expectEquals(FX::slotInChannel(13), 1);

            expectWithinAbsoluteError(FX::divisionBeats(7), 0.5, 1.0e-12);   // 1/8
            expectWithinAbsoluteError(FX::divisionBeats(10), 1.0, 1.0e-12);  // 1/4
            expectWithinAbsoluteError(FX::divisionBeats(14), 4.0, 1.0e-12);  // 1/1
            for (int i = 1; i < FX::kNumDivisions; ++i)
                expect(FX::divisionBeats(i) > FX::divisionBeats(i - 1), "Divisions must ascend");
        }
    }
};

static Phase7CatalogueTest phase7CatalogueTest;

// ── Slot ─────────────────────────────────────────────────────────────────────

namespace
{
    // A module that scales by a gain and counts its own destructions, so the slot's handoff and
    // retirement can be observed.
    class ScaleModule final : public FXModule
    {
    public:
        explicit ScaleModule(float g, int latencySamples = 0)
            : FXModule(FX::ModuleType::filter), gain(g), latency(latencySamples) {}
        ~ScaleModule() override { ++destroyed; }

        void prepare(double, int) override {}
        void reset() noexcept override { ++resets; }
        int getLatencySamples() const noexcept override { return latency; }
        void setParameters(const FX::SlotValues&, const FX::BlockContext&) noexcept override {}
        void process(float* l, float* r, int n) noexcept override
        {
            for (int i = 0; i < n; ++i) { l[i] *= gain; r[i] *= gain; }
        }

        static inline int destroyed = 0;
        int resets = 0;

    private:
        const float gain;
        const int latency;
    };

    // Runs a slot over a signal in blocks.
    Stereo runSlot(FXSlot& slot, Stereo signal, const FX::SlotParams& params, FXRetiredModules& retired,
                   const std::function<void(int, FX::SlotParams&)>& changeAt = nullptr)
    {
        auto current = params;
        for (size_t start = 0, block = 0; start < signal.size(); start += kBlock, ++block)
        {
            if (changeAt)
                changeAt((int) block, current);
            const int n = (int) std::min<size_t>(kBlock, signal.size() - start);
            slot.process(signal.left.data() + start, signal.right.data() + start, n, current, {}, retired);
        }
        return signal;
    }
}

class Phase7SlotTest : public juce::UnitTest
{
public:
    Phase7SlotTest() : juce::UnitTest("Phase7Slot") {}

    void runTest() override
    {
        const int maxLatency = FX::oversamplingLatency(FX::kMaxOversamplingLog2, kBlock);
        const auto input = mono(kickLike(kBlock * 12));

        beginTest("(a) An empty slot is an exact passthrough");
        {
            FXSlot slot;
            FXRetiredModules retired;
            slot.prepare(kRate, kBlock, maxLatency);
            slot.installNow(FX::createModule(FX::ModuleType::none, 0));

            const auto out = runSlot(slot, input, {}, retired);
            expect(identical(input.left, out.left) && identical(input.right, out.right));
            expectEquals(slot.getLatencySamples(), 0);
        }

        beginTest("(b) Bypass is bit-identical for every module (§6) - and exactly one latency late when oversampled");
        for (int os : { 0, 2 })
        {
            for (auto type : allModules())
            {
                FXSlot slot;
                FXRetiredModules retired;
                slot.prepare(kRate, kBlock, maxLatency);
                auto module = makeModule(type, os);
                const int latency = module->getLatencySamples();
                slot.installNow(std::move(module));

                FX::SlotParams params;
                params.type = (int) type;
                params.on = false;
                params.values = defaults(type);

                const auto out = runSlot(slot, input, params, retired);
                const juce::String where = juce::String(FX::getInfo(type).name) + (os > 0 ? " at 4x" : " at Off");

                expect(identical(input.left, out.left, (size_t) latency) && identical(input.right, out.right, (size_t) latency),
                       where + ": bypassed output differs from the input delayed by " + juce::String(latency));

                if (os == 0 || ! FX::getInfo(type).oversampled)
                    expectEquals(latency, 0, where + " should add no latency");
                else
                    expect(latency > 0, where + " should report its oversampling latency");
            }
        }

        beginTest("(c) Engaged at full mix, the slot's output is exactly the module's");
        for (auto type : allModules())
        {
            FXSlot slot;
            FXRetiredModules retired;
            slot.prepare(kRate, kBlock, maxLatency);
            slot.installNow(makeModule(type, 1));

            FX::SlotParams params;
            params.type = (int) type;
            params.values = defaults(type);
            params.mix = 1.0f;

            auto reference = makeModule(type, 1);
            const auto expected = run(*reference, input, params.values);
            const auto out = runSlot(slot, input, params, retired);
            expect(identical(expected.left, out.left) && identical(expected.right, out.right),
                   juce::String(FX::getInfo(type).name) + ": slot changed the module's output");
        }

        beginTest("(d) Mix blends the module against the dry signal");
        {
            FXSlot slot;
            FXRetiredModules retired;
            slot.prepare(kRate, kBlock, maxLatency);
            slot.installNow(std::make_unique<ScaleModule>(-1.0f));

            FX::SlotParams params;
            params.mix = 0.25f;
            const auto out = runSlot(slot, input, params, retired);

            float worst = 0.0f;
            for (size_t i = 0; i < input.size(); ++i)
                worst = std::max(worst, std::abs(out.left[i] - 0.5f * input.left[i])); // 0.75x - 0.25x
            expectLessThan(worst, 1.0e-6f);
        }

        beginTest("(e) A module offered while running crossfades in, and the old one is retired - never freed on the audio thread");
        {
            ScaleModule::destroyed = 0;
            FXSlot slot;
            FXRetiredModules retired;
            slot.prepare(kRate, kBlock, maxLatency);
            slot.installNow(std::make_unique<ScaleModule>(1.0f));

            const auto constant = mono(std::vector<float>((size_t) kBlock * 10, 0.5f));
            const auto out = runSlot(slot, constant, {}, retired, [&](int block, FX::SlotParams&)
            {
                if (block == 2)
                    expect(slot.offer(std::make_unique<ScaleModule>(2.0f)) == nullptr);
            });

            expectEquals(ScaleModule::destroyed, 0, "the outgoing module must wait for the message thread");
            retired.deleteAll();
            expectEquals(ScaleModule::destroyed, 1, "...which then frees it");

            expectWithinAbsoluteError(out.left[(size_t) kBlock * 2 - 1], 0.5f, 1.0e-6f);
            expectWithinAbsoluteError(out.left.back(), 1.0f, 1.0e-6f);

            // No jump bigger than a 10 ms fade's step (0.5 over ~441 samples), out or in.
            expectLessThan(maxStep(out.left), 0.01f);
        }

        beginTest("(f) A superseded offer that never played is handed back, not leaked");
        {
            ScaleModule::destroyed = 0;
            FXSlot slot;
            slot.prepare(kRate, kBlock, maxLatency);
            slot.installNow(std::make_unique<ScaleModule>(1.0f));

            expect(slot.offer(std::make_unique<ScaleModule>(2.0f)) == nullptr);
            auto superseded = slot.offer(std::make_unique<ScaleModule>(3.0f));
            expect(superseded != nullptr);
            superseded.reset();
            expectEquals(ScaleModule::destroyed, 1);
        }

        beginTest("(g) Switching bypass mid-signal ramps rather than steps, and re-enabling starts the module clean");
        {
            FXSlot slot;
            FXRetiredModules retired;
            slot.prepare(kRate, kBlock, maxLatency);
            auto owned = std::make_unique<ScaleModule>(0.0f);
            auto* module = owned.get();
            slot.installNow(std::move(owned));

            const auto constant = mono(std::vector<float>((size_t) kBlock * 12, 0.5f));
            const auto out = runSlot(slot, constant, {}, retired, [](int block, FX::SlotParams& p)
            {
                p.on = ! (block >= 3 && block < 7); // off for blocks 3-6, then back on
            });

            expectLessThan(maxStep(out.left), 0.01f);
            expectWithinAbsoluteError(out.left[(size_t) kBlock * 6], 0.5f, 1.0e-6f, "fully bypassed passes the input");
            expectWithinAbsoluteError(out.left.back(), 0.0f, 1.0e-6f, "re-enabled");
            expectEquals(module->resets, 1, "reset once on re-enabling after a full bypass");
        }

        beginTest("(h) Oversampling latency grows with the factor, and is zero when off");
        {
            const int l1 = FX::oversamplingLatency(1, kBlock);
            const int l2 = FX::oversamplingLatency(2, kBlock);
            const int l3 = FX::oversamplingLatency(3, kBlock);
            logMessage("Oversampling latency at 44.1 kHz: 2x " + juce::String(l1) + ", 4x " + juce::String(l2)
                       + ", 8x " + juce::String(l3) + " samples");
            expectEquals(FX::oversamplingLatency(0, kBlock), 0);
            expect(l1 > 0 && l2 >= l1 && l3 >= l2);
        }
    }
};

static Phase7SlotTest phase7SlotTest;

// ── Module responses (§6: "engaged = measured response matches spec") ──────

class Phase7ModuleSpecTest : public juce::UnitTest
{
public:
    Phase7ModuleSpecTest() : juce::UnitTest("Phase7ModuleSpec") {}

    void runTest() override
    {
        testFilter();
        testEQ();
        testBitCrusher();
        testClip();
        testTube();
        testWave();
        testTape();
        testDrive();
        testRingMod();
        testCompressor();
        testDualComp();
        testDelay();
        testReverb();
    }

private:
    void testFilter()
    {
        beginTest("Filter: low and high pass are -3 dB at the cutoff, both slopes; band pass 0 dB at its peak; notch nulls");

        constexpr auto type = FX::ModuleType::filter;
        for (int slope : { 0, 1 })
        {
            for (float cutoff : { 200.0f, 1000.0f, 5000.0f })
            {
                auto values = defaults(type);
                set(values, type, 1, cutoff);
                set(values, type, 2, 0.0f);
                set(values, type, 3, (float) slope);

                const juce::String where = juce::String(cutoff) + " Hz, " + (slope == 0 ? "12" : "24") + " dB";

                set(values, type, 0, 0.0f);
                expectWithinAbsoluteError(gainDb(type, values, cutoff), -3.01, 0.05, "LP at cutoff, " + where);
                expectWithinAbsoluteError(gainDb(type, values, cutoff / 8.0), 0.0, 0.05, "LP passband, " + where);
                expectLessThan(gainDb(type, values, cutoff * 4.0), slope == 0 ? -20.0 : -44.0, "LP stopband, " + where);

                set(values, type, 0, 1.0f);
                expectWithinAbsoluteError(gainDb(type, values, cutoff), -3.01, 0.05, "HP at cutoff, " + where);
                expectWithinAbsoluteError(gainDb(type, values, cutoff * 4.0), 0.0, 0.05, "HP passband, " + where);

                set(values, type, 0, 2.0f);
                expectWithinAbsoluteError(gainDb(type, values, cutoff), 0.0, 0.05, "BP peak, " + where);

                set(values, type, 0, 3.0f);
                expectLessThan(gainDb(type, values, cutoff), -40.0, "Notch, " + where);
            }
        }

        // Resonance peaks the response near the cutoff.
        auto resonant = defaults(type);
        set(resonant, type, 1, 1000.0f);
        set(resonant, type, 2, 80.0f);
        expectGreaterThan(gainDb(type, resonant, 1000.0), 10.0, "High resonance should peak");
    }

    void testEQ()
    {
        beginTest("EQ: the bell reaches its gain at its frequency; each shelf is half its gain at its corner");

        constexpr auto type = FX::ModuleType::eq;
        auto values = defaults(type);
        set(values, type, 2, 6.0f);     // mid +6 dB
        set(values, type, 3, 1000.0f);
        expectWithinAbsoluteError(gainDb(type, values, 1000.0), 6.0, 0.05, "Bell at its frequency");
        expectWithinAbsoluteError(gainDb(type, values, 50.0), 0.0, 0.1, "Bell far below");

        values = defaults(type);
        set(values, type, 0, 12.0f);    // low shelf +12 dB at 100 Hz
        set(values, type, 1, 100.0f);
        expectWithinAbsoluteError(gainDb(type, values, 100.0), 6.0, 0.1, "Low shelf at its corner");
        expectWithinAbsoluteError(gainDb(type, values, 20.0), 12.0, 0.3, "Low shelf below");

        values = defaults(type);
        set(values, type, 4, -12.0f);   // high shelf -12 dB at 8 kHz
        set(values, type, 5, 8000.0f);
        expectWithinAbsoluteError(gainDb(type, values, 8000.0), -6.0, 0.1, "High shelf at its corner");
        expectWithinAbsoluteError(gainDb(type, values, 200.0), 0.0, 0.1, "High shelf far below");

        // All bands flat: an exact identity.
        auto module = makeModule(type);
        const auto input = mono(kickLike(kBlock * 4));
        const auto out = run(*module, input, defaults(type));
        expect(identical(input.left, out.left), "A flat EQ should change nothing");
    }

    void testBitCrusher()
    {
        beginTest("Bit Crusher: every sample sits on the 2^-(bits-1) grid (§6's visible quantisation steps); Rate holds samples");

        constexpr auto type = FX::ModuleType::bitCrusher;
        for (int bits : { 1, 3, 8 })
        {
            auto values = defaults(type);
            set(values, type, 0, (float) bits);
            set(values, type, 1, 48000.0f);

            auto module = makeModule(type);
            const auto out = run(*module, mono(sine(110.0, 0.9, 8192)), values);

            const float levels = (float) (1 << (bits - 1));
            bool onGrid = true;
            std::vector<float> distinct;
            for (float v : out.left)
            {
                onGrid = onGrid && juce::exactlyEqual(v * levels, std::round(v * levels));
                if (std::find(distinct.begin(), distinct.end(), v) == distinct.end())
                    distinct.push_back(v);
            }

            expect(onGrid, juce::String(bits) + " bits: a sample is off the quantisation grid");
            // A 0.9 sine crosses every step between -0.9 and 0.9.
            const int expectedLevels = 2 * (int) std::round(0.9f * levels) + 1;
            expectEquals((int) distinct.size(), expectedLevels, juce::String(bits) + " bits: distinct levels");
        }

        auto values = defaults(type);
        set(values, type, 0, 16.0f);
        set(values, type, 1, 1000.0f);
        auto module = makeModule(type);
        const auto out = run(*module, mono(sine(50.0, 0.5, 8820)), values);

        int changes = 0;
        for (size_t i = 1; i < out.size(); ++i)
            changes += juce::exactlyEqual(out.left[i], out.left[i - 1]) ? 0 : 1;
        expectWithinAbsoluteError(changes, 199, 2, "1 kHz hold over 0.2 s should change ~200 times");
    }

    void testClip()
    {
        beginTest("Clip: hard clipping never exceeds the ceiling; below it, the gain is the drive");

        constexpr auto type = FX::ModuleType::clip;
        auto values = defaults(type);
        set(values, type, 0, 12.0f);    // drive
        set(values, type, 1, -6.0f);    // ceiling
        set(values, type, 2, 0.0f);     // hard
        set(values, type, 3, 0.0f);     // no bias
        set(values, type, 4, 0.0f);     // output 0 dB

        auto module = makeModule(type);
        const auto out = run(*module, mono(sine(100.0, 1.0, 8192)), values);
        expectLessOrEqual(maxAbs(out.left), FX::dbToGain(-6.0f) + 1.0e-6f, "Peak above the ceiling");
        expectGreaterThan(maxAbs(out.left), FX::dbToGain(-6.0f) - 1.0e-3f, "Should reach the ceiling");

        expectWithinAbsoluteError(gainDb(type, values, 100.0, 0.01), 12.0, 0.05, "Small-signal gain = drive");

        // Silence in, silence out - with bias, the static offset is removed.
        set(values, type, 3, 60.0f);
        auto biased = makeModule(type);
        const auto quiet = run(*biased, mono(std::vector<float>(2048, 0.0f)), values);
        expectEquals(maxAbs(quiet.left), 0.0f);
    }

    void testTube()
    {
        beginTest("Tube: no bias gives odd harmonics only; bias adds the second");

        constexpr auto type = FX::ModuleType::tube;
        auto harmonics = [&](float biasPercent)
        {
            auto values = defaults(type);
            set(values, type, 0, 18.0f);
            set(values, type, 1, biasPercent);
            set(values, type, 2, 20000.0f);
            auto module = makeModule(type);
            const int length = (int) kRate;
            const auto out = run(*module, mono(sine(200.0, 0.5, length)), values);
            const int w = length / 2;
            return std::pair { amplitudeAt(out.left, 400.0, w, w) / amplitudeAt(out.left, 200.0, w, w),
                               amplitudeAt(out.left, 600.0, w, w) / amplitudeAt(out.left, 200.0, w, w) };
        };

        const auto symmetric = harmonics(0.0f);
        expectLessThan(toDb(symmetric.first), -80.0, "No bias: H2");
        expectGreaterThan(toDb(symmetric.second), -30.0, "No bias: H3 present");

        const auto biased = harmonics(60.0f);
        expectGreaterThan(toDb(biased.first), -30.0, "Bias: H2 present");
    }

    void testWave()
    {
        beginTest("Wave: folding stays inside full scale at any drive, and keeps adding harmonics as it rises");

        constexpr auto type = FX::ModuleType::wave;
        double previousRichness = 0.0;
        for (float fold : { 0.0f, 12.0f, 24.0f })
        {
            for (float shape : { 0.0f, 100.0f })
            {
                auto values = defaults(type);
                set(values, type, 0, fold);
                set(values, type, 2, shape);
                set(values, type, 3, 20000.0f);
                set(values, type, 4, 0.0f);
                auto module = makeModule(type);
                const auto out = run(*module, mono(sine(100.0, 1.0, 8192)), values);
                expectLessOrEqual(maxAbs(out.left), 1.0f + 1.0e-5f, "Fold " + juce::String(fold) + " shape " + juce::String(shape));
            }

            auto values = defaults(type);
            set(values, type, 0, fold);
            set(values, type, 3, 20000.0f);
            auto module = makeModule(type);
            const int length = 16384;
            const auto out = run(*module, mono(sine(100.0, 1.0, length)), values);
            double upper = 0.0;
            for (int h = 3; h <= 15; h += 2)
                upper += amplitudeAt(out.left, 100.0 * h, length / 2, length / 2);
            expectGreaterThan(upper, previousRichness, "More fold, more harmonics");
            previousRichness = upper;
        }
    }

    void testTape()
    {
        beginTest("Tape: the head bump boosts 80 Hz by its setting; Tone is -3 dB at its frequency");

        constexpr auto type = FX::ModuleType::tape;
        auto values = defaults(type);
        set(values, type, 0, 0.0f);     // drive 0 dB: near-linear at a small level
        set(values, type, 1, 6.0f);     // bump
        set(values, type, 2, 20000.0f);
        expectWithinAbsoluteError(gainDb(type, values, 80.0, 0.001), 6.0, 0.05, "Bump at 80 Hz");

        set(values, type, 1, 0.0f);
        set(values, type, 2, 4000.0f);
        expectWithinAbsoluteError(gainDb(type, values, 4000.0, 0.001), -3.01, 0.05, "Tone at its frequency");
        expectWithinAbsoluteError(gainDb(type, values, 200.0, 0.001), 0.0, 0.05, "Tone passband");
    }

    void testDrive()
    {
        beginTest("Drive: below the crossover the signal passes clean; above it, it is driven");

        constexpr auto type = FX::ModuleType::drive;
        auto thd = [&](double hz)
        {
            auto values = defaults(type);
            set(values, type, 0, 30.0f);
            set(values, type, 1, 250.0f);
            set(values, type, 2, 20000.0f);
            auto module = makeModule(type, 1);
            const int length = (int) kRate;
            const auto out = run(*module, mono(sine(hz, 0.5, length)), values);
            const int w = length / 2;
            return toDb(amplitudeAt(out.left, 3.0 * hz, w, w) / amplitudeAt(out.left, hz, w, w));
        };

        expectLessThan(thd(40.0), -40.0, "40 Hz under a 250 Hz crossover should stay clean");
        expectGreaterThan(thd(2000.0), -20.0, "2 kHz above it should be driven");
    }

    void testRingMod()
    {
        beginTest("Ring Mod: a 1 kHz tone against a 300 Hz carrier becomes 700 Hz and 1.3 kHz, and the 1 kHz is gone");

        constexpr auto type = FX::ModuleType::ringMod;
        auto values = defaults(type);
        set(values, type, 0, 300.0f);
        set(values, type, 2, 0.0f);
        auto module = makeModule(type);
        const int length = (int) kRate;
        const auto out = run(*module, mono(sine(1000.0, 0.5, length)), values);
        const int w = length / 2;

        expectWithinAbsoluteError(amplitudeAt(out.left, 700.0, w, w), 0.25, 0.002);
        expectWithinAbsoluteError(amplitudeAt(out.left, 1300.0, w, w), 0.25, 0.002);
        expectLessThan(amplitudeAt(out.left, 1000.0, w, w), 1.0e-4);
    }

    // Holds a constant level so the compressor's static curve can be read exactly.
    static double settledDb(FX::ModuleType type, const FX::SlotValues& values, float level)
    {
        auto module = makeModule(type);
        const auto out = run(*module, mono(std::vector<float>((size_t) kRate, level)), values);
        return toDb(std::abs(out.left.back()));
    }

    void testCompressor()
    {
        beginTest("Compressor: the settled level follows the static curve; attack sets how fast it gets there");

        constexpr auto type = FX::ModuleType::compressor;
        auto values = defaults(type);
        set(values, type, 0, -18.0f);
        set(values, type, 1, 4.0f);
        set(values, type, 2, 1.0f);
        set(values, type, 3, 50.0f);
        set(values, type, 4, 0.0f);
        set(values, type, 5, 0.0f);

        const double in = toDb(0.5);
        expectWithinAbsoluteError(settledDb(type, values, 0.5f), -18.0 + (in + 18.0) / 4.0, 0.01, "Hard knee, 4:1");
        expectWithinAbsoluteError(settledDb(type, values, 0.05f), toDb(0.05), 0.01, "Below threshold: untouched");

        set(values, type, 5, 6.0f);
        expectWithinAbsoluteError(settledDb(type, values, 0.5f), -18.0 + (in + 18.0) / 4.0 + 6.0, 0.01, "Makeup");

        // Soft knee: at the threshold itself the reduction is (1/R - 1) * W / 8.
        set(values, type, 4, 12.0f);
        set(values, type, 5, 0.0f);
        const float atThreshold = FX::dbToGain(-18.0f);
        expectWithinAbsoluteError(settledDb(type, values, atThreshold), -18.0 + (0.25 - 1.0) * 12.0 / 8.0, 0.01, "Soft knee");

        // Attack: from silence-level to a loud step, 63% of the final reduction after one attack time.
        set(values, type, 2, 10.0f);
        set(values, type, 3, 500.0f);
        set(values, type, 4, 0.0f);
        auto module = makeModule(type);
        const auto out = run(*module, mono(std::vector<float>((size_t) kRate / 2, 0.5f)), values);
        const double finalReduction = toDb(out.left.back() / 0.5f);
        const size_t oneAttack = (size_t) (0.010 * kRate);
        const double reached = toDb(out.left[oneAttack] / 0.5f) / finalReduction;
        expectWithinAbsoluteError(reached, 1.0 - std::exp(-1.0), 0.02, "63% of the reduction after one attack time");
    }

    void testDualComp()
    {
        beginTest("Dual Comp: the settled level is the peak stage's curve fed into the glue stage's");

        constexpr auto type = FX::ModuleType::dualComp;
        auto values = defaults(type);
        set(values, type, 0, -12.0f);
        set(values, type, 1, 8.0f);
        set(values, type, 2, -24.0f);
        set(values, type, 3, 2.0f);
        set(values, type, 5, 0.0f);

        const double in = toDb(0.5);                       // -6.02, well above both 6 dB knees
        const double afterPeak = -12.0 + (in + 12.0) / 8.0;
        const double afterGlue = -24.0 + (afterPeak + 24.0) / 2.0;
        expectWithinAbsoluteError(settledDb(type, values, 0.5f), afterGlue, 0.01);
    }

    void testDelay()
    {
        beginTest("Delay: the echo lands exactly on time, repeats at the feedback level, syncs to tempo and ping-pongs");

        constexpr auto type = FX::ModuleType::delay;
        auto values = defaults(type);
        set(values, type, 0, 100.0f);    // 4410 samples
        set(values, type, 1, 50.0f);
        set(values, type, 2, 20000.0f);
        set(values, type, 3, 0.0f);
        set(values, type, 4, 0.0f);

        Stereo impulse(20000);
        impulse.left[0] = impulse.right[0] = 1.0f;

        auto module = makeModule(type);
        const auto out = run(*module, impulse, values);
        expectEquals(maxAbs(out.left, 0, 4410), 0.0f, "Nothing before the echo");
        expectWithinAbsoluteError(out.left[4410], 1.0f, 1.0e-6f, "First echo, full level, on its sample");

        double second = 0.0;
        for (size_t i = 8810; i < 8900; ++i)
            second += out.left[i];
        expectWithinAbsoluteError(second, 0.5, 0.01, "Second echo at the feedback level");

        // Tempo sync: 1/8 at 120 BPM is 0.25 s.
        set(values, type, 4, 1.0f);
        set(values, type, 5, 7.0f);
        FX::BlockContext context;
        context.bpm = 120.0;
        auto synced = makeModule(type);
        const auto syncedOut = run(*synced, impulse, values, context);
        expectWithinAbsoluteError(syncedOut.left[11025], 1.0f, 1.0e-6f, "1/8 at 120 BPM lands at 11025");
        expectEquals(maxAbs(syncedOut.left, 0, 11025), 0.0f);

        // Ping-pong: the first repeat on the left only, the second on the right.
        set(values, type, 3, 100.0f);
        set(values, type, 4, 0.0f);
        auto pingPong = makeModule(type);
        const auto pp = run(*pingPong, impulse, values);
        expectGreaterThan(std::abs(pp.left[4410]), 0.5f);
        expectEquals(maxAbs(pp.right, 0, 8800), 0.0f, "Right is silent until the second repeat");
        expectGreaterThan(maxAbs(pp.right, 8800, 8900), 0.2f, "Second repeat on the right");
    }

    // RT60 from an impulse response by Schroeder backward integration, fitted over -5 to -25 dB.
    static double measureRT60(const std::vector<float>& left, const std::vector<float>& right)
    {
        std::vector<double> energy(left.size());
        double total = 0.0;
        for (size_t i = left.size(); i-- > 0;)
        {
            total += (double) left[i] * left[i] + (double) right[i] * right[i];
            energy[i] = total;
        }

        size_t t5 = 0, t25 = 0;
        for (size_t i = 0; i < energy.size(); ++i)
        {
            const double db = 10.0 * std::log10(energy[i] / total);
            if (t5 == 0 && db <= -5.0)  t5 = i;
            if (t25 == 0 && db <= -25.0) { t25 = i; break; }
        }
        return 3.0 * (double) (t25 - t5) / kRate; // 20 dB of decay, times three
    }

    void testReverb()
    {
        beginTest("Reverb: the measured RT60 matches Decay; Width 0 is mono");

        constexpr auto type = FX::ModuleType::reverb;
        for (float decay : { 0.5f, 1.0f, 3.0f })
        {
            auto values = defaults(type);
            set(values, type, 0, decay);
            set(values, type, 1, 0.0f);
            set(values, type, 2, 20000.0f); // no damping
            set(values, type, 3, 100.0f);
            set(values, type, 5, 20.0f);

            Stereo impulse((size_t) (kRate * (decay * 2.0 + 0.5)));
            impulse.left[0] = impulse.right[0] = 1.0f;
            auto module = makeModule(type);
            const auto out = run(*module, impulse, values);

            const double rt60 = measureRT60(out.left, out.right);
            logMessage("Reverb decay " + juce::String(decay) + " s measured RT60 " + juce::String(rt60, 3) + " s");
            expectWithinAbsoluteError(rt60, (double) decay, decay * 0.03);
        }

        auto values = defaults(type);
        set(values, type, 4, 0.0f);
        auto module = makeModule(type);
        Stereo impulse(20000);
        impulse.left[0] = 1.0f;
        const auto out = run(*module, impulse, values);
        expect(identical(out.left, out.right), "Width 0 should be exactly mono");
        expectGreaterThan(maxAbs(out.left), 0.01f);
    }
};

static Phase7ModuleSpecTest phase7ModuleSpecTest;

// ── Oversampling (§6: "less aliasing with oversampling") ───────────────────

class Phase7OversamplingTest : public juce::UnitTest
{
public:
    Phase7OversamplingTest() : juce::UnitTest("Phase7Oversampling") {}

    // Power that lands off the harmonic series of `hz` - which, for a periodic input through a
    // memoryless shaper, can only be aliasing - against the total, in dB.
    static double aliasDb(const std::vector<float>& x, double hz)
    {
        // A 4-term Blackman-Harris window: sidelobes ~92 dB down, so leakage from the strong
        // harmonics cannot pass itself off as aliasing (a Hann window floors this at about -46 dB).
        constexpr int order = 15, size = 1 << order;
        juce::dsp::FFT fft(order);
        std::vector<float> data((size_t) size * 2, 0.0f);
        const size_t start = x.size() - (size_t) size;
        for (int i = 0; i < size; ++i)
        {
            const double t = juce::MathConstants<double>::twoPi * i / (size - 1);
            const double window = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2.0 * t) - 0.01168 * std::cos(3.0 * t);
            data[(size_t) i] = x[start + (size_t) i] * (float) window;
        }
        fft.performFrequencyOnlyForwardTransform(data.data());

        double harmonic = 0.0, alias = 0.0;
        for (int bin = 1; bin < size / 2; ++bin)
        {
            const double freq = bin * kRate / size;
            const double nearest = std::round(freq / hz) * hz;
            const double power = (double) data[(size_t) bin] * data[(size_t) bin];
            (std::abs(freq - nearest) < 6.0 * kRate / size ? harmonic : alias) += power;
        }
        return 10.0 * std::log10(alias / (harmonic + alias));
    }

    void runTest() override
    {
        beginTest("Each distortion aliases less oversampled than not (FFT)");

        for (auto type : { FX::ModuleType::clip, FX::ModuleType::tube, FX::ModuleType::wave,
                           FX::ModuleType::tape, FX::ModuleType::drive })
        {
            auto values = defaults(type);
            set(values, type, 0, FX::getInfo(type).params[0].maxValue * 0.8f); // heavy drive
            if (type == FX::ModuleType::clip)
                set(values, type, 2, 0.0f);                                    // hard clip
            if (type == FX::ModuleType::tube || type == FX::ModuleType::drive)
                set(values, type, 2, 20000.0f);                                // tone open

            const double hz = 3123.0; // not a divisor of the rate, so aliases miss the harmonics
            const auto input = mono(sine(hz, 0.7, 40000));

            auto measure = [&](int os)
            {
                auto module = makeModule(type, os);
                return aliasDb(run(*module, input, values).left, hz);
            };

            const double off = measure(0), twice = measure(1), four = measure(2);
            logMessage(juce::String(FX::getInfo(type).name) + " alias power: Off " + juce::String(off, 1)
                       + " dB, 2x " + juce::String(twice, 1) + " dB, 4x " + juce::String(four, 1) + " dB");
            expectLessThan(twice, off - 6.0, juce::String(FX::getInfo(type).name) + ": 2x vs Off");
            expectLessThan(four, twice + 0.5, juce::String(FX::getInfo(type).name) + ": 4x no worse than 2x");
        }
    }
};

static Phase7OversamplingTest phase7OversamplingTest;

// ── Smoothing (§6: "automate a param fast — no zipper noise") ─────────────

class Phase7SmoothingTest : public juce::UnitTest
{
public:
    Phase7SmoothingTest() : juce::UnitTest("Phase7Smoothing") {}

    // Steps one control between two values every block for a second, on a constant input, and
    // returns the biggest sample-to-sample change in the output. A smoothed control turns each step
    // into a ramp; an unsmoothed one would jump.
    float worstJump(FX::ModuleType type, int index, float low, float high, float level, FX::SlotValues values)
    {
        auto module = makeModule(type);
        const auto out = run(*module, mono(std::vector<float>((size_t) kRate, level)), values, {},
                             [&](int block, FX::SlotValues& v) { set(v, type, index, (block / 4) % 2 == 0 ? low : high); });
        return maxStep(out.left, (size_t) kBlock * 2);
    }

    void runTest() override
    {
        beginTest("Gains, EQ bands and filter cutoffs stepped every few blocks glide rather than jump");

        {
            auto values = defaults(FX::ModuleType::clip);
            set(values, FX::ModuleType::clip, 0, 0.0f);
            const float jump = worstJump(FX::ModuleType::clip, 4, -12.0f, 12.0f, 0.05f, values);
            // A 24 dB step on 0.05 is a jump of ~0.19; spread over 20 ms it is ~0.0002 a sample.
            expectLessThan(jump, 0.002f, "Clip output gain");
        }
        {
            auto values = defaults(FX::ModuleType::eq);
            set(values, FX::ModuleType::eq, 1, 500.0f);
            const float jump = worstJump(FX::ModuleType::eq, 0, -18.0f, 18.0f, 0.1f, values);
            expectLessThan(jump, 0.01f, "EQ low shelf gain");
        }
        {
            auto values = defaults(FX::ModuleType::compressor);
            const float jump = worstJump(FX::ModuleType::compressor, 5, 0.0f, 24.0f, 0.01f, values);
            expectLessThan(jump, 0.002f, "Compressor makeup");
        }
        {
            auto values = defaults(FX::ModuleType::filter);
            set(values, FX::ModuleType::filter, 0, 1.0f); // high pass: a DC input settles to zero
            const float jump = worstJump(FX::ModuleType::filter, 1, 100.0f, 10000.0f, 0.1f, values);
            expectLessThan(jump, 0.01f, "Filter cutoff");
        }
    }
};

static Phase7SmoothingTest phase7SmoothingTest;

// ── Engine: routing, alignment, macros ───────────────────────────────────────

namespace
{
    struct EngineRig
    {
        FXEngine engine;
        std::array<int, FX::kNumSlots> types {};
        FX::EngineParams params;
        int oversampling = 0;

        EngineRig() { engine.prepare(kRate, kBlock); install(); }

        void put(FX::Channel channel, int slot, FX::ModuleType type, const FX::SlotValues& values)
        {
            const int index = FX::slotIndex(channel, slot);
            types[(size_t) index] = (int) type;
            params.slots[(size_t) index].type = (int) type;
            params.slots[(size_t) index].values = values;
        }

        void install() { engine.setModuleTypes(types, oversampling, true); }

        // Feeds the three buses from the given mono signals (empty = silence) and returns the
        // stereo output.
        Stereo run(const std::vector<float>& dry, const std::vector<float>& a, const std::vector<float>& b)
        {
            const size_t length = std::max({ dry.size(), a.size(), b.size() });
            Stereo out(length);
            for (size_t start = 0; start < length; start += kBlock)
            {
                const int n = (int) std::min<size_t>(kBlock, length - start);
                engine.clearBuses(n);
                const std::vector<float>* sources[] { &dry, &a, &b };
                for (int bus = 0; bus < 3; ++bus)
                    for (int i = 0; i < n; ++i)
                        if (start + (size_t) i < sources[bus]->size())
                            engine.getBus((FX::Bus) bus)[i] = (*sources[bus])[start + (size_t) i];
                engine.process(out.left.data() + start, out.right.data() + start, n, params);
            }
            return out;
        }
    };
}

class Phase7EngineTest : public juce::UnitTest
{
public:
    Phase7EngineTest() : juce::UnitTest("Phase7Engine") {}

    void runTest() override
    {
        const auto x = kickLike(kBlock * 8);
        const auto y = sine(440.0, 0.3, kBlock * 8);
        const auto z = sine(97.0, 0.2, kBlock * 8);

        beginTest("(a) Empty FX: the output is exactly dry + A + B on both channels");
        {
            EngineRig rig;
            const auto out = rig.run(x, y, z);
            std::vector<float> expected(x.size());
            for (size_t i = 0; i < x.size(); ++i)
                expected[i] = x[i] + y[i] + z[i];
            expect(identical(expected, out.left) && identical(expected, out.right));
            expectEquals(rig.engine.getLatencySamples(), 0);

            // Dry alone is passed through untouched - the bit-identity the default patch relies on.
            const auto dryOnly = rig.run(x, {}, {});
            expect(identical(x, dryOnly.left) && identical(x, dryOnly.right));
        }

        beginTest("(b) A module in Insert A processes only Insert A's bus");
        {
            EngineRig rig;
            auto values = defaults(FX::ModuleType::ringMod);
            rig.put(FX::Channel::insertA, 2, FX::ModuleType::ringMod, values);
            rig.install();

            const auto out = rig.run(x, y, {});
            auto reference = makeModule(FX::ModuleType::ringMod);
            const auto processed = run(*reference, mono(y), values);

            float worst = 0.0f;
            for (size_t i = 0; i < x.size(); ++i)
                worst = std::max(worst, std::abs(out.left[i] - (x[i] + processed.left[i])));
            expectLessThan(worst, 1.0e-6f);
        }

        beginTest("(c) Routes are aligned: an oversampled module in Insert A delays Dry and Insert B to match, and the total is reported");
        {
            EngineRig rig;
            rig.oversampling = 2;
            auto values = defaults(FX::ModuleType::clip);
            set(values, FX::ModuleType::clip, 0, 0.0f);    // no drive...
            set(values, FX::ModuleType::clip, 2, 100.0f);  // ...soft: near-linear at a small level
            rig.put(FX::Channel::insertA, 0, FX::ModuleType::clip, values);
            rig.put(FX::Channel::insertA, 1, FX::ModuleType::clip, values);
            rig.install();

            const int one = rig.engine.getModuleLatency(2);
            expectEquals(rig.engine.getLatencySamples(), 2 * one, "Two oversampled modules in series");

            // The phase of a 500 Hz tone through each route. JUCE trims its filters' latency to a
            // whole sample with a fractional-delay all-pass, which is exact at low and mid
            // frequencies (where a kick lives) but smears an impulse's peak - so phase at a
            // frequency, not an impulse's peak, is the honest measure of alignment.
            const auto tone = sine(500.0, 0.01, kBlock * 16);
            auto phaseOf = [&](const std::vector<float>& v)
            {
                const int start = kBlock * 8, length = kBlock * 8;
                double re = 0.0, im = 0.0;
                for (int n = 0; n < length; ++n)
                {
                    const double w = juce::MathConstants<double>::twoPi * 500.0 * (start + n) / kRate;
                    re += v[(size_t) (start + n)] * std::cos(w);
                    im -= v[(size_t) (start + n)] * std::sin(w);
                }
                return std::atan2(im, re);
            };
            auto degreesApart = [](double a, double b) { return std::abs(std::remainder(a - b, juce::MathConstants<double>::twoPi)) * 180.0 / juce::MathConstants<double>::pi; };

            const double viaDry = phaseOf(rig.run(tone, {}, {}).left);
            const double viaA   = phaseOf(rig.run({}, tone, {}).left);
            const double viaB   = phaseOf(rig.run({}, {}, tone).left);
            const double input  = phaseOf(tone);

            // One sample at 500 Hz is 4.1 degrees.
            const double expectedShift = std::abs(std::remainder(2.0 * one * 360.0 * 500.0 / kRate, 360.0));
            expectWithinAbsoluteError(degreesApart(viaDry, input), expectedShift, 0.5, "Dry waits exactly for Insert A");
            expectLessThan(degreesApart(viaA, viaDry), 0.5, "Insert A arrives with Dry");
            expectLessThan(degreesApart(viaB, viaDry), 0.5, "Insert B arrives with Dry");

            // Master adds its own on top.
            rig.put(FX::Channel::master, 0, FX::ModuleType::clip, values);
            rig.install();
            expectEquals(rig.engine.getLatencySamples(), 3 * one);
        }

        beginTest("(d) Insert level scales its insert's output");
        {
            EngineRig rig;
            rig.params.insertLevel = { 0.5f, 1.0f };
            rig.run({}, y, {}); // let the level settle
            const auto out = rig.run({}, y, {});
            float worst = 0.0f;
            for (size_t i = 0; i < y.size(); ++i)
                worst = std::max(worst, std::abs(out.left[i] - 0.5f * y[i]));
            expectLessThan(worst, 1.0e-6f);
        }

        beginTest("(e) Master processes the sum of all three routes");
        {
            EngineRig rig;
            auto values = defaults(FX::ModuleType::filter);
            set(values, FX::ModuleType::filter, 1, 300.0f);
            rig.put(FX::Channel::master, 3, FX::ModuleType::filter, values);
            rig.install();

            const auto out = rig.run(x, y, z);
            std::vector<float> sum(x.size());
            for (size_t i = 0; i < x.size(); ++i)
                sum[i] = x[i] + y[i] + z[i];
            auto reference = makeModule(FX::ModuleType::filter);
            const auto expected = run(*reference, mono(sum), values);
            expect(identical(expected.left, out.left));
        }

        beginTest("(f) Macros move their targets by depth x macro, summed and then clamped, without touching the base");
        {
            EngineRig rig;
            FX::MacroSnapshot snapshot;
            snapshot.assignments.push_back({ 0, 4, 1, 0.3f });             // macro 1 -> slot 5 p2, +30%
            snapshot.assignments.push_back({ 1, 4, 1, 0.5f });             // macro 2 -> same, +50%
            snapshot.assignments.push_back({ 2, 7, FX::kMixTarget, -1.0f }); // macro 3 -> slot 8 mix, -100%
            rig.params.macroSnapshot = &snapshot;
            rig.params.slots[4].values[1] = 0.4f;
            rig.params.slots[7].mix = 0.8f;

            rig.params.macros = { 1.0f, 0.0f, 0.25f, 0.0f };
            rig.run(x, {}, {});
            expectWithinAbsoluteError(rig.engine.getEffectiveValue(4, 1), 0.7f, 1.0e-6f);
            expectWithinAbsoluteError(rig.engine.getEffectiveValue(7, FX::kMixTarget), 0.55f, 1.0e-6f);
            expectWithinAbsoluteError(rig.params.slots[4].values[1], 0.4f, 0.0f, "The base value is untouched");

            rig.params.macros = { 1.0f, 1.0f, 1.0f, 0.0f };
            rig.run(x, {}, {});
            expectWithinAbsoluteError(rig.engine.getEffectiveValue(4, 1), 1.0f, 0.0f, "Clamped at the top");
            expectWithinAbsoluteError(rig.engine.getEffectiveValue(7, FX::kMixTarget), 0.0f, 0.0f, "Clamped at the bottom");

            // An assignment pointing nowhere is ignored rather than trusted.
            snapshot.assignments.push_back({ 0, 99, 1, 1.0f });
            snapshot.assignments.push_back({ 9, 1, 1, 1.0f });
            rig.run(x, {}, {});
            expectWithinAbsoluteError(rig.engine.getEffectiveValue(4, 1), 1.0f, 0.0f);
        }

        beginTest("(g) A type change while running is offered, swapped in on the next block, and the old module freed by the message thread");
        {
            EngineRig rig;
            rig.put(FX::Channel::insertB, 0, FX::ModuleType::delay, defaults(FX::ModuleType::delay));
            rig.install();
            expect(rig.engine.getActiveType(6) == FX::ModuleType::delay);

            rig.types[6] = (int) FX::ModuleType::reverb;
            rig.engine.setModuleTypes(rig.types, 0, false);
            expect(rig.engine.getActiveType(6) == FX::ModuleType::delay, "Not until the audio thread takes it");

            rig.run(x, {}, {});
            expect(rig.engine.getActiveType(6) == FX::ModuleType::reverb);
            rig.engine.setModuleTypes(rig.types, 0, false); // drains the retired delay

            // Changing only the oversampling rebuilds the nonlinear modules and leaves the rest.
            rig.types[0] = (int) FX::ModuleType::tube;
            rig.install();
            rig.engine.setModuleTypes(rig.types, 2, true);
            expect(rig.engine.getLatencySamples() == rig.engine.getModuleLatency(2));
        }

        beginTest("(h) Tails: reverb decay plus pre-delay, delay repeats to -60 dB (capped), nothing for the rest");
        {
            auto reverb = defaults(FX::ModuleType::reverb);
            set(reverb, FX::ModuleType::reverb, 0, 2.0f);
            set(reverb, FX::ModuleType::reverb, 1, 50.0f);
            expectWithinAbsoluteError(FX::tailSeconds(FX::ModuleType::reverb, reverb, 120.0), 2.05, 1.0e-4);

            auto delay = defaults(FX::ModuleType::delay);
            set(delay, FX::ModuleType::delay, 0, 100.0f);
            set(delay, FX::ModuleType::delay, 1, 50.0f);
            expectWithinAbsoluteError(FX::tailSeconds(FX::ModuleType::delay, delay, 120.0),
                                      0.1 * (1.0 + std::log(0.001) / std::log(0.5)), 1.0e-3);

            expectEquals(FX::tailSeconds(FX::ModuleType::clip, defaults(FX::ModuleType::clip), 120.0), 0.0);
        }
    }
};

static Phase7EngineTest phase7EngineTest;

// ── Macro assignments ────────────────────────────────────────────────────────

class Phase7MacroAssignmentsTest : public juce::UnitTest
{
public:
    Phase7MacroAssignmentsTest() : juce::UnitTest("Phase7MacroAssignments") {}

    void runTest() override
    {
        beginTest("(a) Assign, update, remove; depth clamps; each macro is capped");
        {
            MacroAssignments m;
            m.assign(0, 3, 1, 0.5f);
            m.assign(0, 3, 1, 2.0f); // update, clamped
            m.assign(1, 3, FX::kMixTarget, -0.25f);
            expect(m.isAssigned(0, 3, 1));
            expectEquals(m.getDepth(0, 3, 1), 1.0f);
            expectEquals((int) m.getAssignments(0).size(), 1);
            expectEquals((int) m.getSnapshot()->assignments.size(), 2);

            m.remove(0, 3, 1);
            expect(! m.isAssigned(0, 3, 1));
            expectEquals((int) m.getSnapshot()->assignments.size(), 1);

            m.assign(5, 3, 1, 0.5f);   // no macro 6
            m.assign(0, 18, 1, 0.5f);  // no slot 19
            m.assign(0, 3, 7, 0.5f);   // no target 8
            expectEquals((int) m.getSnapshot()->assignments.size(), 1, "Invalid assignments are refused");

            for (int i = 0; i < 40; ++i)
                m.assign(2, i % FX::kNumSlots, i / FX::kNumSlots, 0.1f);
            expectEquals((int) m.getAssignments(2).size(), FX::kMaxAssignmentsPerMacro);
        }

        beginTest("(b) Swapping slots moves their assignments; clearing a slot drops its own");
        {
            MacroAssignments m;
            m.assign(0, 2, 0, 0.5f);
            m.assign(1, 9, 3, -0.5f);
            m.assign(2, 4, 1, 0.2f);
            m.swapSlots(2, 9);
            expect(m.isAssigned(0, 9, 0) && m.isAssigned(1, 2, 3) && m.isAssigned(2, 4, 1));
            expect(! m.isAssigned(0, 2, 0));

            m.clearSlot(9);
            expect(! m.isAssigned(0, 9, 0) && m.isAssigned(1, 2, 3));
        }

        beginTest("(c) State round-trips, and a damaged state is cleaned rather than trusted");
        {
            MacroAssignments m;
            m.assign(0, 1, 2, 0.75f);
            m.assign(3, 17, FX::kMixTarget, -0.5f);

            MacroAssignments copy;
            copy.setState(m.getState());
            expectEquals(copy.getDepth(0, 1, 2), 0.75f);
            expectEquals(copy.getDepth(3, 17, FX::kMixTarget), -0.5f);

            auto damaged = m.getState();
            auto add = [&](const juce::var& macro, const juce::var& slot, const juce::var& target, const juce::var& depth)
            {
                juce::ValueTree a { "ASSIGN" };
                a.setProperty("macro", macro, nullptr);
                a.setProperty("slot", slot, nullptr);
                a.setProperty("target", target, nullptr);
                a.setProperty("depth", depth, nullptr);
                damaged.appendChild(a, nullptr);
            };
            add(0, 1, 2, 0.1);        // duplicate: merges into the existing one
            add(7, 1, 2, 0.5);        // bad macro
            add(0, -1, 2, 0.5);       // bad slot
            add(0, 1, 2, std::numeric_limits<double>::quiet_NaN());
            add(1, 5, 0, 9.0);        // depth clamps
            damaged.appendChild(juce::ValueTree { "SOMETHING_ELSE" }, nullptr);

            MacroAssignments cleaned;
            cleaned.setState(damaged);
            expectEquals((int) cleaned.getSnapshot()->assignments.size(), 3);
            expectEquals(cleaned.getDepth(0, 1, 2), 0.1f);
            expectEquals(cleaned.getDepth(1, 5, 0), 1.0f);

            cleaned.setState({});
            expectEquals((int) cleaned.getSnapshot()->assignments.size(), 0, "No MACROS (an older preset) means none");
        }
    }
};

static Phase7MacroAssignmentsTest phase7MacroAssignmentsTest;
