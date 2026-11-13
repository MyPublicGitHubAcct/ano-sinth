# anoesis_test_support: measurement and fixture code shared by the DSP tests
# (buffers, lifecycle contract checks, and later the signal generators, FFT,
# spectral measurements, and golden-file comparison).
#
# It lives under tests/, never in libs/dsp: it may allocate, use double
# precision freely, and read and write files. Like libs/dsp it has no JUCE
# dependency, so it can test the DSP library on its own.

add_library(anoesis_test_support STATIC
    src/buffer.cpp
    src/lifecycle.cpp)
add_library(anoesis::test_support ALIAS anoesis_test_support)

target_include_directories(anoesis_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(anoesis_test_support PUBLIC anoesis::dsp)
anoesis_set_warnings(anoesis_test_support)

# The support code has its own tests: a measurement that is wrong makes every
# test built on it meaningless.
add_executable(anoesis_test_support_tests
    test/lifecycle_test.cpp)
target_link_libraries(anoesis_test_support_tests PRIVATE anoesis::test_support Catch2::Catch2WithMain)
anoesis_set_warnings(anoesis_test_support_tests)
anoesis_register_tests(anoesis_test_support_tests)
