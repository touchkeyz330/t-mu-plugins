# T-MU plugins for Windows and macOS: VST3, AU (Mac) and AAX, built with JUCE.
cmake_minimum_required(VERSION 3.22)
project(TMU_PLUGINS VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Oldest macOS supported")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # no Visual C++ runtime needed
endif()

# JUCE: use a local copy if given (-DJUCE_DIR=...), otherwise download JUCE 8.0.10
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.10 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

if(APPLE)
    set(TMU_FORMATS VST3 AU AAX)
elseif(WIN32)
    set(TMU_FORMATS VST3 AAX)
else()
    set(TMU_FORMATS VST3)          # Linux: test builds only
endif()
option(TMU_NO_AAX "Leave out AAX" OFF)
if(TMU_NO_AAX)
    list(REMOVE_ITEM TMU_FORMATS AAX)
endif()

# tmu_plugin(<target> NAME <name> CODE <4 chars> [INSTRUMENT] DIR <folder> SOURCES <engine files>)
function(tmu_plugin target)
    cmake_parse_arguments(P "INSTRUMENT" "NAME;CODE;DIR;AAX_CAT;VST3_CAT" "SOURCES" ${ARGN})
    if(P_INSTRUMENT)
        set(synth TRUE)
        set(au_type kAudioUnitType_MusicDevice)
    else()
        set(synth FALSE)
        set(au_type kAudioUnitType_Effect)
    endif()
    juce_add_plugin(${target}
        COMPANY_NAME "T-MU"
        COMPANY_WEBSITE "https://github.com/touchkeyz330/t-mu-plugins"
        BUNDLE_ID "com.tmu.${target}"
        PLUGIN_MANUFACTURER_CODE TMuz
        PLUGIN_CODE ${P_CODE}
        PRODUCT_NAME "${P_NAME}"
        VERSION ${PROJECT_VERSION}
        FORMATS ${TMU_FORMATS}
        IS_SYNTH ${synth}
        NEEDS_MIDI_INPUT ${synth}
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AAX_CATEGORY ${P_AAX_CAT}
        VST3_CATEGORIES ${P_VST3_CAT}
        AU_MAIN_TYPE ${au_type})

    file(GLOB skin_files CONFIGURE_DEPENDS "${P_DIR}/skin/*")
    juce_add_binary_data(${target}_Data NAMESPACE BinaryData SOURCES ${skin_files} "${CMAKE_SOURCE_DIR}/shared/fonts/Inter-Bold.otf")

    target_sources(${target} PRIVATE ${P_SOURCES} shared/TmuProcessor.cpp shared/TmuEditor.cpp)
    target_include_directories(${target} PRIVATE shim shared "${P_DIR}")
    target_compile_definitions(${target} PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0
        MPC_DESKTOP=1 _USE_MATH_DEFINES NOMINMAX)
    target_link_libraries(${target}
        PRIVATE ${target}_Data juce::juce_audio_processors juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags)
    if(MSVC)
        target_compile_options(${target} PRIVATE /permissive- /utf-8 /bigobj)
    endif()
endfunction()

tmu_plugin(MeltChannel NAME "Melt Channel" CODE Mch1 DIR "${CMAKE_SOURCE_DIR}/plugins/MeltChannel"
    AAX_CAT AAX_ePlugInCategory_Dynamics VST3_CAT "Fx|Dynamics|EQ"
    SOURCES plugins/MeltChannel/MeltChannel.cpp)
tmu_plugin(MeltStutter NAME "Melt Stutter" CODE Mst1 DIR "${CMAKE_SOURCE_DIR}/plugins/MeltStutter"
    AAX_CAT AAX_ePlugInCategory_Delay VST3_CAT "Fx|Delay"
    SOURCES plugins/MeltStutter/MeltStutter.cpp)
tmu_plugin(MeltDeck NAME "Melt Deck" CODE Mdk1 INSTRUMENT DIR "${CMAKE_SOURCE_DIR}/plugins/MeltDeck"
    AAX_CAT AAX_ePlugInCategory_SWGenerators VST3_CAT "Instrument|Sampler"
    SOURCES plugins/MeltDeck/MeltDeck.cpp)

# ---- test harness (developer use: -DTMU_TESTS=ON)
option(TMU_TESTS "Build the test programs" OFF)
if(TMU_TESTS)
    foreach(t MeltChannel MeltStutter MeltDeck)
        juce_add_console_app(tmu_test_${t} PRODUCT_NAME "tmu_test_${t}")
        get_target_property(src ${t} SOURCES)
        list(FILTER src INCLUDE REGEX "plugins/")
        target_sources(tmu_test_${t} PRIVATE tests/PluginTest.cpp shared/TmuProcessor.cpp shared/TmuEditor.cpp ${src})
        get_target_property(name ${t} JUCE_PRODUCT_NAME)
        target_include_directories(tmu_test_${t} PRIVATE shim shared "${CMAKE_SOURCE_DIR}/plugins/${t}")
        target_compile_definitions(tmu_test_${t} PRIVATE JucePlugin_Name="${name}" JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
            MPC_DESKTOP=1 _USE_MATH_DEFINES NOMINMAX JUCE_STANDALONE_APPLICATION=0 JUCE_MODAL_LOOPS_PERMITTED=1)
        target_link_libraries(tmu_test_${t} PRIVATE ${t}_Data juce::juce_audio_processors juce::juce_gui_basics juce::juce_recommended_config_flags)
    endforeach()
endif()
