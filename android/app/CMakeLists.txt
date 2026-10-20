cmake_minimum_required(VERSION 3.22)
project(D110Android VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Same JUCE tag the desktop build fetches (plugin/CMakeLists.txt) - already cloned to
# android/juce-src by settings.gradle before Gradle even gets this far, since the Java
# side (app/build.gradle's sourceSets) needs those same sources on disk too.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../juce-src" "${CMAKE_CURRENT_BINARY_DIR}/juce")

# The LA32 sound engine (vendored mt32emu fork) - pure portable C++, no platform-specific
# code, same static-library setup plugin/CMakeLists.txt uses for the desktop build.
set(libmt32emu_SHARED FALSE CACHE BOOL "" FORCE)
set(libmt32emu_C_INTERFACE FALSE CACHE BOOL "" FORCE)
set(libmt32emu_CPP_INTERFACE TRUE CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../munt/mt32emu" "${CMAKE_CURRENT_BINARY_DIR}/mt32emu_build")

# The photographed front-panel image + memory card art the real D110Panel/D110MemoryCard
# classes (reused as-is below) paint - same sources plugin/CMakeLists.txt embeds.
juce_add_binary_data(D110PanelData
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/panel_reference.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/panel_reference_compact.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/memory_card_m256d.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/D20infos.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/volume_knob_disc.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/panel_reference_alt_256.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/panel_reference_compact_alt_256.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/volume_knob_alt_disc.png")

juce_add_gui_app(D110Android
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "DIY"
    PRODUCT_NAME "D-110 Emulator"
)

# _juce_set_output_name (JUCEUtils.cmake) deliberately no-ops on Android, so PRODUCT_NAME
# above never reaches the actual .so filename there - has to be set directly instead. JUCE's
# generic Android Java glue (com.rmsl.juce.Java, see app/build.gradle's sourceSets comment)
# hardcodes System.loadLibrary("juce_jni"): the produced library has to be named to match,
# or it dlopen()s a name nothing on disk has (confirmed empirically: UnsatisfiedLinkError,
# "library "libjuce_jni.so" not found", the very first real-device run of this project).
set_target_properties(D110Android PROPERTIES OUTPUT_NAME "juce_jni")

# NDK r27's linker (lld) does not yet default to 16 KB ELF segment alignment (that only
# became the default starting with NDK r28) - without this flag libjuce_jni.so itself is
# 4 KB-aligned and trips Android's 16 KB page-size compatibility warning, same as the
# prebuilt libc++_shared.so worked around via ANDROID_STL=c++_static in app/build.gradle.
if(ANDROID)
    target_link_options(D110Android PRIVATE "-Wl,-z,max-page-size=16384")
endif()

set(D110_PLUGIN_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../plugin")

# The real D110AudioProcessor/D110Panel/D110Keyboard, unchanged - same file list
# plugin/CMakeLists.txt's own d110_native_plugin_smoke probe already compiles standalone
# (outside any plugin format), proving these files have no VST3/AU-only dependency. The
# sequencer engine files are pulled in because D110AudioProcessor owns a sequencer instance
# unconditionally (see plugin/CLAUDE.md) - not because this app's UI exposes it; it doesn't
# (no D110SequencerPanel is ever instantiated below).
target_sources(D110Android PRIVATE
    src/main/cpp/Main.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/PluginProcessor.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/PluginEditor.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/D110Keyboard.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/native/mcs96_cpu.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/native/d110_bus.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/native/msm6222b.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/native/D110CoreNative.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/native/ram_mirror.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/sequencer/D110SequencerEngine.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/sequencer/D110SequencerSongsFile.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/UiTheme.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/SoundbankDatabase.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/SoundbankBrowser.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/sequencer/D110SequencerPanel.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/sequencer/D110SequencerRetroPanel.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/sequencer/D110SequencerGridPanel.cpp
    ${D110_PLUGIN_SOURCE_DIR}/Source/DotMatrixFont.cpp
)

target_include_directories(D110Android PRIVATE ${D110_PLUGIN_SOURCE_DIR})

target_compile_definitions(D110Android PRIVATE
    D110_NATIVE_CORE=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Not about push notifications at all here: this macro is also what activates
    # juce_Windowing_android.cpp's ONLY implementation of appOnResume()/appNewIntent(), two
    # native methods JuceActivity.java calls UNCONDITIONALLY from onResume()/onNewIntent()
    # regardless of whether the app uses push notifications. Without it those two native
    # methods have no implementation anywhere in the binary and the app crashes with
    # UnsatisfiedLinkError the first time onResume() runs (confirmed empirically, real device).
    # JUCE_PUSH_NOTIFICATIONS=1 has to come with it: the two functions those callbacks call
    # (juce_handleOnResume/juce_handleNotificationIntent) only exist in
    # juce_PushNotifications_android.cpp, which juce_gui_extra.cpp only compiles in under
    # this flag - no Firebase/FCM dependency involved, it's a self-contained JUCE source file.
    JUCE_PUSH_NOTIFICATIONS_ACTIVITY="com/rmsl/juce/JuceActivity"
    JUCE_PUSH_NOTIFICATIONS=1
)

target_link_libraries(D110Android PRIVATE
    D110PanelData
    MT32Emu::mt32emu
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_extra
)

target_link_libraries(D110Android PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
