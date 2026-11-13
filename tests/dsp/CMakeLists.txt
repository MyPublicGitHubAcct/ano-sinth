# Tests for libs/dsp. Links Catch2 only: no JUCE, mirroring the library.
add_executable(anoesis_dsp_tests
    library_test.cpp)

target_link_libraries(anoesis_dsp_tests PRIVATE anoesis::dsp Catch2::Catch2WithMain)
anoesis_set_warnings(anoesis_dsp_tests)
anoesis_register_tests(anoesis_dsp_tests)
