# Tests for plugin/. The runner compiles the plugin's sources itself (through
# brutalis_plugin_common) rather than linking the plugin's shared-code
# library, so the tests exercise exactly the same code without format wrappers.
juce_add_console_app(brutalis_plugin_tests)
target_sources(brutalis_plugin_tests PRIVATE
    plugin_processor_test.cpp)

target_link_libraries(brutalis_plugin_tests PRIVATE brutalis_plugin_common Catch2::Catch2WithMain)
anoesis_configure_juce_target(brutalis_plugin_tests)
anoesis_register_tests(brutalis_plugin_tests)
