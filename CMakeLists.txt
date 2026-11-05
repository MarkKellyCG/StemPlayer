cmake_minimum_required(VERSION 3.22)

# Static MSVC runtime so the .vst3 doesn't need the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(StemPlayer VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STEMPLAYER_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (leave empty to download JUCE 8.0.4)")
if(WIN32)
    set(_stemplayer_tests_default OFF)   # the process tests use POSIX shell scripts
else()
    set(_stemplayer_tests_default ON)
endif()
option(STEMPLAYER_BUILD_TESTS "Build the headless processor test app" ${_stemplayer_tests_default})

if(STEMPLAYER_JUCE_DIR)
    add_subdirectory("${STEMPLAYER_JUCE_DIR}" juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(STEMPLAYER_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/StemLoader.cpp
    Source/HelperRunner.cpp)

set(STEMPLAYER_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

juce_add_plugin(StemPlayer
    COMPANY_NAME "StemTools"
    PRODUCT_NAME "StemPlayer"
    PLUGIN_MANUFACTURER_CODE Stmt
    PLUGIN_CODE Stpl
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Sampler)

target_sources(StemPlayer PRIVATE ${STEMPLAYER_SOURCES})
target_compile_definitions(StemPlayer PUBLIC ${STEMPLAYER_DEFS})
target_link_libraries(StemPlayer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

if(STEMPLAYER_BUILD_TESTS)
    juce_add_console_app(StemPlayerTests PRODUCT_NAME "StemPlayerTests")
    target_sources(StemPlayerTests PRIVATE Tests/ProcessorTests.cpp ${STEMPLAYER_SOURCES})
    target_compile_definitions(StemPlayerTests PUBLIC ${STEMPLAYER_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(StemPlayerTests
        PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_audio_formats
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags)
endif()
