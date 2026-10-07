cmake_minimum_required(VERSION 3.22)
# The version number is a readiness estimate in percent, not a sequential release counter.
# 0.9.9 promised ninety-nine and was too generous: the BOSS reverb chip was not emulated
# at all, which is a noticeable part of the unit's sound, and the eight individual
# outputs were missing too. The estimate was honestly lowered, even though the number goes backwards.
#
# 2026-08-04: both fixed. The BOSS reverb (chip HG61H20R36F/BOS-007, ROM IC6)
# now sounds through a real microcode emulation (BossEmu, ported from
# github.com/davidhsilaban/munt with authorship preserved - Sergey V. Mikayev), instead of an
# approximation through four MT-32 modes. Six individual outputs (MULTI OUT 1-6,
# plus the main stereo MIX bus) are wired up as separate VST3 buses, off by default -
# routing by the part's Output Assign was verified by measurement, the sound arrives
# exactly in the assigned channel and in no other.
project(D110Emulator VERSION 0.9.5)

# C++20 because MAME's headers need it.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CRITICAL: on Windows, MAME's Release libs are built against the static runtime, so
# everything here - JUCE and mt32emu included - must use /MT too or the link mixes
# runtimes. Doesn't apply to the Linux (GCC) build.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Comments in this project were historically written in Russian, and without this switch MSVC reads the source
# in the system code page and emits C4819 on Cyrillic - and in the worst case swallows
# a byte that looks like a backslash at the end of a comment line.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# OFF by default: D110EmulatorNative (the MAME-free native core, see below) is the primary
# supported build now that this fork keeps the original MAME-backed engine as a dormant
# fallback rather than the default. Turn this ON (and point mame.cmake's MAME_DIR at a built
# MAME 0.288+patch tree) to also build the original D110Emulator plugin and its MAME-only
# test probes.
option(D110_BUILD_MAME_BACKEND "Build the MAME-backed D110Emulator plugin and probes" OFF)

if(D110_BUILD_MAME_BACKEND)
    include("${CMAKE_CURRENT_SOURCE_DIR}/mame.cmake")
endif()

# Optional: a real JACK MIDI input port for the Standalone app (Alan's request, 2026-08-20 -
# see JackMidiInput.h). Linux-only concept, and entirely opt-in on top of that - only turns
# on when libjack's dev headers are actually installed, so this stays a no-op on Windows/
# macOS CI and on any Linux box without libjack-dev (or libjack-jackd2-dev/PipeWire's own
# jack-compatible package). Genuinely absent, not just disabled: D110_HAVE_JACK_MIDI unset
# means JackMidiInput.cpp is never even added to the target, and PluginProcessor.*'s own
# #ifdef D110_HAVE_JACK_MIDI blocks compile out completely.
set(D110_HAVE_JACK_MIDI FALSE)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(JACK QUIET jack)
    endif()
    if(NOT JACK_FOUND)
        find_library(JACK_LIBRARY_PATH NAMES jack)
        find_path(JACK_INCLUDE_PATH NAMES jack/jack.h)
        if(JACK_LIBRARY_PATH AND JACK_INCLUDE_PATH)
            set(JACK_FOUND TRUE)
            set(JACK_LIBRARIES "${JACK_LIBRARY_PATH}")
            set(JACK_INCLUDE_DIRS "${JACK_INCLUDE_PATH}")
        endif()
    endif()
    if(JACK_FOUND)
        set(D110_HAVE_JACK_MIDI TRUE)
        message(STATUS "D-110: libjack found, building the Standalone JACK MIDI input port")
    endif()
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Build mt32emu as a static library, C++ interface only (matches rom_test setup).
# PIC is required so this static lib can be linked into the VST3 shared module on Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS FALSE CACHE BOOL "" FORCE)
set(libmt32emu_SHARED FALSE CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE FALSE CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE TRUE CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu" "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build")

# The front panel IS the reference photograph - see docs/panel_reference_notes.md.
# app_icon.png (see its own docs/app_icon.svg comment) rides along in the same binary-data
# target purely for convenience - it's the app's window/taskbar icon on platforms JUCE's own
# ICON_BIG/ICON_SMALL CMake args don't cover (Linux), not anything panel-related. (Nonet
# Sequencer, formerly built from this repo too, has its own copy of this icon and its own
# binary-data target - see github.com/luginf/nonet-sequencer, extracted 2026-09-22.)
juce_add_binary_data(D110PanelData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/panel_reference.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/panel_reference_compact.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/memory_card_m256d.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/D20infos.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/app_icon.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../docs/volume_knob.png")

# On Windows, plugins conventionally live in the shared Program Files VST3 folder.
# Elsewhere, JUCE's own default (~/Library/Audio/Plug-Ins/VST3 on macOS, ~/.vst3 on Linux)
# is already the right per-user location, so leave VST3_COPY_DIR unset there.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(D110_VST3_COPY_DIR_ARGS VST3_COPY_DIR "C:/Program Files/Common Files/VST3")
else()
    set(D110_VST3_COPY_DIR_ARGS)
endif()

# AU (Audio Unit) only exists on Apple platforms - JUCE errors out if asked to build it
# elsewhere, so this has to be conditional rather than just always appended.
set(D110_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND D110_FORMATS AU)
endif()

# Linux releases also ship LV2 (JUCE's own format) and CLAP (via clap-juce-extensions, wired up
# right after the D110EmulatorNative target below). Only the native product gets them - the dormant
# MAME backend and the D-50 target keep D110_FORMATS.
set(D110_NATIVE_FORMATS ${D110_FORMATS})
if(UNIX AND NOT APPLE)
    list(APPEND D110_NATIVE_FORMATS LV2)
endif()
option(D110_BUILD_CLAP "Also build a CLAP plugin of the native product (Linux)" ON)

if(D110_BUILD_MAME_BACKEND)

juce_add_plugin(D110Emulator
    # Without this JUCE sets its own default version, and the host shows it instead of
    # the one the build was released under. The version lives in one place, project() above.
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "DIY"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    ${D110_VST3_COPY_DIR_ARGS}
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvst
    FORMATS ${D110_FORMATS}
    # Distinct from D110EmulatorNative's own "D-110 Emulator" below so the two can never
    # collide on disk (same .vst3 bundle name, same Standalone binary name) if this dormant,
    # opt-in backend is ever built alongside the shipping one - see this target's own header
    # comment for why it's the one that carries the qualifier now that MAME is the fallback,
    # not the default (2026-08-07).
    PRODUCT_NAME "D-110 Emulator MAME"
    PLUGIN_NAME "D-110 Emulator MAME"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../docs/app_icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../docs/app_icon.png"
)

# On Apple, MAME_DRIVLIST is built as its own tiny static lib instead of going straight into
# D110Emulator's sources, so the foreach() below can -force_load JUST that (see its comment
# for why: force-loading the whole D110Emulator archive pulled in D110Core.cpp.o's
# emulator_info:: overrides a second time, which are already reached normally, and ld64 has
# no --allow-multiple-definition to paper over the resulting duplicate-symbol error the way
# the Windows/Linux branches do).
if(APPLE)
    add_library(D110DriverList STATIC "${MAME_DRIVLIST}")
    target_include_directories(D110DriverList PRIVATE ${MAME_INCLUDES})
    target_compile_definitions(D110DriverList PRIVATE ${MAME_DEFS})
    set(D110_DRIVLIST_SOURCE)
else()
    set(D110_DRIVLIST_SOURCE "${MAME_DRIVLIST}")
endif()

target_sources(D110Emulator PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp Source/D110Keyboard.cpp
    Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/SoundbankDatabase.cpp
    Source/SoundbankBrowser.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp
    ${D110_DRIVLIST_SOURCE}
)

target_include_directories(D110Emulator PRIVATE ${MAME_INCLUDES})
target_compile_definitions(D110Emulator PRIVATE ${MAME_DEFS})

target_compile_definitions(D110Emulator PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(D110Emulator PRIVATE
    D110PanelData
    ${MAME_LIBS} ${MAME_SYS_LIBS}
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
)

if(APPLE)
    target_link_libraries(D110Emulator PRIVATE D110DriverList)
endif()

# MAME's own emulator_info definitions collide with ours by design, and its libs are
# built with a different debug-info setting than JUCE's.
foreach(_fmt VST3 Standalone AU)
    if(TARGET D110Emulator_${_fmt})
        target_link_options(D110Emulator_${_fmt} PRIVATE ${MAME_LINK_OPTIONS})
        if(APPLE)
            # Same problem as the Linux branch below (a static-archive member only pulled in
            # if something already needs it at that point in the link, and nothing does until
            # the MAME libs reference it later), ld64's answer instead of GNU ld's
            # --whole-archive/--no-whole-archive: force_load takes one archive, not a region,
            # so it needs its own flag per invocation rather than a pair of markers.
            # Confirmed by CI: force_load-ing the WHOLE D110Emulator archive (first attempt)
            # duplicate-symbol'd D110Core.cpp.o's emulator_info:: overrides against
            # libfrontend.a's mame.o, because D110Core.cpp.o is already reached normally and
            # force_load pulled it in a second time. D110DriverList is a single-object archive
            # (see above) holding only the generated driver_list table, so forcing it can't
            # cause that - it has nothing else in it to duplicate.
            target_link_options(D110Emulator_${_fmt} PRIVATE
                -Wl,-force_load,$<TARGET_FILE:D110DriverList>)
        elseif(NOT MSVC)
            # GNU ld only extracts a .a member when something already needs it at that point
            # in the link. libD-110_Emulator_SharedCode.a (which the MAME_DRIVLIST object -
            # driver_list::s_driver_count/s_drivers_sorted - ends up archived into) is linked
            # before the MAME libs that reference those symbols, so it's never pulled in.
            # --whole-archive forces every member in, same as a plain .o would behave.
            target_link_options(D110Emulator_${_fmt} PRIVATE
                -Wl,--whole-archive $<TARGET_FILE:D110Emulator> -Wl,--no-whole-archive)
        endif()
    endif()
endforeach()

target_link_libraries(D110Emulator PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

endif() # D110_BUILD_MAME_BACKEND

# ---------------------------------------------------------------------------
# D110EmulatorNative: the SAME PluginProcessor/PluginEditor, compiled with D110_NATIVE_CORE
# so D110CoreType resolves to D110CoreNative instead of the MAME-backed D110Core - see the
# "Native D-110 CPU core port" plan and PluginProcessor.h's own comment on D110CoreType.
# Deliberately links none of MAME_LIBS/MAME_INCLUDES/D110Core.cpp/MAME_DRIVLIST - that
# absence is the whole point of this target, not an optimisation. This is THE shipping
# product now (Alan's call, 2026-08-05 - see feedback_native_only_releases memory); the
# MAME-backed D110Emulator above is a dormant, opt-in fallback, not built by default. The
# CMake target keeps the "Native" name (an internal identifier only, invisible to users and
# threaded through ~60 probe executables' target_link_libraries calls), but the PRODUCT_NAME
# below - what actually shows up as the .vst3 bundle name, the Standalone binary, and the
# app's own settings file name - dropped it as of 2026-08-07: which CPU core runs underneath
# is an implementation detail, not something worth surfacing in the product's own name.
juce_add_plugin(D110EmulatorNative
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "DIY"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
    ${D110_VST3_COPY_DIR_ARGS}
    PLUGIN_MANUFACTURER_CODE Manu
    PLUGIN_CODE Dvsn
    FORMATS ${D110_NATIVE_FORMATS}
    LV2URI "https://github.com/luginf/d110-vst-emulator/plugins/di111"
    # Not "D-110 Emulator (Native)" and not parenthesised in general: parentheses in the
    # product name end up in the .vst3 bundle's own path, and CMake's Unix Makefiles
    # generator doesn't shell-escape them in the generated copy/remove recipes - `(` reaches
    # /bin/sh unquoted and breaks the build. Spaces are escaped there; parens are not, on
    # Linux. Windows never hit this.
    # Renamed "Di-111" 2026-10-01 (Alan's request): the Standalone binary, its window title and
    # the .vst3 bundle name. Folder names (D-110 Data, app-data "D-110 Emulator") are string
    # literals in PluginProcessor.cpp, unaffected; only the Standalone's settings file moves
    # (~/.config/Di-111.settings, previously "D-110 Emulator.settings").
    PRODUCT_NAME "Di-111"
    PLUGIN_NAME "Di-111"
    # The D-110 front panel's own "D-110" wordmark, vectorised - see docs/app_icon.svg's own
    # comment. Used for the Standalone window/taskbar icon (and the VST3 bundle icon on
    # platforms that show one); Android's separate launcher icon
    # (android/app/src/main/res/drawable/icon.png) is rendered from the same SVG, not this
    # CMake path - Android has no CMake-driven icon mechanism to hook into here.
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../docs/app_icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../docs/app_icon.png"
)

if(UNIX AND NOT APPLE AND D110_BUILD_CLAP)
    FetchContent_Declare(clap-juce-extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG main)
    FetchContent_MakeAvailable(clap-juce-extensions)
    clap_juce_extensions_plugin(TARGET D110EmulatorNative
        CLAP_ID "com.luginf.di111"
        CLAP_FEATURES instrument synthesizer)
endif()

target_sources(D110EmulatorNative PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp
    Source/native/d110_bus.cpp
    Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp
    Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/SoundbankDatabase.cpp
    Source/SoundbankBrowser.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp
)

target_compile_definitions(D110EmulatorNative PRIVATE D110_NATIVE_CORE=1)

if(D110_HAVE_JACK_MIDI)
    target_sources(D110EmulatorNative PRIVATE Source/JackMidiInput.cpp)
    target_compile_definitions(D110EmulatorNative PRIVATE D110_HAVE_JACK_MIDI=1)
    target_include_directories(D110EmulatorNative PRIVATE ${JACK_INCLUDE_DIRS})
    target_link_libraries(D110EmulatorNative PRIVATE ${JACK_LIBRARIES})
endif()

target_compile_definitions(D110EmulatorNative PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # On (JUCE's default). VST3 has no pitch bend/aftertouch/CC events: hosts such as Ardour
    # and FL Studio deliver them only through IMidiMapping, i.e. as parameter changes, and this
    # shim is what maps them back to real MIDI (GitHub issue #8). It was turned off on
    # 2026-08-20 because a host resetting every parameter to its default (Carla) made the
    # pitch bend shadow parameter (default 0 = fully down) land as a real Pitch Bend 0; that
    # is now handled in processBlock() (dropRogueInitialPitchBend), not by disabling this.
    JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=1
)

target_link_libraries(D110EmulatorNative PRIVATE
    D110PanelData
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
)

target_link_libraries(D110EmulatorNative PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

if(D110_BUILD_MAME_BACKEND)

# ---------------------------------------------------------------------------
# Offline audio check: instantiates the real processor, powers it on, plays a note
# and measures the output. No DAW, no audio device. Also the harness for proving
# that a panel edit becomes audible once the RAM bridge is in.
# ---------------------------------------------------------------------------
add_executable(d110_audio_test audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_audio_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_audio_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_audio_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_audio_test PRIVATE ${MAME_LINK_OPTIONS})

# Maps the firmware's battery RAM onto Roland's SysEx addresses by experiment: presses
# panel buttons and reports which bytes moved. See docs/sysex_address_map.md.
add_executable(d110_bridge_probe bridge_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_bridge_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_bridge_probe PRIVATE ${MAME_DEFS})
target_link_libraries(d110_bridge_probe PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_bridge_probe PRIVATE ${MAME_LINK_OPTIONS})

# ---------------------------------------------------------------------------
# Locates the Tone Temporary Area in the firmware's RAM by comparing bytes: reads the
# tone mt32emu holds for each part back out of the engine and slides it over the whole
# 32 KB. An exact match both pins the base and proves that mirroring an unedited tone
# changes nothing - the null test the bridge needed before the deep Edit pages could be
# wired up. See docs/sysex_address_map.md.
# ---------------------------------------------------------------------------
add_executable(d110_tone_probe tone_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_probe PRIVATE ${MAME_LINK_OPTIONS})

# Checks that loading the plugin does NOT re-initialise the instrument, and that the
# hardware's own factory init (latch WRITE/COPY, power on, ENTER) works from the panel.
add_executable(d110_coldstart_test coldstart_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_coldstart_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_coldstart_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_coldstart_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_coldstart_test PRIVATE ${MAME_LINK_OPTIONS})

# Checks that a project recalls its own firmware memory, that two instances get separate
# memory, and that the second one refuses to power on instead of crashing the host.
add_executable(d110_state_test state_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_state_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_state_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_state_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_state_test PRIVATE ${MAME_LINK_OPTIONS})

# Discovers the LA32's register interface by logging every access to an address the D-110's
# map leaves unclaimed - which is precisely where the missing sound board would answer.
add_executable(d110_la32_probe la32_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_probe PRIVATE ${MAME_LINK_OPTIONS})

# One-shot: play a single note, freeze it (no interrupt answered), dump the raw voice
# bookkeeping tables - ground truth for what StuckPolicy::La32Stub's slot scan should see.
add_executable(d110_la32_lifecycle la32_lifecycle_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_lifecycle PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_lifecycle PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_lifecycle PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_lifecycle PRIVATE ${MAME_LINK_OPTIONS})

# Live write tap over the voice-context bookkeeping arrays (f3c0..f480), engaged
# together with StuckPolicy::La32Stub - answers whether the firmware's own housekeeping
# ever reaches f460[] bit 6 at all (the second stall) instead of guessing from statics.
add_executable(d110_la32_ctx la32_ctx_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_ctx PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_ctx PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_ctx PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_ctx PRIVATE ${MAME_LINK_OPTIONS})

# Practical question hang_probe/longrun_test don't ask: with La32Stub engaged, does the
# panel survive NORMAL playing (single notes, realistic gaps), not deliberate chord-stress.
add_executable(d110_la32_realistic la32_realistic_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_realistic PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_realistic PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_realistic PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_realistic PRIVATE ${MAME_LINK_OPTIONS})

# Reproduces the user's own reported steps directly - EDIT+ENTER into ROM Play, ENTER to
# play the demo song - instead of only inferring from synthetic MIDI tests that it hits
# the same freeze. Also checks whether La32Stub fares differently here, since the demo
# song runs entirely on MAME's own thread with no host MIDI injection at all.
add_executable(d110_demo_song_repro demo_song_repro.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_song_repro PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_song_repro PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_song_repro PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_song_repro PRIVATE ${MAME_LINK_OPTIONS})

# Does a factory reset actually restore the documented per-part pan fan, and does that
# byte reach the sound engine as a real stereo shift? Answers the "panning feels wrong /
# it won't return to defaults" question by measurement rather than by listening.
add_executable(d110_pan_verify pan_verify.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_verify PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_verify PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_pan_verify PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_verify PRIVATE ${MAME_LINK_OPTIONS})

# The demo song is silent because the firmware plays it internally and never transmits it.
# This checks whether the notes are nevertheless recoverable from the firmware's own RAM
# (f400 = note, f420 = velocity, f3a0 = part?), which would let the bridge carry them.
add_executable(d110_note_source note_source_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_note_source PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_note_source PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_note_source PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_note_source PRIVATE ${MAME_LINK_OPTIONS})

# Renders the demo song to a WAV so "is there any sound at all" is answered by a file the
# user can listen to, not by inference.
add_executable(d110_demo_wav demo_wav_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_wav PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_wav PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_demo_wav PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_wav PRIVATE ${MAME_LINK_OPTIONS})

# Sounds each part on its own and measures it, so "part 6 lights up but makes no noise" is
# answered per part instead of by ear across nine at once. Two passes: straight into the
# engine, and through the firmware on that part's own channel.
add_executable(d110_part_audio part_audio_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_audio PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_audio PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_audio PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_audio PRIVATE ${MAME_LINK_OPTIONS})

# Renders the demo song once per part, with only that part delivered, and measures what
# comes out. Note counts cannot tell "silent" from "buried"; a level can.
add_executable(d110_solo_level solo_level_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_solo_level PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_solo_level PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_solo_level PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_solo_level PRIVATE ${MAME_LINK_OPTIONS})

# Compares, per part and DURING the demo, what the firmware holds against what the engine
# holds. A level 20 dB adrift is either the bridge failing to carry something or the
# arrangement; only a byte-for-byte comparison tells them apart.
add_executable(d110_part_state part_state_compare.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_part_state PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_part_state PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_part_state PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_part_state PRIVATE ${MAME_LINK_OPTIONS})

# Asks for every part, in ONE run, whether the engine holds the same timbre as the
# firmware does - by name, which a level measurement cannot do. Proves the engine's own read path
# with a "write and read back" control before any other readings.
add_executable(d110_tone_clobber tone_clobber_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_clobber PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_clobber PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_clobber PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_clobber PRIVATE ${MAME_LINK_OPTIONS})

# Tells apart two causes of "Attempted to play unmapped key" at the start of the demo: a rhythm map
# that has not reached the engine yet, and a map in which the key really is switched off.
add_executable(d110_rhythm_map rhythm_map_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_map PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_map PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_map PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_map PRIVATE ${MAME_LINK_OPTIONS})

# Looks for the path by which the reverb type/time/level reach the BOSS chip: presses
# buttons, prints the screen and shows RAM shifts and SO register writes side by side.
add_executable(d110_reverb_path reverb_path_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_path PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_path PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_path PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_path PRIVATE ${MAME_LINK_OPTIONS})

# LA32 register map: what the firmware puts into the synth chip and what each cell means.
# Three stimuli differing in exactly one property, plus a window with no note as a control.
add_executable(d110_la32_regmap la32_regmap_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_regmap PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_regmap PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_regmap PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_regmap PRIVATE ${MAME_LINK_OPTIONS})

# Does the factory patch bank reach the firmware itself and what does it change in its memory. The
# "before and after" difference is compared with a control window of the same length WITHOUT an import, because
# the firmware writes continuously into its work areas on its own.
add_executable(d110_factory_bank factory_bank_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_factory_bank PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_factory_bank PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_factory_bank PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_factory_bank PRIVATE ${MAME_LINK_OPTIONS})

# The bridge's reverse path: does an edit sent as an exclusive message reach the firmware
# itself, where does it land and does Mem Protect forbid it. This is the foundation of the extended
# editor - without it an edit from the editor would be a guess. Also measures the location of the TONE
# memory, which cannot be found by content: it is empty in a factory unit.
add_executable(d110_editor_write editor_write_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_write PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_write PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_editor_write PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_write PRIVATE ${MAME_LINK_OPTIONS})

# A PNG snapshot of the extended editor - the real component, with live values from a
# running unit. Layout cannot be judged from constants, so it is photographed and looked at.
add_executable(d110_editor_shot editor_shot.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_shot PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_shot PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    # Not an actual JUCE plugin format target, so PluginEditor.cpp's own
    # JucePlugin_Build_Standalone checks (see the sequencer drawer, Standalone/Android only
    # since 2026-09-04) would otherwise default to the trimmed VST3/AU layout - wrong for a
    # tool whose whole point is rendering the full editor, every drawer included, for visual
    # review.
    JucePlugin_Build_Standalone=1)
target_link_libraries(d110_editor_shot PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_shot PRIVATE ${MAME_LINK_OPTIONS})

# Does an edit from the extended editor reach the unit, the sound, and come back. Checks
# every sender against the firmware memory, audibility by RMS with a control run, and
# the switch to a patch by the number the unit ends up on.
#
# JUCE_MODAL_LOOPS_PERMITTED is enabled here deliberately and only for this purpose: the switch to a patch
# goes through the message queue timer, and in a console program that queue has to be pumped by hand.
add_executable(d110_editor_test editor_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_editor_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_editor_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_editor_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_editor_test PRIVATE ${MAME_LINK_OPTIONS})

# Looks for the address by which the firmware sets the reverb TYPE: intercepts all of the free
# external I/O 0x0400-0x0BFF, steps through the eight types and compares with a control
# run where the key presses and the note are the same but the type does not change.
add_executable(d110_reverb_reg reverb_reg_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_reverb_reg PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reverb_reg PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reverb_reg PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_reverb_reg PRIVATE ${MAME_LINK_OPTIONS})

# Does a timbre edit survive a part parameter change on a NORMAL patch - a question the
# demo song cannot answer, because its timbre group 5 is unreachable from the panel. Sets
# up the experiment twice in one run, with and without timbre confirmation, so that the result has a
# control capable of showing a failure.
add_executable(d110_tone_edit_survives tone_edit_survives_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_tone_edit_survives PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_tone_edit_survives PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_tone_edit_survives PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_tone_edit_survives PRIVATE ${MAME_LINK_OPTIONS})

# The first bridge from registers to sound: state captured from the firmware is run through the model of the
# same chip from munt. Includes its internal headers - the library is static, the symbols are
# there, but the wave generator has no public interface and none is planned.
add_executable(d110_la32_render la32_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_render PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../munt/mt32emu/src"
    "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build/src")
target_compile_definitions(d110_la32_render PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_render PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_render PRIVATE ${MAME_LINK_OPTIONS})

# What the 0x0CC0 bank carries over time while a note sounds. Prints the stream as a series rather than comparing
# sets of values - that comparison is what the three earlier attempts at this bank failed on.
add_executable(d110_la32_stream la32_stream_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_la32_stream PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_la32_stream PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_la32_stream PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_la32_stream PRIVATE ${MAME_LINK_OPTIONS})

# Captures the real panel to a PNG so there is something to look at next to the photograph.
add_executable(d110_panel_render panel_render.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_panel_render PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
# The map's progress is driven by the panel's own timer, so the test rig needs a real message
# loop; the plugin does not need it and does not enable it.
target_compile_definitions(d110_panel_render PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_panel_render PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_panel_render PRIVATE ${MAME_LINK_OPTIONS})

# What the firmware uses to tell an inserted memory card from an empty slot. Feeds it four
# different cards and captures from the screen what it said about each.
add_executable(d110_card card_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_card PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_card PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_card PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_card PRIVATE ${MAME_LINK_OPTIONS})

# Looks for the moment when the firmware reprograms the reverb chip: writes every
# write to the SO register together with the address of the routine that made it.
add_executable(d110_so_trace so_trace_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_so_trace PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_so_trace PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_so_trace PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_so_trace PRIVATE ${MAME_LINK_OPTIONS})

# Renders the LCD at several settings to PNG so the result can be looked at beside the
# reference photo. Calibration only - not part of the plugin's render path.
add_executable(d110_lcd_check lcd_render_check.cpp)
target_compile_definitions(d110_lcd_check PRIVATE
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_lcd_check PRIVATE juce::juce_gui_basics juce::juce_graphics)

# Checks that an imported SysEx bank actually reaches the control board, rather than only
# the sound engine - where the next RAM mirror silently undid it.
add_executable(d110_sysex_test sysex_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_sysex_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sysex_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_sysex_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_sysex_test PRIVATE ${MAME_LINK_OPTIONS})

# Reads the firmware back as MCS-96 assembly, using MAME's own disassembler. Needed to see
# what the loop hang_probe found is actually waiting for.
# D110Core.cpp comes along only for the emulator_info definitions MAME's frontend expects.
add_executable(d110_disasm disasm_tool.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_disasm PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_disasm PRIVATE ${MAME_DEFS})
target_link_libraries(d110_disasm PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_disasm PRIVATE ${MAME_LINK_OPTIONS})

# Finds WHERE the firmware gets stuck once notes reach it, by sampling the program counter
# in a healthy state and a dead one and diffing the two histograms.
add_executable(d110_hang_probe hang_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_hang_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_hang_probe PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_hang_probe PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_hang_probe PRIVATE ${MAME_LINK_OPTIONS})

# Isolates why the control board stops responding after some seconds of playing: runs the
# firmware for a minute with and without host MIDI, pressing a button throughout.
add_executable(d110_longrun_test longrun_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_longrun_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_longrun_test PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_longrun_test PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_longrun_test PRIVATE ${MAME_LINK_OPTIONS})

# Records what really happens when two machines run in one process, which is what a DAW
# does as soon as a second instance of the plugin is loaded. Diagnostic, not a fix.
add_executable(d110_two_instance_test two_instance_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_two_instance_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_two_instance_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_two_instance_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_two_instance_test PRIVATE ${MAME_LINK_OPTIONS})

# Console check of the D110Core lifecycle: boots the real firmware in-process and prints
# the MSM6222B's own dot matrix while pressing front-panel buttons. Proves the wrap
# independently of the plugin.
add_executable(d110_core_test core_test.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_core_test PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_core_test PRIVATE ${MAME_DEFS})
target_link_libraries(d110_core_test PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_core_test PRIVATE ${MAME_LINK_OPTIONS})

# Where notes go during fast playing: either the LA32 stub loses them or there is nowhere to play them.
# Counts a note at three checkpoints - sent, taken by the firmware, taken by the engine - and does this
# twice, with a two-partial tone and a four-partial tone, so that polyphony cannot be mistaken for loss.
add_executable(d110_polyphony polyphony_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_polyphony PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_polyphony PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_polyphony PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_polyphony PRIVATE ${MAME_LINK_OPTIONS})

# Two parts (bass + solo) playing fast, overlapping lines AT THE SAME TIME - the user's actual
# reported shape (both parts losing notes together), not one part tested in isolation.
add_executable(d110_multi_part_polyphony multi_part_polyphony_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_multi_part_polyphony PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_multi_part_polyphony PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_multi_part_polyphony PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_multi_part_polyphony PRIVATE ${MAME_LINK_OPTIONS})

# Measures the real DSP cost of raising mt32emu's usePartialCount - no real-time pacing in its
# timed section, unlike every other probe here, on purpose.
add_executable(d110_partial_count_cpu partial_count_cpu_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_partial_count_cpu PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_partial_count_cpu PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_partial_count_cpu PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_partial_count_cpu PRIVATE ${MAME_LINK_OPTIONS})

# Owner's live-DAW report: D110Emulator (MAME-backed) sustains notes forever and freezes the
# LCD on Fantasy/Steel Drum, while D110EmulatorNative is fine - so this is backend-specific,
# not the mechanism fixed earlier this session (which was ported to both).
add_executable(d110_mame_stuck_note_repro mame_stuck_note_repro.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_stuck_note_repro PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_stuck_note_repro PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_mame_stuck_note_repro PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_mame_stuck_note_repro PRIVATE ${MAME_LINK_OPTIONS})

# Who writes to the LA32 slot table and whether anyone ever frees it. Intercepting writes to
# the dispatch window gives the address, value and PC - i.e. it answers not with a guess but with the name
# of a routine. See docs/la32_interface.md.
add_executable(d110_slot_life slot_life_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_slot_life PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_slot_life PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_slot_life PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_slot_life PRIVATE ${MAME_LINK_OPTIONS})

# Restoring the unit's real battery-backed memory after the file was corrupted (for example by an abrupt
# power loss in the middle of a write). Goes the same way as the plugin itself - through
# D110AudioProcessor::getNvramRoot() without a single argument of its own - so it fixes exactly
# the file that the installed VST3 will open.
add_executable(d110_nvram_recovery nvram_recovery.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_nvram_recovery PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_nvram_recovery PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_nvram_recovery PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_nvram_recovery PRIVATE ${MAME_LINK_OPTIONS})

# Checks the documented factory-reset procedure ("POWER off, Ctrl+click WRITE/COPY,
# POWER on, ENTER") with real presses on a real D110Panel - unlike
# coldstart_test.cpp, which calls D110Core::setButton() bypassing the panel and so could not
# catch a regression in D110Panel::setButtonState() itself.
add_executable(d110_panel_factory_init panel_factory_init_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_panel_factory_init PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_panel_factory_init PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_panel_factory_init PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_panel_factory_init PRIVATE ${MAME_LINK_OPTIONS})

# How much the firmware's note registration time actually diverges when several
# parts strike at once - measured for real through D110Core::startNoteLog(), rather than by
# recomputing the byte rate on paper. See docs on the shared MIDI queue to the firmware.
add_executable(d110_midi_congestion midi_congestion_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_midi_congestion PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_midi_congestion PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_midi_congestion PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_midi_congestion PRIVATE ${MAME_LINK_OPTIONS})

# Loading a tone bank into the unit's real memory. A separate tool, not part of
# nvram_recovery: that one restores the factory state, and the tone memory in the factory state is
# EMPTY, and it can only be filled with a bank from outside.
add_executable(d110_bank_import bank_import.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_bank_import PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_bank_import PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_bank_import PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_bank_import PRIVATE ${MAME_LINK_OPTIONS})

# Reproduction of the complaint "notes get stuck on all parts at once". Separate from d110_polyphony:
# that one measures note LOSS on a single part, while this one measures the RESIDUE with the keys released on all
# nine, and these are opposite symptoms.
add_executable(d110_stuck_notes stuck_notes_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_stuck_notes PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_stuck_notes PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_stuck_notes PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_stuck_notes PRIVATE ${MAME_LINK_OPTIONS})

# MAME-backed counterpart to d110_native_ramp_edge_stress - same dense overlapping/legato
# pattern, against D110Core.cpp's own midiTick() fix.
add_executable(d110_mame_ramp_edge_stress mame_ramp_edge_stress_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_ramp_edge_stress PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_ramp_edge_stress PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_mame_ramp_edge_stress PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_mame_ramp_edge_stress PRIVATE ${MAME_LINK_OPTIONS})

# Plays each rhythm key individually and measures the peak. Answers "closed hat does not
# sound" by measurement, not by guesswork - gives the list of silent keys directly, without assumptions
# about which one exactly is broken.
add_executable(d110_rhythm_key rhythm_key_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_key PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_key PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_key PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_key PRIVATE ${MAME_LINK_OPTIONS})

# Check: does reselecting the current patch return the live Timbre Temp to the patch data.
# The live area was read as a solid center (pan 7 on all eight) with correct data in the patch
# itself - the question is whether the same press the panel uses fixes it.
add_executable(d110_pan_resync pan_resync_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_resync PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_resync PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_pan_resync PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_resync PRIVATE ${MAME_LINK_OPTIONS})

# Check on a virgin, temporary NVRAM: right after the factory reset, BEFORE a single user
# press, is the live pan read correctly or as a solid center - separates a "firmware boot
# bug" from "the accumulated trace of today's experiments in the real file".
add_executable(d110_virgin_boot virgin_boot_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_virgin_boot PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_virgin_boot PRIVATE ${MAME_DEFS})
target_link_libraries(d110_virgin_boot PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_virgin_boot PRIVATE ${MAME_LINK_OPTIONS})

# Traces writes to the full voice circuit tables (dispatch + completion) for a single hit
# on a rhythm key, to find the source of "Attempted to play invalid key 1" near closed
# hat/open hat: whether the firmware itself writes this into f400[], or the substitution happens in our bridge.
add_executable(d110_rhythm_ctx rhythm_ctx_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_ctx PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_ctx PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_rhythm_ctx PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_ctx PRIVATE ${MAME_LINK_OPTIONS})

# "The patches in the demo song do not match" - separates two possible explanations: the demo touches
# Patch/Timbre Memory (so it depends on what really lies in the user's battery-backed memory,
# which has no real factory names without a FACTORY PRESET card), or the demo
# is entirely self-contained and does not touch the user's patches at all.
add_executable(d110_demo_patch demo_patch_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_demo_patch PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_demo_patch PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_demo_patch PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_demo_patch PRIVATE ${MAME_LINK_OPTIONS})

# "Sync with Ableton drifts" - measures the real delay between feeding a note into
# processBlock and the moment the firmware itself registers it (NoteLog). The demo song never goes
# through this loop (the firmware plays to itself), so it does not drift.
add_executable(d110_note_latency note_latency_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_note_latency PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_note_latency PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_note_latency PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_note_latency PRIVATE ${MAME_LINK_OPTIONS})

# Checks the ported BOSS reverb not at compile time but on sound: is
# ic6.bin loaded, does the reverb type byte (System 0x2D95) reach BossEmu, and do different
# ones of the eight types give a noticeably different decay tail.
add_executable(d110_boss_reverb boss_reverb_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_boss_reverb PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_boss_reverb PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_boss_reverb PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_boss_reverb PRIVATE ${MAME_LINK_OPTIONS})

# Check of a single reverb type in a FRESH instance (argument argv[1]) - separates
# "the type is quiet by itself" from "switching between types breaks something".
add_executable(d110_boss_reverb_iso boss_reverb_isolated_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_boss_reverb_iso PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_boss_reverb_iso PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_boss_reverb_iso PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_boss_reverb_iso PRIVATE ${MAME_LINK_OPTIONS})

# Checks the ported six individual outputs on sound routing, not only on
# compilation: enables all the extra buses, changes the Output Assign of part 1, measures the energy in
# each of the eight channels.
add_executable(d110_multi_output multi_output_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_multi_output PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_multi_output PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_multi_output PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_multi_output PRIVATE ${MAME_LINK_OPTIONS})

# Regression by ear: you play a kick (35), then Closed Hi-Hat (42) - and on the hi-hat a
# neighbouring drum sounds. The cause was that the substitution hint was queued for
# ANY rhythm note, not just for the three broken keys - working keys choked the queue.
add_executable(d110_rhythm_hint_bleed rhythm_hint_bleed_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_hint_bleed PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_hint_bleed PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_rhythm_hint_bleed PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_hint_bleed PRIVATE ${MAME_LINK_OPTIONS})

# Diagnostic: pan_verify.cpp just showed 6 of 8 parts silent after a factory
# reset. Dump of the live Timbre Temp/Tone Temp for all eight parts right after the same
# reset - separates "the Synth.cpp edits broke the render" from "the live area simply is not
# filled by the reset for most parts".
add_executable(d110_pan_verify_diag pan_verify_diag_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_pan_verify_diag PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pan_verify_diag PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_pan_verify_diag PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_pan_verify_diag PRIVATE ${MAME_LINK_OPTIONS})

# A real sound measurement of the drum pan - until now only the byte in
# memory was checked against the factory dump, never the real sound on both channels.
add_executable(d110_rhythm_pan_audio rhythm_pan_audio_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/D110Core.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_rhythm_pan_audio PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_rhythm_pan_audio PRIVATE ${MAME_DEFS}
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_rhythm_pan_audio PRIVATE
    D110PanelData ${MAME_LIBS} ${MAME_SYS_LIBS} MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
target_link_options(d110_rhythm_pan_audio PRIVATE ${MAME_LINK_OPTIONS})

endif() # D110_BUILD_MAME_BACKEND

# ---------------------------------------------------------------------------
# Native D-110 CPU core port (see the "Native D-110 CPU core port" plan) - the D-110's own
# i8x9x/MCS-96 CPU, hand-ported out of MAME's device/scheduler framework into a standalone
# interpreter (Source/native/), the same treatment DX100-VST already gave its DX100. Runs
# stepped inline on the audio thread instead of on MAME's own real-time thread, which is
# what the MAME-backed D110Core needs and is the root cause of its measured MIDI-to-sound
# jitter (note_latency_probe.cpp). Phase 1 scope only: CPU core + a minimal memory map
# (bank register, ROM windows, rams window; LCD/panel/SO stubbed) - enough to boot the real
# firmware ROM and prove the interpreter itself is correct via a PC-trace comparison against
# the MAME-backed core. Deliberately links NONE of MAME_LIBS/MAME_INCLUDES/JUCE - that
# absence is itself the thing this phase has to prove, not just an optimization.
# ---------------------------------------------------------------------------
add_executable(d110_native_boot native_boot_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp)
target_include_directories(d110_native_boot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 2 gate: LCD + panel buttons, verified by the same ASCII-art dump technique as
# core_test.cpp (the MAME-backed original) - run both, compare the printed screens by eye.
# Still zero MAME/JUCE dependency, same as d110_native_boot above.
add_executable(d110_native_core_test native_core_test.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_core_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 3 gate, NVRAM half: round-trip through D110CoreNative::start()/stop(), and format
# compatibility against a folder an existing MAME-backed D110Core session already wrote.
add_executable(d110_native_nvram native_nvram_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_nvram PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 3 gate, RAM-mirror half: does popSysex() actually hand back well-formed DT1 messages
# once the native core is past its boot-settle resync.
add_executable(d110_native_mirror native_mirror_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_mirror PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Phase 4 gate: MIDI-in -> firmware -> NoteEvent round trip.
add_executable(d110_native_note native_note_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_note PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Chord stress test - exercises serviceLa32Stub()'s per-context slot lookup, which a single
# note never needs to distinguish.
add_executable(d110_native_chord native_chord_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# First real sound: native core + real mt32emu Synth, rendered to a WAV file. Links MT32Emu
# only - still no MAME, no JUCE.
add_executable(d110_native_audio native_audio_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(d110_native_audio PRIVATE MT32Emu::mt32emu)

# Phase 6 gate: factory reset done entirely through the native core, checked against the
# same known factory-default bytes the MAME-backed tool's output was checked against by hand.
add_executable(d110_native_factory_reset native_factory_reset_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_factory_reset PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The actual jitter measurement, same technique as note_latency_probe.cpp against the
# MAME-backed core - see that file's own header for why this is the real question.
add_executable(d110_native_note_latency native_note_latency_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_note_latency PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# The check that actually matters: the REAL D110AudioProcessor, compiled against the native
# backend, driven by processBlock() exactly as a DAW would - not a probe that steps
# D110CoreNative directly. This is what caught the missing runForSeconds() call.
add_executable(d110_native_chord_freeze_repro native_chord_freeze_repro.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord_freeze_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(d110_native_chord_audio_repro native_chord_audio_repro.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_chord_audio_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(d110_native_chord_audio_repro PRIVATE MT32Emu::mt32emu)

add_executable(d110_native_polyphony_stress native_polyphony_stress_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_polyphony_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Targeted repro for the specific EXTINT-falling-edge-skipped mechanism found while reading
# serviceStuckPolicy() - see the probe's own header comment.
add_executable(d110_native_ramp_edge_stress native_ramp_edge_stress_probe.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp)
target_include_directories(d110_native_ramp_edge_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(d110_native_plugin_smoke native_plugin_smoke_test.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_plugin_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_plugin_smoke PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_plugin_smoke PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Custom PCM wave sample injection (loadCustomPcmWave()/restoreFactoryPcmWave()) - see the
# probe's own header comment. Same shape as d110_native_plugin_smoke just above.
add_executable(d110_pcm_custom_sample pcm_custom_sample_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_pcm_custom_sample PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pcm_custom_sample PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_pcm_custom_sample PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats)

# One-off measurement: what PCM wave pitch-calibration value makes a custom sample play back
# at its own recorded pitch (see the probe's own header comment for the full story - this is
# what tuned kNeutralPcmPitchOffset in PluginProcessor.cpp). Not a regression test in the usual
# sense (its answer is baked into the constant once), kept for re-verification if TVP.cpp's
# pitch model or the resample target rate ever changes.
add_executable(d110_pcm_pitch_calibration pcm_pitch_calibration_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_pcm_pitch_calibration PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pcm_pitch_calibration PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_pcm_pitch_calibration PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats)

# Same shape as d110_native_plugin_smoke - the real D110AudioProcessor, wrapperType forced to
# Standalone exactly as JUCE's own Standalone wrapper does it (see the probe's own header
# comment), driven by prepareToPlay()/processBlock() directly. Only built when libjack was
# found (D110_HAVE_JACK_MIDI, see this file's own detection block near the top) - otherwise
# the probe still builds and runs, it just skips the JACK-specific checks.
add_executable(d110_jack_midi_input_probe jack_midi_input_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_jack_midi_input_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_jack_midi_input_probe PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_jack_midi_input_probe PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)
if(D110_HAVE_JACK_MIDI)
    target_sources(d110_jack_midi_input_probe PRIVATE Source/JackMidiInput.cpp)
    target_compile_definitions(d110_jack_midi_input_probe PRIVATE D110_HAVE_JACK_MIDI=1)
    target_include_directories(d110_jack_midi_input_probe PRIVATE ${JACK_INCLUDE_DIRS})
    target_link_libraries(d110_jack_midi_input_probe PRIVATE ${JACK_LIBRARIES})
endif()

# Ground-truth reference note (no host, no Carla) for comparing against a Carla-hosted
# recording of the exact same note/patch - see the probe's own header comment.
add_executable(d110_reference_pitch_probe reference_pitch_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_reference_pitch_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_reference_pitch_probe PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_reference_pitch_probe PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats)

# Dumps the real firmware's 128 factory Patch names (Program Change 0-127) - see the probe's
# own header comment. One-off data generation for a MusE .idf, not a regression test.
add_executable(d110_patch_name_dump_probe patch_name_dump_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_patch_name_dump_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_patch_name_dump_probe PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_patch_name_dump_probe PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke - checks whether a TimbreTemp group/number write via
# sendTimbreTempParam genuinely sticks on the real firmware RAM; see the probe's own header
# comment.
add_executable(d110_native_timbre_group_number native_timbre_group_number_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_timbre_group_number PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_timbre_group_number PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_timbre_group_number PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke - reproduces Alan's single-part "notes stop getting
# through, Super Mode toggle or a patch change unsticks it" report; see the probe's own
# header comment.
add_executable(d110_native_single_part_stuck native_single_part_stuck_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_single_part_stuck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_single_part_stuck PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_single_part_stuck PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Alan's report, 2026-08-30: MIDI PANIC (Utility tab) didn't reset a stuck note's audio or
# the Monitor tab's voice-slot grid - see the probe's own header comment.
add_executable(d110_native_midi_panic_stuck native_midi_panic_stuck_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_midi_panic_stuck PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_midi_panic_stuck PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_midi_panic_stuck PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Reproduces Alan's 2026-08-06 JACK/Focusrite report: retriggering a note (same or a
# different pitch) while the previous one's firmware voice slot is still busy produces no
# audible sound - see the probe's own header comment for why this checks actual rendered
# audio, not just firmware bookkeeping the way native_single_part_stuck_probe does.
add_executable(d110_native_retrigger_silence native_retrigger_silence_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_retrigger_silence PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_retrigger_silence PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_retrigger_silence PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Longer, randomised companion to d110_native_retrigger_silence: thousands of notes over a
# simulated session, watching for state that drifts into occasional silence rather than
# testing one fixed retrigger shape - see the probe's own header comment.
add_executable(d110_native_long_session_stress native_long_session_stress_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_long_session_stress PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_long_session_stress PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_long_session_stress PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Isolates whether notes going silent over a long session is tied to a specific tone's attack
# time (ruled out for part 8/Strings 3 by native_retrigger_silence_probe's tone-swap test) or
# a genuine cumulative drift - one part, one fast-attack tone, thousands of notes, watching for
# exactly when (if ever) it starts failing. See the probe's own header comment.
add_executable(d110_native_cumulative_drift native_cumulative_drift_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_cumulative_drift PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_cumulative_drift PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_cumulative_drift PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Alan's decisive clue: the cutout only follows interacting with the Editor drawer (tone/
# patch selection) while playing, and clears up when reselecting from the native panel
# instead - checks the documented parameter-vs-note race for the Editor's own direct
# TimbreTemp write specifically, landing while notes are actively streaming. See the probe's
# own header comment.
add_executable(d110_native_editor_race native_editor_race_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_editor_race PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_editor_race PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_editor_race PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Whether the real D-110 firmware's Display Message SysEx (0x200000) can put independent
# text on both physical LCD rows, or only ever fills one 20-byte buffer - Alan asked
# 2026-08-29. Reads the LCD back through the real CGROM, same technique as
# editor_write_probe.cpp's screen(), against the native core instead of MAME's. See the
# probe's own header comment.
add_executable(d110_native_display_line native_display_line_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_display_line PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_display_line PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_display_line PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Follow-up to d110_native_editor_race, this time for the PATCHES tab's selectPatch() - a
# queued, juce::Timer-driven sequence of simulated button presses, properly pumped via
# runDispatchLoopUntil this time (earlier probes calling selectPatch() never did, so its
# timer likely never fired in them at all). See the probe's own header comment.
add_executable(d110_native_patch_select_race native_patch_select_race_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_patch_select_race PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_patch_select_race PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_native_patch_select_race PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Alan's screenshot: patch I-11 selected, then a chord played - Monitor showed the engine
# holding 0 partials/no parts sounding while the firmware's own voice-slot table showed 12
# busy. See the probe's own header comment.
add_executable(d110_native_chord_patch native_chord_patch_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_chord_patch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_chord_patch PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_native_chord_patch PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Loads Alan's own saved snapshot (confirmed to reproduce the cutout on this machine too, not
# just his JACK/Focusrite one) and hammers notes/chords on it. See the probe's own header
# comment.
add_executable(d110_native_snapshot_repro native_snapshot_repro_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_snapshot_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_snapshot_repro PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_native_snapshot_repro PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Isolates the duplicate-channel-mapping detail found in Alan's reproducing snapshot (parts 3
# and 4 both on MIDI channel 3) on an otherwise fresh/default instance. See the probe's own
# header comment.
add_executable(d110_native_duplicate_channel native_duplicate_channel_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_duplicate_channel PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_duplicate_channel PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_native_duplicate_channel PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Alan's fully deterministic repro: load his snapshot, select patch 01, change Part 1's tone
# to AcouPiano1 via the Editor's own direct-write mechanism (not the native panel) - notes go
# silent every time. See the probe's own header comment.
add_executable(d110_native_editor_tone_repro native_editor_tone_repro_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_editor_tone_repro PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_editor_tone_repro PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_native_editor_tone_repro PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Alan's report (2026-08-22): a MIDI file's track 7 writes a custom Tone + TimbreTemp
# assignment for Part 6 via raw SysEx (no Program Change at all) - see the probe's own
# header comment for the exact bytes and address reasoning.
add_executable(d110_midi_import_bombastic midi_import_bombastic_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_midi_import_bombastic PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_midi_import_bombastic PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(d110_midi_import_bombastic PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# The one path no earlier probe in this investigation tested: real MIDI input via
# osMidiCollector (injectTestNote), driven at genuine real-time cadence (Thread::sleep, not
# fast-forwarded rendering) - see the probe's own header comment for why this, not direct
# processBlock() buffer injection, is what Alan's actual keyboard/on-screen-keyboard usage
# goes through.
add_executable(d110_native_realtime_collector native_realtime_collector_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_realtime_collector PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_realtime_collector PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_realtime_collector PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Confirms the root cause found by diffing Alan's own "broken"/"fixed" snapshots: TimbreTemp
# field 5 (ASSIGN), never touched by the Parts tab's tone-picker. See the probe's own header
# comment.
add_executable(d110_native_assign_mode native_assign_mode_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_assign_mode PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_assign_mode PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_assign_mode PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Repro for silent rhythm channel (ch 10).
add_executable(d110_native_rhythm_silent native_rhythm_silent_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_rhythm_silent PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_rhythm_silent PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_rhythm_silent PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Verifies the 2026-08-07 fix mirroring masterTune to the sound engine. See the probe's own
# header comment.
add_executable(d110_native_master_tune native_master_tune_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_master_tune PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_master_tune PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_native_master_tune PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same shape as d110_native_plugin_smoke, but for the sequencer's getStateInformation/
# setStateInformation round trip - see that file's header comment.
add_executable(d110_sequencer_state_probe sequencer_state_probe.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp Source/UiTheme.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_sequencer_state_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sequencer_state_probe PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(d110_sequencer_state_probe PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# Same snapshot idea as d110_editor_shot (see that target's comment), but against the
# native core so it builds without D110_BUILD_MAME_BACKEND - useful for eyeballing LCD
# rendering changes (panel scale, dot antialiasing) without a full MAME configure.
add_executable(d110_native_editor_shot editor_shot.cpp
    Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/SoundbankDatabase.cpp Source/SoundbankBrowser.cpp Source/D110Keyboard.cpp
    Source/native/mcs96_cpu.cpp Source/native/d110_bus.cpp Source/native/msm6222b.cpp
    Source/native/D110CoreNative.cpp Source/native/ram_mirror.cpp
    Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp
    Source/UiTheme.cpp
    Source/sequencer/D110SequencerPanel.cpp
    Source/sequencer/D110SequencerRetroPanel.cpp
    Source/sequencer/D110SequencerGridPanel.cpp
    Source/DotMatrixFont.cpp)
target_include_directories(d110_native_editor_shot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_native_editor_shot PRIVATE
    D110_NATIVE_CORE=1
    JUCE_STANDALONE_APPLICATION=0 JUCE_MODULE_AVAILABLE_juce_audio_plugin_client=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    # See d110_editor_shot's own comment on this same define - render the full editor,
    # sequencer drawer included, not the trimmed VST3/AU layout.
    JucePlugin_Build_Standalone=1)
target_link_libraries(d110_native_editor_shot PRIVATE
    D110PanelData MT32Emu::mt32emu
    juce::juce_audio_utils juce::juce_audio_processors)

# The sequencer engine (Source/sequencer/) has no firmware/plugin dependency at all -
# it only knows MIDI channels, beats and sample counts - so its probe links nothing
# beyond the two JUCE modules it actually uses.
add_executable(d110_sequencer_probe sequencer_probe.cpp Source/sequencer/D110SequencerEngine.cpp
    Source/sequencer/D110SequencerSongsFile.cpp)
target_include_directories(d110_sequencer_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sequencer_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(d110_sequencer_probe PRIVATE juce::juce_audio_basics juce::juce_core)

# dropRogueInitialPitchBend() (Source/PitchBendGuard.h) - header-only, no firmware needed.
add_executable(d110_pitch_bend_guard_probe pitch_bend_guard_probe.cpp)
target_include_directories(d110_pitch_bend_guard_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_pitch_bend_guard_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(d110_pitch_bend_guard_probe PRIVATE juce::juce_audio_basics juce::juce_core)

# d110bank::Database on its own - see SoundbankDatabase.h's own comment for why it has no
# firmware/core dependency at all, matching this probe's own minimal link list.
add_executable(d110_soundbank_probe soundbank_probe.cpp Source/SoundbankDatabase.cpp)
target_include_directories(d110_soundbank_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_soundbank_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(d110_soundbank_probe PRIVATE juce::juce_audio_basics juce::juce_core)

add_executable(d110_sequencer_stray_channel_probe sequencer_stray_channel_probe.cpp
    Source/sequencer/D110SequencerEngine.cpp)
target_include_directories(d110_sequencer_stray_channel_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_sequencer_stray_channel_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(d110_sequencer_stray_channel_probe PRIVATE juce::juce_audio_basics juce::juce_core)

# Nonet Sequencer, the independent standalone sequencer app that used to be built from this
# repo (D110SequencerPanel/Engine plus a bare window and direct system MIDI In/Out, no
# firmware/ROMs/plugin wrapper at all), was extracted into its own repository 2026-09-22 -
# see github.com/luginf/nonet-sequencer (renamed there to drop the D110* prefix: Sequencer
# Engine/Panel/Keyboard instead of D110Sequencer*/D110Keyboard). NonetSeqMain.cpp and
# NonetSeqHost.h/.cpp, and the "Nonet-Seq"/"d110_keyboard_activity_probe" targets that built
# from them, no longer live here - this repo keeps only its OWN in-plugin sequencer drawer
# (D110SequencerPanel/Engine/RetroPanel/GridPanel below), unaffected by that split.

# --- D-50 (d50/) ------------------------------------------------------------
# Michi71's PicoFaceD5 engine (github.com/Michi71/PicoVintageSynthCollection),
# ported from a reimplemented RP2350 firmware, not real D-50 hardware - see
# d50/UPSTREAM_README.md for provenance and CLAUDE.md for what that means for
# this port. Needs a local ROM set (the two PCM ROMs + the program EPROM) that
# is not part of this repository; without one the probe is skipped and
# everyone else's build stays green, the same convention already used for the
# MAME backend/D-110 ROMs.
set(_d50_dir ${CMAKE_CURRENT_SOURCE_DIR}/../d50)
file(GLOB _d50_rom_files ${_d50_dir}/roms/*.bin ${_d50_dir}/roms/*.BIN)
if(_d50_rom_files)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_d50_gen ${CMAKE_CURRENT_BINARY_DIR}/d50_rom)
    set(_d50_pcm_table ${_d50_gen}/d5_pcm_table.h)
    set(_d50_tbl ${_d50_dir}/tools/d5_sample_table.json)
    set(_d50_make_blob ${_d50_dir}/tools/d5_make_blob.py)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 ${_d50_tbl} ${_d50_make_blob})
    # Rerun when the sample table or the converter changes, not only when the
    # output is missing - a build that silently kept a stale blob would send
    # the wrong sample geometry to a probe/plugin without any signal.
    if(EXISTS ${_d50_pcm_table} AND
       (${_d50_tbl} IS_NEWER_THAN ${_d50_pcm_table} OR
        ${_d50_make_blob} IS_NEWER_THAN ${_d50_pcm_table}))
        message(STATUS "D-50: sample table changed, reconverting")
        file(REMOVE ${_d50_pcm_table})
    endif()
    if(NOT EXISTS ${_d50_pcm_table})
        message(STATUS "D-50: converting ROM set (this takes a moment)")
        execute_process(
            COMMAND ${Python3_EXECUTABLE} ${_d50_make_blob} ${_d50_dir}/roms ${_d50_gen}
            RESULT_VARIABLE _d50_rc
            OUTPUT_VARIABLE _d50_out
            ERROR_VARIABLE _d50_err)
        if(NOT _d50_rc EQUAL 0)
            message(FATAL_ERROR "D-50: ROM conversion failed\n${_d50_out}${_d50_err}")
        endif()
        message(STATUS "D-50: ${_d50_out}")
    endif()

    # Patch bank(s): optional upstream, but the probe below (vendored as-is
    # from Michi71's own host harness) always includes the generated header,
    # so for now it only builds once a bank is there. The real plugin target
    # will fall back to d5_presets.h's 8 hand-built patches the way D5_Bridge
    # already does (see its __has_include("d5_patch_data.h") guard).
    file(GLOB _d50_syx ${_d50_dir}/roms/*.syx ${_d50_dir}/roms/*.SYX)
    set(_d50_patch_data ${_d50_gen}/d5_patch_data.h)
    if(_d50_syx)
        list(SORT _d50_syx)
        set(_d50_stale TRUE)
        if(EXISTS ${_d50_patch_data})
            set(_d50_stale FALSE)
            foreach(_f ${_d50_syx})
                if(${_f} IS_NEWER_THAN ${_d50_patch_data})
                    set(_d50_stale TRUE)
                endif()
            endforeach()
        endif()
        if(_d50_stale)
            message(STATUS "D-50: converting patch bank(s)")
            execute_process(
                COMMAND ${Python3_EXECUTABLE} ${_d50_dir}/tools/d5_syx_to_patches.py
                        ${_d50_syx} ${_d50_gen}
                RESULT_VARIABLE _d50_syx_rc
                OUTPUT_VARIABLE _d50_syx_out
                ERROR_VARIABLE _d50_syx_err)
            if(NOT _d50_syx_rc EQUAL 0)
                message(FATAL_ERROR "D-50: patch bank conversion failed\n${_d50_syx_out}${_d50_syx_err}")
            endif()
            message(STATUS "D-50: ${_d50_syx_out}")
        endif()
    endif()

    if(EXISTS ${_d50_patch_data})
        add_executable(d50_engine_probe d50_engine_probe.cpp)
        target_include_directories(d50_engine_probe PRIVATE ${_d50_dir} ${_d50_gen})
        target_compile_features(d50_engine_probe PRIVATE cxx_std_17)

        # D50Emulator: the first playable JUCE wrapper around the ported engine - see
        # plugin/Source/d50/D50AudioProcessor.h's own header comment. Gated on the same
        # ROMs+bank requirement as d50_engine_probe just above, for the same reason
        # (d5_engine/d5_patch_map.h needs d5_pcm_table.h either way) - D5_Bridge still
        # compiles whatever .syx is in d50/roms/ into d5_patch_data.h as a fallback (and
        # this target won't even exist without one, see the else() branch below), but
        # D5SyxLoader.cpp loading a real bank at runtime (see D5_Bridge::loadBank()) now
        # takes priority whenever one is found - a release build that ships no .syx at
        # all still plays real patches, from disk, same as the ROMs (d50/roms/README.md).
        juce_add_plugin(D50Emulator
            VERSION ${PROJECT_VERSION}
            COMPANY_NAME "DIY"
            IS_SYNTH TRUE
            NEEDS_MIDI_INPUT TRUE
            NEEDS_MIDI_OUTPUT FALSE
            IS_MIDI_EFFECT FALSE
            EDITOR_WANTS_KEYBOARD_FOCUS FALSE
            COPY_PLUGIN_AFTER_BUILD TRUE
            ${D110_VST3_COPY_DIR_ARGS}
            PLUGIN_MANUFACTURER_CODE Manu
            PLUGIN_CODE Dv50
            FORMATS ${D110_FORMATS}
            PRODUCT_NAME "D-50 Emulator"
        )

        target_sources(D50Emulator PRIVATE
            Source/d50/D50AudioProcessor.cpp
            Source/d50/D50Editor.cpp
            ../d50/D5_Bridge.cpp
            ../d50/D5RomLoader.cpp
            ../d50/D5SyxLoader.cpp
            Source/D110Keyboard.cpp
            Source/UiTheme.cpp
            Source/DotMatrixFont.cpp
            Source/sequencer/D110SequencerEngine.cpp
            Source/sequencer/D110SequencerSongsFile.cpp
            Source/sequencer/D110SequencerPanel.cpp
        )
        target_include_directories(D50Emulator PRIVATE ${_d50_dir} ${_d50_gen})
        target_compile_definitions(D50Emulator PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            JUCE_DISPLAY_SPLASH_SCREEN=0
            JUCE_VST3_EMULATE_MIDI_CC_WITH_PARAMETERS=0
        )
        target_link_libraries(D50Emulator PRIVATE
            juce::juce_audio_utils
            juce::juce_audio_processors
        )
        target_link_libraries(D50Emulator PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags
        )

        # loadPcmBlob()/loadPatchBank() look for raw ROM dumps and .syx banks in a
        # d50-roms/ folder beside the running binary first (see their own comments) -
        # copying Alan's own raw files there (never a pre-converted blob or the compiled-
        # in bank) after every build exercises the SAME path a real end user's own ROM/
        # bank would take, not a shortcut only his own dev setup gets.
        if(TARGET D50Emulator_Standalone)
            # Wiped and recreated every build, not just copy_if_different'd into - a
            # dropped or swapped .syx (Alan, 2026-09-02: replacing a bogus bank with
            # the real PN-D50-00 factory card) must not leave the OLD one sitting next
            # to the new one, where loadBankFromSyxFolder()'s alphabetically-first-file
            # pick could silently keep loading the stale one forever.
            add_custom_command(TARGET D50Emulator_Standalone POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E remove_directory
                        $<TARGET_FILE_DIR:D50Emulator_Standalone>/d50-roms
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        $<TARGET_FILE_DIR:D50Emulator_Standalone>/d50-roms
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_d50_rom_files}
                        $<TARGET_FILE_DIR:D50Emulator_Standalone>/d50-roms/)
            if(_d50_syx)
                add_custom_command(TARGET D50Emulator_Standalone POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_d50_syx}
                            $<TARGET_FILE_DIR:D50Emulator_Standalone>/d50-roms/)
            endif()
        endif()

        # Offline check for D50AudioProcessor itself (MIDI mapping, resampling) - same
        # reasoning as d110_audio_test.cpp, see d50_processor_probe.cpp's own header
        # comment for why this is more trustworthy here than a Standalone window under Xvfb.
        add_executable(d50_processor_probe d50_processor_probe.cpp
            Source/d50/D50AudioProcessor.cpp
            Source/d50/D50Editor.cpp
            ../d50/D5_Bridge.cpp
            ../d50/D5RomLoader.cpp
            ../d50/D5SyxLoader.cpp
            Source/D110Keyboard.cpp
            Source/UiTheme.cpp
            Source/DotMatrixFont.cpp
            Source/sequencer/D110SequencerEngine.cpp
            Source/sequencer/D110SequencerSongsFile.cpp
            Source/sequencer/D110SequencerPanel.cpp)
        target_include_directories(d50_processor_probe PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}" ${_d50_dir} ${_d50_gen})
        target_compile_definitions(d50_processor_probe PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
        target_link_libraries(d50_processor_probe PRIVATE
            juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_formats)

        # Same "beside the binary" convenience as the plugin's own POST_BUILD step, so this
        # probe is runnable straight after a build with no manual file copying either -
        # wiped and recreated every build for the same reason (see that step's own comment).
        add_custom_command(TARGET d50_processor_probe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove_directory
                    $<TARGET_FILE_DIR:d50_processor_probe>/d50-roms
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    $<TARGET_FILE_DIR:d50_processor_probe>/d50-roms
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_d50_rom_files}
                    $<TARGET_FILE_DIR:d50_processor_probe>/d50-roms/)
        if(_d50_syx)
            add_custom_command(TARGET d50_processor_probe POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_d50_syx}
                        $<TARGET_FILE_DIR:d50_processor_probe>/d50-roms/)
        endif()
    else()
        message(STATUS
            "D-50: no patch bank (*.syx) in ${_d50_dir}/roms - skipping d50_engine_probe "
            "for now (needs one to build; put a D-50 SysEx bulk dump there)")
    endif()
else()
    message(STATUS
        "D-50: skipped - no ROM images in ${_d50_dir}/roms "
        "(needs the two PCM ROMs and a program EPROM; see d50/UPSTREAM_README.md)")
endif()

if(D110_BUILD_MAME_BACKEND)

# One-shot MAME-backed helper: produces a properly factory-reset nvram folder for
# native_note_probe.cpp to load (see that file's header comment).
add_executable(d110_mame_factory_reset_tool mame_factory_reset_tool.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_factory_reset_tool PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_factory_reset_tool PRIVATE ${MAME_DEFS})
target_link_libraries(d110_mame_factory_reset_tool PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_mame_factory_reset_tool PRIVATE ${MAME_LINK_OPTIONS})

# Diagnostic: same raw pushMidi()->popNoteEvent() experiment as native_note_probe.cpp, but
# against the real MAME-backed core directly - isolates a native-core bug from a property of
# the raw D110Core interface itself.
add_executable(d110_mame_note_probe mame_note_probe.cpp Source/D110Core.cpp "${MAME_DRIVLIST}")
target_include_directories(d110_mame_note_probe PRIVATE ${MAME_INCLUDES} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(d110_mame_note_probe PRIVATE ${MAME_DEFS})
target_link_libraries(d110_mame_note_probe PRIVATE ${MAME_LIBS} ${MAME_SYS_LIBS})
target_link_options(d110_mame_note_probe PRIVATE ${MAME_LINK_OPTIONS})

endif() # D110_BUILD_MAME_BACKEND

# ROM-free headless probes, run by CI (.github/workflows/headless-probes.yml) via ctest.
enable_testing()
foreach(_t d110_sequencer_probe d110_soundbank_probe d110_pitch_bend_guard_probe)
    if(TARGET ${_t})
        add_test(NAME ${_t} COMMAND ${_t})
    endif()
endforeach()
