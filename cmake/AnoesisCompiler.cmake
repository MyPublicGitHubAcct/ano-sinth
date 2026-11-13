# Compiler policy: which compiler each platform uses, warning flags, and
# sanitizers. Rules: CLAUDE.md "Tech stack" and "Conventions".

include_guard(GLOBAL)

# Each platform builds with its standard compiler: Apple Clang on macOS, GCC on
# Linux, MSVC on Windows. Anything else fails configure unless
# BRUTALIS_ALLOW_ANY_COMPILER is ON (useful for one-off experiments only).
function(anoesis_check_platform_compiler)
    if(APPLE)
        set(expected "AppleClang")
    elseif(WIN32)
        set(expected "MSVC")
    elseif(UNIX)
        set(expected "GNU")
    else()
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL expected)
        set(msg "Expected the ${expected} compiler on this platform, found "
                "${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}).")
        if(BRUTALIS_ALLOW_ANY_COMPILER)
            message(WARNING ${msg})
        else()
            message(FATAL_ERROR ${msg} " Set BRUTALIS_ALLOW_ANY_COMPILER=ON to override.")
        endif()
    endif()
endfunction()

# Strict warnings for project code. Applied per target, never globally, so
# third-party code (JUCE, Catch2) keeps its own settings.
#
# Beyond -Wall -Wextra -Wpedantic, GCC and Clang get the numeric warnings that
# matter most in DSP code. JUCE targets also get these through
# juce_recommended_warning_flags, but targets without JUCE (libs/dsp and its
# tests) need them spelled out here:
#   -Wfloat-equal       `x == 0.0f` must be written as an explicit comparison
#   -Wconversion        no silent double -> float or float -> int narrowing
#   -Wsign-conversion   no silent signed <-> unsigned (index arithmetic)
#   -Wshadow            no local that hides a member or outer variable
# MSVC's /W4 already covers the conversion warnings (C4244, C4267, C4456-8).
#
# Targets that compile JUCE module sources (the plugin and the JUCE-based test
# runners) also compile those sources with these flags. JUCE headers are
# included as ordinary (non-system) headers, so they are held to the same
# standard as our code.
function(anoesis_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(BRUTALIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wfloat-equal -Wconversion -Wsign-conversion -Wshadow)
        if(BRUTALIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Extra warnings for libs/dsp only. -Wdouble-promotion flags every implicit
# float -> double promotion: on a Cortex-M4F (single-precision FPU only) each
# one becomes a slow software double operation. Test code is exempt, because
# Catch2's matchers take double arguments.
function(anoesis_set_dsp_warnings target)
    anoesis_set_warnings(${target})
    if(NOT MSVC)
        target_compile_options(${target} PRIVATE -Wdouble-promotion)
    endif()
endfunction()

# ASan + UBSan for the whole build (including JUCE and Catch2), so that
# instrumented and uninstrumented code are never mixed.
macro(anoesis_enable_sanitizers)
    if(MSVC)
        message(FATAL_ERROR "BRUTALIS_SANITIZERS supports Clang and GCC only.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endmacro()
