# anoesis::dsp: portable DSP library. MIT-licensed (see LICENSE in this folder).
#
# Rules (CLAUDE.md "Architecture and modularity"): no JUCE, no dependency on
# the other libs, no heap allocation after prepare(), no exceptions or RTTI
# needed, no OS/thread/file/log calls. It must stay buildable for bare-metal
# ARM, so it only uses the C++ standard library.

add_library(anoesis_dsp STATIC
    src/denormals.cpp
    src/library.cpp
    src/public_headers.cpp)
add_library(anoesis::dsp ALIAS anoesis_dsp)

target_include_directories(anoesis_dsp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(anoesis_dsp PUBLIC cxx_std_23)
anoesis_set_dsp_warnings(anoesis_dsp)
