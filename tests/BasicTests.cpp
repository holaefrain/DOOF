#include <juce_core/juce_core.h>

// Phase 0 sanity test — verifies the test runner itself is wired up correctly.
// Real audio/DSP tests are added per-phase as the engine grows.
class SanityTest : public juce::UnitTest
{
public:
    SanityTest() : juce::UnitTest("Sanity") {}

    void runTest() override
    {
        beginTest("1 + 1 == 2");
        expectEquals(1 + 1, 2);

        beginTest("juce::String round-trip");
        juce::String s("DOOF");
        expectEquals(s, juce::String("DOOF"));
    }
};

static SanityTest sanityTest;

int main()
{
    juce::UnitTestRunner runner;
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult(i)->failures;

    return failures > 0 ? 1 : 0;
}
