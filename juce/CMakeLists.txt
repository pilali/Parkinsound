cmake_minimum_required(VERSION 3.22)

# macOS: universal binaries (arm64 + x86_64) + minimum OS. Must be set
# BEFORE project().
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS archs")
endif()
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "min macOS")
endif()

project(ParkinsoundStepGate VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: fetched automatically. Override with -DLOCAL_JUCE_DIR=/path/to/JUCE
# for an offline / vendored checkout.
set(JUCE_TAG "8.0.8" CACHE STRING "JUCE git tag")
option(COPY_AFTER_BUILD "Install into user plug-in folders after build" ON)
if(DEFINED LOCAL_JUCE_DIR)
    add_subdirectory(${LOCAL_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${JUCE_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(ParkinsoundStepGate
    COMPANY_NAME "pilali"
    BUNDLE_ID com.pilali.parkinsound
    PLUGIN_MANUFACTURER_CODE Plli      # 4 chars, >=1 uppercase
    PLUGIN_CODE Pksn                   # 4 chars, >=1 uppercase
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD ${COPY_AFTER_BUILD}
    FORMATS AU VST3 Standalone         # no VST2 (SDK withdrawn by Steinberg)
    VST3_CATEGORIES Fx
    AU_MAIN_TYPE kAudioUnitType_Effect
    PRODUCT_NAME "Parkinsound Step Gate")

set(DSP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_sources(ParkinsoundStepGate PRIVATE
    PluginProcessor.cpp
    ${DSP_DIR}/stepgate_dsp.c)
target_include_directories(ParkinsoundStepGate PRIVATE ${DSP_DIR})

target_compile_definitions(ParkinsoundStepGate PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ParkinsoundStepGate
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
