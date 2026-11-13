# Tests for libs/gui. The runner is a JUCE console app because it compiles
# the JUCE module (and the JUCE modules it depends on) into itself.
juce_add_console_app(anoesis_gui_tests)
target_sources(anoesis_gui_tests PRIVATE
    library_test.cpp)

target_link_libraries(anoesis_gui_tests PRIVATE anoesis::anoesis_gui Catch2::Catch2WithMain)
anoesis_configure_juce_target(anoesis_gui_tests)
anoesis_register_tests(anoesis_gui_tests)
