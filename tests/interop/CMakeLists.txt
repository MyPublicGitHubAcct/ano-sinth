# Tests for libs/interop. The runner is a JUCE console app because it compiles
# the JUCE module (and the JUCE modules it depends on) into itself.
juce_add_console_app(anoesis_interop_tests)
target_sources(anoesis_interop_tests PRIVATE
    library_test.cpp)

target_link_libraries(anoesis_interop_tests PRIVATE anoesis::anoesis_interop Catch2::Catch2WithMain)
anoesis_configure_juce_target(anoesis_interop_tests)
anoesis_register_tests(anoesis_interop_tests)
