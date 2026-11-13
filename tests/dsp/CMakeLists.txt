# Tests for libs/dsp. No JUCE, mirroring the library.
add_executable(anoesis_dsp_tests
    library_test.cpp
    core/audio_block_test.cpp
    core/denormals_test.cpp
    core/processor_test.cpp
    control/smoother_test.cpp
    math/decibels_test.cpp
    math/fast_math_test.cpp
    math/pitch_test.cpp)

# anoesis::dsp comes in through anoesis::test_support. Naming it here as well
# makes Apple's linker warn about a duplicate library.
target_link_libraries(anoesis_dsp_tests PRIVATE anoesis::test_support Catch2::Catch2WithMain)
anoesis_set_warnings(anoesis_dsp_tests)
anoesis_register_tests(anoesis_dsp_tests)
