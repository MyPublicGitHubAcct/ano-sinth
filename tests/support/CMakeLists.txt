# anoesis_test_support: measurement and fixture code shared by the DSP tests
# (buffers, lifecycle contract checks, and later the signal generators, FFT,
# spectral measurements, and golden-file comparison).
#
# It lives under tests/, never in libs/dsp: it may allocate, use double
# precision freely, and read and write files. Like libs/dsp it has no JUCE
# dependency, so it can test the DSP library on its own.

add_library(anoesis_test_support STATIC
    src/buffer.cpp
    src/fft.cpp
    src/golden.cpp
    src/lifecycle.cpp
    src/measure.cpp
    src/signals.cpp
    src/spectrum.cpp
    src/window.cpp)
add_library(anoesis::test_support ALIAS anoesis_test_support)

target_include_directories(anoesis_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anoesis_test_support PUBLIC anoesis::dsp)
# Golden files are committed under tests/golden/. Failed renders go to the
# build tree, never the source tree.
target_compile_definitions(anoesis_test_support PRIVATE
    ANOESIS_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden"
    ANOESIS_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output")
anoesis_set_warnings(anoesis_test_support)

# The support code has its own tests: a measurement that is wrong makes every
# test built on it meaningless.
add_executable(anoesis_test_support_tests
    test/fft_test.cpp
    test/golden_test.cpp
    test/lifecycle_test.cpp
    test/measure_test.cpp
    test/spectrum_test.cpp)
target_link_libraries(anoesis_test_support_tests PRIVATE anoesis::test_support Catch2::Catch2WithMain)
anoesis_set_warnings(anoesis_test_support_tests)
anoesis_register_tests(anoesis_test_support_tests)
