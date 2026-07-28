cmake_minimum_required(VERSION 3.22)

# Root build script for DOOF — configures the plugin target, unit-test target,
# and links JUCE as a subdirectory dependency.
project(DOOF VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in JUCE from the local submodule so the build is fully self-contained.
add_subdirectory(JUCE)

# ─── Plugin target ────────────────────────────────────────────────────────────
# Main plugin artefact: builds VST3 and Standalone formats.
# IS_SYNTH + NEEDS_MIDI_INPUT configure JUCE's bus/MIDI plumbing for a synth.
juce_add_plugin(DOOF
    VERSION                     "0.1.0"
    COMPANY_NAME                "EfrainCuessy"
    BUNDLE_ID                   "com.efraincuessy.doof"
    PLUGIN_MANUFACTURER_CODE    ECsy        # 4-char manufacturer code registered with Steinberg
    PLUGIN_CODE                 Doof        # 4-char plugin code, unique per product
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "DOOF"
    IS_SYNTH                    TRUE        # declares main bus as a synth (no audio input required)
    NEEDS_MIDI_INPUT            TRUE        # plugin receives MIDI notes to trigger the engine
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Instrument|Synth"
)

# Source files compiled into the plugin target.
target_sources(DOOF
    PRIVATE
        src/PluginProcessor.cpp   # audio engine + JUCE AudioProcessor boilerplate
        src/PluginEditor.cpp      # GUI root + JUCE AudioProcessorEditor boilerplate
        src/SubVoice.cpp          # mono sine voice: oscillator, envelopes, choke state machine
        src/EnvelopeModel.cpp     # node-based envelope data model (ValueTree + undo)
)

# Compile-time definitions applied to the plugin target.
# WEB_BROWSER and CURL disabled to reduce binary size and avoid unnecessary dependencies.
# VST3_CAN_REPLACE_VST2 disabled because DOOF has no VST2 heritage.
target_compile_definitions(DOOF
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

# Link JUCE modules needed by the plugin.
# juce_audio_utils provides AudioProcessor, AudioProcessorEditor, and related helpers.
# juce_dsp provides the DSP toolkit used by the engine (oscillators, filters, FFT, etc.).
target_link_libraries(DOOF
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_data_structures  # ValueTree + UndoManager, used by EnvelopeModel
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ─── Unit tests ───────────────────────────────────────────────────────────────
# Headless console app that runs all juce::UnitTest subclasses via CTest.
# Each phase adds its own test cases; all test files are listed here.
juce_add_console_app(DOOFTests PRODUCT_NAME "DOOFTests")

# Test source files.
# SubVoice.cpp and EnvelopeModel.cpp are included so tests can instantiate them
# directly, without pulling in the full plugin target and its audio_utils/dsp deps.
target_sources(DOOFTests
    PRIVATE
        tests/BasicTests.cpp   # Phase 0 sanity test + Phase 1 SubVoice verify tests
        src/SubVoice.cpp       # voice engine under test (juce_core only, no audio_utils)
        src/EnvelopeModel.cpp  # envelope data model under test (juce_data_structures)
)

# Expose the src/ directory so tests can #include "SubVoice.h" without path prefixes.
target_include_directories(DOOFTests PRIVATE src)

target_compile_definitions(DOOFTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# SubVoice uses juce::MathConstants and juce::ignoreUnused, both from juce_core.
# EnvelopeModel uses juce::ValueTree/UndoManager, from juce_data_structures.
# Add juce_audio_basics or juce_dsp here when later test phases need them.
target_link_libraries(DOOFTests
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Register the test binary with CTest so `ctest` discovers and runs it automatically.
enable_testing()
add_test(NAME BasicTests COMMAND DOOFTests)
