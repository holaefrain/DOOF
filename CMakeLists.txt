cmake_minimum_required(VERSION 3.22)
project(DOOF VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(JUCE)

# ─── Plugin target ────────────────────────────────────────────────────────────
juce_add_plugin(DOOF
    VERSION                     "0.1.0"
    COMPANY_NAME                "EfrainCuessy"
    BUNDLE_ID                   "com.efraincuessy.doof"
    PLUGIN_MANUFACTURER_CODE    ECsy        # unique 4-char manufacturer code
    PLUGIN_CODE                 Doof        # unique 4-char plugin code
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "DOOF"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             "Instrument|Synth"
)

target_sources(DOOF
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
)

target_compile_definitions(DOOF
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(DOOF
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# ─── Unit tests ───────────────────────────────────────────────────────────────
juce_add_console_app(DOOFTests PRODUCT_NAME "DOOFTests")

target_sources(DOOFTests PRIVATE tests/BasicTests.cpp)

target_compile_definitions(DOOFTests
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(DOOFTests
    PRIVATE
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

enable_testing()
add_test(NAME BasicTests COMMAND DOOFTests)
