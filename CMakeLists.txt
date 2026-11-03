cmake_minimum_required(VERSION 3.22)
project(AutoTuneLite VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the Visual C++ runtime statically so the plugin runs on any Windows PC
# without the user having to install the "VC++ Redistributable".
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Downloads JUCE automatically the first time you configure (needs internet + git).
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(AutoTuneLite
    COMPANY_NAME                "YourName"
    PLUGIN_MANUFACTURER_CODE    Yrnm
    PLUGIN_CODE                 Atlt
    PRODUCT_NAME                "AutoTune Lite"
    FORMATS                     VST3 Standalone
    VST3_CATEGORIES             Fx "Pitch Shift"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE)   # copy the .vst3 yourself (see README) to avoid needing admin rights during the build

target_sources(AutoTuneLite PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/PianoRollComponent.cpp
    Source/PianoRollComponent.h
    Source/AutoTuneCore.h
    Source/AutoTuneEdit.h)

target_compile_definitions(AutoTuneLite PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AutoTuneLite
    PRIVATE juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

# Optional: command-line DSP tests (no JUCE involved).  cmake -DAUTOTUNE_BUILD_TESTS=ON ..
option(AUTOTUNE_BUILD_TESTS "Build the offline DSP test" OFF)
if(AUTOTUNE_BUILD_TESTS)
    add_executable(offline_test tests/offline_test.cpp)
    target_include_directories(offline_test PRIVATE Source)
    add_executable(edit_test tests/edit_test.cpp)
    target_include_directories(edit_test PRIVATE Source)
endif()
