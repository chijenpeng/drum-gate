cmake_minimum_required(VERSION 3.22)

project(DrumGate VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(DRUMGATE_COMPILER_OPTIONS /W4)
else()
    set(DRUMGATE_COMPILER_OPTIONS "")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The detector and gain stage have no JUCE dependency.
add_library(drumgate_dsp STATIC
    Source/dsp/DrumDetector.cpp
    Source/dsp/DrumGateEngine.cpp)
target_include_directories(drumgate_dsp PUBLIC Source)
target_compile_options(drumgate_dsp PRIVATE ${DRUMGATE_COMPILER_OPTIONS})

juce_add_plugin(DrumGate
    PRODUCT_NAME "Drum Gate"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.drumgate"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Drgt
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Dynamics
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(DrumGateAssets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)

target_sources(DrumGate PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(DrumGate PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DrumGate
    PRIVATE
        drumgate_dsp
        DrumGateAssets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Renders the editor to a PNG without a host (see tests/snapshot.cpp).
juce_add_console_app(drumgate_snapshot)
target_sources(drumgate_snapshot PRIVATE
    tests/snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_compile_definitions(drumgate_snapshot PRIVATE
    JucePlugin_Name="Drum Gate"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(drumgate_snapshot PRIVATE drumgate_dsp DrumGateAssets juce::juce_audio_utils)

# The snapshot tool exits non-zero if the editor image cannot be written. This is the only
# registered test: it checks that the processor and editor construct, run audio and render.
enable_testing()
add_test(NAME drumgate_snapshot_renders
    COMMAND drumgate_snapshot drumgate_ctest_editor.png
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
