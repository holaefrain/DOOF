#include "FXModuleInfo.h"
#include <cmath>

namespace FX
{
namespace
{
    const char* const filterModes[] { "Low Pass", "High Pass", "Band Pass", "Notch" };
    const char* const filterSlopes[] { "12 dB", "24 dB" };
    const char* const offOn[] { "Off", "On" };

    // Ascending, so sweeping the control lengthens the delay monotonically.
    const char* const divisionNames[] { "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8",
                                        "1/4T", "1/8D", "1/4", "1/2T", "1/4D", "1/2", "1/1" };
    const double divisionValues[] { 1.0 / 16.0, 1.0 / 12.0, 1.0 / 8.0, 1.0 / 6.0, 1.0 / 4.0, 1.0 / 3.0, 3.0 / 8.0, 1.0 / 2.0,
                                    2.0 / 3.0, 3.0 / 4.0, 1.0, 4.0 / 3.0, 3.0 / 2.0, 2.0, 4.0 };
    static_assert(std::size(divisionNames) == (size_t) kNumDivisions && std::size(divisionValues) == (size_t) kNumDivisions,
                  "Delay division names and values have drifted apart");

    // Shorthand for the table below.
    ParamSpec decibels(const char* name, float lo, float hi, float def)    { return { name, Unit::decibels, Scale::linear, lo, hi, def }; }
    ParamSpec hertz(const char* name, float lo, float hi, float def)       { return { name, Unit::hertz, Scale::logarithmic, lo, hi, def }; }
    ParamSpec millis(const char* name, float lo, float hi, float def)      { return { name, Unit::milliseconds, Scale::logarithmic, lo, hi, def }; }
    ParamSpec percent(const char* name, float lo, float hi, float def)     { return { name, Unit::percent, Scale::linear, lo, hi, def }; }
    ParamSpec ratio(const char* name, float def)                           { return { name, Unit::ratio, Scale::power, 1.0f, 20.0f, def, 2.0f }; }
    ParamSpec output()                                                     { return decibels("Output", -24.0f, 12.0f, 0.0f); }
    ParamSpec unused()                                                     { return {}; }

    template <size_t N>
    ParamSpec choice(const char* name, const char* const (&names)[N], int def)
    {
        ParamSpec spec { name, Unit::choice, Scale::linear, 0.0f, (float) (N - 1), (float) def };
        spec.choices = names;
        spec.numChoices = (int) N;
        return spec;
    }

    // Index-aligned with ModuleType. Every module's controls are documented in its DSP file; this
    // is only their names, units, ranges and defaults.
    const std::array<ModuleInfo, kNumModuleTypes>& table()
    {
        static const std::array<ModuleInfo, kNumModuleTypes> modules {{
            { "None", { unused(), unused(), unused(), unused(), unused(), unused() }, 1.0f, false },

            { "Clip", { decibels("Drive", 0.0f, 36.0f, 9.0f), decibels("Ceiling", -24.0f, 0.0f, 0.0f),
                        percent("Softness", 0.0f, 100.0f, 30.0f), percent("Bias", -100.0f, 100.0f, 0.0f),
                        output(), unused() }, 1.0f, true },

            { "Tube", { decibels("Drive", 0.0f, 30.0f, 12.0f), percent("Bias", 0.0f, 100.0f, 30.0f),
                        hertz("Tone", 1000.0f, 20000.0f, 12000.0f), output(), unused(), unused() }, 1.0f, true },

            { "Wave", { decibels("Fold", 0.0f, 24.0f, 6.0f), percent("Symmetry", -100.0f, 100.0f, 0.0f),
                        percent("Shape", 0.0f, 100.0f, 0.0f), hertz("Tone", 1000.0f, 20000.0f, 20000.0f),
                        output(), unused() }, 1.0f, true },

            { "Tape", { decibels("Drive", 0.0f, 24.0f, 6.0f), decibels("Bump", 0.0f, 6.0f, 2.0f),
                        hertz("Tone", 2000.0f, 20000.0f, 12000.0f), output(), unused(), unused() }, 1.0f, true },

            { "Drive", { decibels("Drive", 0.0f, 36.0f, 12.0f), hertz("Crossover", 20.0f, 500.0f, 20.0f),
                         hertz("Tone", 1000.0f, 20000.0f, 20000.0f), output(), unused(), unused() }, 1.0f, true },

            { "Bit Crusher", { ParamSpec { "Bits", Unit::bits, Scale::linear, 1.0f, 16.0f, 8.0f },
                               hertz("Rate", 500.0f, 48000.0f, 48000.0f), output(), unused(), unused(), unused() }, 1.0f, false },

            { "Ring Mod", { hertz("Frequency", 20.0f, 5000.0f, 220.0f), percent("Spread", 0.0f, 100.0f, 0.0f),
                            percent("Shape", 0.0f, 100.0f, 0.0f), output(), unused(), unused() }, 1.0f, true },

            { "Filter", { choice("Mode", filterModes, 0), hertz("Cutoff", 20.0f, 20000.0f, 2000.0f),
                          percent("Resonance", 0.0f, 100.0f, 0.0f), choice("Slope", filterSlopes, 0),
                          unused(), unused() }, 1.0f, false },

            { "EQ", { decibels("Low Gain", -18.0f, 18.0f, 0.0f), hertz("Low Freq", 20.0f, 500.0f, 100.0f),
                      decibels("Mid Gain", -18.0f, 18.0f, 0.0f), hertz("Mid Freq", 100.0f, 10000.0f, 1000.0f),
                      decibels("High Gain", -18.0f, 18.0f, 0.0f), hertz("High Freq", 1000.0f, 20000.0f, 8000.0f) }, 1.0f, false },

            { "Compressor", { decibels("Threshold", -60.0f, 0.0f, -18.0f), ratio("Ratio", 4.0f),
                              millis("Attack", 0.1f, 100.0f, 10.0f), millis("Release", 5.0f, 2000.0f, 120.0f),
                              decibels("Knee", 0.0f, 24.0f, 6.0f), decibels("Makeup", 0.0f, 24.0f, 0.0f) }, 1.0f, false },

            { "Dual Comp", { decibels("Peak Thresh", -60.0f, 0.0f, -12.0f), ratio("Peak Ratio", 8.0f),
                             decibels("Glue Thresh", -60.0f, 0.0f, -24.0f), ratio("Glue Ratio", 2.0f),
                             percent("Speed", 0.0f, 100.0f, 50.0f), decibels("Makeup", 0.0f, 24.0f, 0.0f) }, 1.0f, false },

            { "Delay", { millis("Time", 1.0f, 2000.0f, 250.0f), percent("Feedback", 0.0f, 95.0f, 30.0f),
                         hertz("Tone", 500.0f, 20000.0f, 6000.0f), percent("Ping-Pong", 0.0f, 100.0f, 0.0f),
                         choice("Sync", offOn, 0), choice("Division", divisionNames, 7) }, 0.3f, false },

            { "Reverb", { ParamSpec { "Decay", Unit::seconds, Scale::logarithmic, 0.1f, 10.0f, 1.2f },
                          ParamSpec { "Pre-Delay", Unit::milliseconds, Scale::linear, 0.0f, 200.0f, 10.0f },
                          hertz("Damping", 1000.0f, 20000.0f, 6000.0f), percent("Size", 0.0f, 100.0f, 60.0f),
                          percent("Width", 0.0f, 100.0f, 100.0f), hertz("Low Cut", 20.0f, 1000.0f, 120.0f) }, 0.25f, false },
        }};

        return modules;
    }

    // Two decimals below 10, one below 100, none above - "2.50 ms", "25.0 ms", "250 ms".
    juce::String threeFigures(double value)
    {
        const double magnitude = std::abs(value);
        return juce::String(value, magnitude < 10.0 ? 2 : (magnitude < 100.0 ? 1 : 0));
    }
}

juce::String channelName(Channel channel)
{
    switch (channel)
    {
        case Channel::insertA: return "Insert A";
        case Channel::insertB: return "Insert B";
        case Channel::master:  return "Master";
    }
    return {};
}

juce::StringArray moduleTypeChoices()
{
    juce::StringArray names;
    for (const auto& module : table())
        names.add(module.name);
    return names;
}

const ModuleInfo& getInfo(ModuleType type)
{
    const int index = juce::jlimit(0, kNumModuleTypes - 1, (int) type);
    return table()[(size_t) index];
}

float toValue(const ParamSpec& spec, float normalised)
{
    const float n = juce::jlimit(0.0f, 1.0f, normalised);
    float value = spec.minValue;

    switch (spec.scale)
    {
        case Scale::linear:      value = spec.minValue + n * (spec.maxValue - spec.minValue); break;
        case Scale::logarithmic: value = spec.minValue * std::pow(spec.maxValue / spec.minValue, n); break;
        case Scale::power:       value = spec.minValue + (spec.maxValue - spec.minValue) * std::pow(n, spec.exponent); break;
    }

    return spec.isDiscrete() ? std::round(value) : value;
}

float toNormalised(const ParamSpec& spec, float value)
{
    const float v = juce::jlimit(spec.minValue, spec.maxValue, value);

    switch (spec.scale)
    {
        case Scale::linear:
            return spec.maxValue > spec.minValue ? (v - spec.minValue) / (spec.maxValue - spec.minValue) : 0.0f;
        case Scale::logarithmic:
            return std::log(v / spec.minValue) / std::log(spec.maxValue / spec.minValue);
        case Scale::power:
            return std::pow((v - spec.minValue) / (spec.maxValue - spec.minValue), 1.0f / spec.exponent);
    }

    return 0.0f;
}

float defaultNormalised(const ParamSpec& spec)
{
    return spec.isUsed() ? toNormalised(spec, spec.defaultValue) : 0.5f;
}

juce::String toText(const ParamSpec& spec, float normalised)
{
    if (! spec.isUsed())
        return {};

    const double value = toValue(spec, normalised);

    switch (spec.unit)
    {
        case Unit::decibels:
            return (value > 0.05 ? "+" : "") + juce::String(value, 1) + " dB";

        case Unit::hertz:
            return value >= 1000.0 ? threeFigures(value / 1000.0) + " kHz" : threeFigures(value) + " Hz";

        case Unit::milliseconds:
            return value >= 1000.0 ? juce::String(value / 1000.0, 2) + " s" : threeFigures(value) + " ms";

        case Unit::seconds:
            return value < 1.0 ? juce::String(juce::roundToInt(value * 1000.0)) + " ms" : juce::String(value, 2) + " s";

        case Unit::percent:
            return (spec.minValue < 0.0f && value > 0.5 ? "+" : "") + juce::String(juce::roundToInt(value)) + "%";

        case Unit::ratio:
            return juce::String(value, 1) + ":1";

        case Unit::bits:
            return juce::String(juce::roundToInt(value)) + " bit";

        case Unit::choice:
        {
            const int index = juce::jlimit(0, spec.numChoices - 1, juce::roundToInt(value));
            return spec.choices[index];
        }

        case Unit::none:
            break;
    }

    return juce::String(value, 2);
}

float fromText(const ParamSpec& spec, const juce::String& typed)
{
    if (! spec.isUsed())
        return 0.5f;

    const auto text = typed.trim().toLowerCase();

    if (spec.unit == Unit::choice)
    {
        for (int i = 0; i < spec.numChoices; ++i)
            if (text == juce::String(spec.choices[i]).toLowerCase())
                return toNormalised(spec, (float) i);

        // A bare number picks by position, 1-based as a person would count them.
        if (text.containsOnly("0123456789") && text.isNotEmpty())
            return toNormalised(spec, (float) (text.getIntValue() - 1));

        return defaultNormalised(spec);
    }

    double value = text.getDoubleValue();

    switch (spec.unit)
    {
        case Unit::hertz:
            if (text.endsWith("k") || text.endsWith("khz"))
                value *= 1000.0;
            break;

        case Unit::milliseconds:
            if (text.endsWith("s") && ! text.endsWith("ms"))
                value *= 1000.0;
            break;

        case Unit::seconds:
            if (text.endsWith("ms"))
                value /= 1000.0;
            break;

        case Unit::decibels: case Unit::percent: case Unit::ratio: case Unit::bits:
        case Unit::choice: case Unit::none:
            break;
    }

    return toNormalised(spec, (float) value);
}

double divisionBeats(int index)
{
    return divisionValues[(size_t) juce::jlimit(0, kNumDivisions - 1, index)];
}
}
