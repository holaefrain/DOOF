cmake_minimum_required(VERSION 3.22)

# Root build script for DOOF — configures the plugin target, unit-test target,
# and links JUCE as a subdirectory dependency.
project(DOOF VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Pull in JUCE from the local submodule so the build is fully self-contained.
add_subdirectory(JUCE)

# ─── Factory click samples (Phase 5) ──────────────────────────────────────────
# Embeds any .wav in resources/clicks/ into the binary, filling the four sample slots of the
# layerN.click.type parameter.
#
# An EMPTY resources/clicks/ is the normal state of this repo and must always build and pass.
# §8 lists factory content licensing as a project risk, so no audio is committed — the pipeline
# ships complete and the slots stay empty until license-clear files are supplied. A slot with
# nothing behind it is silent, which Phase5ClickRoutingTest (c) pins.
#
# CONFIGURE_DEPENDS so that dropping a .wav into the folder re-runs CMake on the next build
# instead of being silently ignored until someone reconfigures by hand.
file(GLOB DOOF_CLICK_SAMPLE_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/resources/clicks/*.wav")

if(DOOF_CLICK_SAMPLE_FILES)
    list(LENGTH DOOF_CLICK_SAMPLE_FILES DOOF_CLICK_SAMPLE_COUNT)
    message(STATUS "DOOF: embedding ${DOOF_CLICK_SAMPLE_COUNT} factory click sample(s)")

    # Sorted so slot order is alphabetical by filename and identical on every machine. The glob's
    # own order is filesystem-dependent, which would make a preset's slot mean different things on
    # different build hosts.
    list(SORT DOOF_CLICK_SAMPLE_FILES)

    juce_add_binary_data(DOOFClickSamples SOURCES ${DOOF_CLICK_SAMPLE_FILES})
    set(DOOF_HAS_CLICK_SAMPLES 1)
else()
    message(STATUS "DOOF: no factory click samples found - the four sample slots will be silent")
    set(DOOF_HAS_CLICK_SAMPLES 0)
endif()

# Applies the define, and the binary-data target when there is one, to a target that needs to read
# click samples. Used by the plugin and the integration tests; deliberately NOT by DOOFTests, which
# stays lean (juce_core only) and never decodes audio.
function(doof_link_click_samples target)
    target_compile_definitions(${target} PRIVATE DOOF_HAS_CLICK_SAMPLES=${DOOF_HAS_CLICK_SAMPLES})

    if(DOOF_HAS_CLICK_SAMPLES)
        target_link_libraries(${target} PRIVATE DOOFClickSamples)
    endif()
endfunction()

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
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE    # needed for Cmd+Z/Cmd+Shift+Z undo/redo (Phase 3 Step 3a)
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Instrument|Synth"
)

# Source files compiled into the plugin target.
target_sources(DOOF
    PRIVATE
        src/PluginProcessor.cpp   # audio engine + JUCE AudioProcessor boilerplate
        src/PluginEditor.cpp      # GUI root + JUCE AudioProcessorEditor boilerplate
        src/EnvelopeCanvas.cpp    # central canvas: pitch+amp curve rendering (§3.4)
        src/SubVoice.cpp          # mono sine voice: oscillator, envelopes, choke state machine
        src/ClickVoice.cpp        # synthesised click transient: tick/noise/snap/thump + tone filter
        src/ClickSampleLibrary.cpp # decodes the embedded factory click samples (message thread only)
        src/EnvelopeModel.cpp     # node-based envelope data model (ValueTree + undo)
        src/BezierSegment.cpp     # single-segment cubic Bezier math (time -> value)
        src/EnvelopeEvaluator.cpp # precomputes an EnvelopeModel into a lookup table
        src/EnvelopePublisher.cpp # atomic-pointer snapshot publish path to the audio thread
        src/DefaultEnvelopes.cpp  # out-of-the-box pitch/amp envelope shapes
        src/LayerAudibility.cpp   # §3.3 mute/solo audibility rule, shared by engine + GUI
        src/LayerStrip.cpp        # one cell of the horizontal layer selector (§4.3)
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
        juce::juce_events           # juce::Timer, used by EnvelopePublisher
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Factory click samples: embedded when resources/clicks/ has any .wav, silent slots when it doesn't.
doof_link_click_samples(DOOF)

# ─── Unit tests ───────────────────────────────────────────────────────────────
# Headless console app that runs all juce::UnitTest subclasses via CTest.
# Each phase adds its own test cases; all test files are listed here.
juce_add_console_app(DOOFTests PRODUCT_NAME "DOOFTests")

# Test source files.
# SubVoice.cpp and EnvelopeModel.cpp are included so tests can instantiate them
# directly, without pulling in the full plugin target and its audio_utils/dsp deps.
target_sources(DOOFTests
    PRIVATE
        tests/BasicTests.cpp   # Phase 0/1/2 verify tests
        src/SubVoice.cpp       # voice engine under test (juce_core only, no audio_utils)
        src/ClickVoice.cpp     # click transient under test (juce_core only, same lean constraint)
        src/EnvelopeModel.cpp  # envelope data model under test (juce_data_structures)
        src/BezierSegment.cpp  # Bezier segment math under test (no JUCE dependency)
        src/EnvelopeEvaluator.cpp # lookup-table builder under test
        src/EnvelopePublisher.cpp # atomic-pointer publish path under test
        src/DefaultEnvelopes.cpp # out-of-the-box envelope shapes, shared with production
        src/LayerAudibility.cpp  # §3.3 audibility rule under test (no JUCE dependency at all)
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
# EnvelopePublisher uses juce::Timer, from juce_events.
# Add juce_audio_basics or juce_dsp here when later test phases need them.
target_link_libraries(DOOFTests
    PRIVATE
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ─── Integration tests ──────────────────────────────────────────────────────
# Separate, heavier binary for tests that need the real PluginProcessor (and
# therefore PluginEditor/EnvelopeCanvas, since createEditor() references
# them) — full-engine tests like Phase 3's save/reload null-match. Kept out
# of DOOFTests deliberately, so day-to-day iteration there stays fast; run
# this target before a phase's final verify pass or when changing
# PluginProcessor-level behaviour.
juce_add_console_app(DOOFIntegrationTests PRODUCT_NAME "DOOFIntegrationTests")

target_sources(DOOFIntegrationTests
    PRIVATE
        tests/IntegrationTests.cpp
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/EnvelopeCanvas.cpp
        src/SubVoice.cpp
        src/ClickVoice.cpp
        src/ClickSampleLibrary.cpp
        src/EnvelopeModel.cpp
        src/BezierSegment.cpp
        src/EnvelopeEvaluator.cpp
        src/EnvelopePublisher.cpp
        src/DefaultEnvelopes.cpp
        src/LayerAudibility.cpp
        src/LayerStrip.cpp
)

target_include_directories(DOOFIntegrationTests PRIVATE src)

# JucePlugin_Name is normally generated by juce_add_plugin's wrapper; this is
# a plain console app, so PluginProcessor::getName() needs it defined directly.
target_compile_definitions(DOOFIntegrationTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JucePlugin_Name="DOOF"
        # Test-only: lets a test pump the message loop for a bounded time, so the GUI's ~30 fps
        # polls (the layer strips' solo/dim, the editor's contextual panel) can actually be
        # observed firing rather than only reasoned about. Never set on the DOOF target itself -
        # modal loops inside a plugin are a hosting hazard.
        JUCE_MODAL_LOOPS_PERMITTED=1
)

target_link_libraries(DOOFIntegrationTests
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_data_structures
        juce::juce_events
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Same click-sample wiring as the plugin, so the tests exercise the real loading path rather than
# a stand-in — including the empty case, which is what this repo normally builds.
doof_link_click_samples(DOOFIntegrationTests)

# Register the test binaries with CTest so `ctest` discovers and runs them automatically.
enable_testing()
add_test(NAME BasicTests COMMAND DOOFTests)
add_test(NAME IntegrationTests COMMAND DOOFIntegrationTests)
