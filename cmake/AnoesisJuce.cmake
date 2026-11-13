# Shared JUCE settings for every target that compiles JUCE module sources (the
# plugin and the JUCE-based test runners). Each final binary compiles JUCE
# once, so these settings must be identical across all of them.

include_guard(GLOBAL)

function(anoesis_configure_juce_target target)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)
    target_link_libraries(${target} PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    anoesis_set_warnings(${target})
    _anoesis_exempt_juce_units_from_gcc_conversion(${target})
endfunction()

# GCC only: compile JUCE's own module units without -Wconversion.
#
# anoesis_set_warnings adds -Wconversion for our code. On Clang, JUCE's
# recommended flags include it too, and JUCE compiles cleanly with it. On GCC,
# JUCE's recommended flags leave it out, and JUCE does not compile cleanly with
# it: GCC's -Wconversion also flags int -> char/uint8_t in shifts and masks and
# int -> float in arithmetic, which Clang does not. Our own sources (the plugin,
# the anoesis_* modules, tests) keep the full set. Every other warning applies
# to JUCE as before. The per-source option comes after the target options on the
# command line, so the -Wno- wins.
#
# JUCE's compilation units are the .cpp and .c files at the root of each module
# (modules/<module>/<module>[_N].cpp, plus .c units wrapping bundled C libraries
# such as libjpeg, FLAC, and zlib). Everything else is #included from those.
function(_anoesis_exempt_juce_units_from_gcc_conversion target)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        return()
    endif()
    file(GLOB juce_units
        "${PROJECT_SOURCE_DIR}/external/JUCE/modules/*/*.cpp"
        "${PROJECT_SOURCE_DIR}/external/JUCE/modules/*/*.c")
    set_source_files_properties(${juce_units}
        TARGET_DIRECTORY ${target}
        PROPERTIES COMPILE_OPTIONS -Wno-conversion)
endfunction()
