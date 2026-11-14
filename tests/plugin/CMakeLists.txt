# Tests for plugin/. The runner compiles the plugin's sources itself (through
# brutalis_plugin_common) rather than linking the plugin's shared-code
# library, so the tests exercise exactly the same code without format wrappers.
juce_add_console_app(brutalis_plugin_tests)
target_sources(brutalis_plugin_tests PRIVATE
    plugin_processor_test.cpp)

target_link_libraries(brutalis_plugin_tests PRIVATE brutalis_plugin_common Catch2::Catch2WithMain)
anoesis_configure_juce_target(brutalis_plugin_tests)
anoesis_register_tests(brutalis_plugin_tests)

# The plugin targets (shared code plus every format wrapper) are in the default
# build, except in sanitizer builds, where nothing can load them
# (plugin/CMakeLists.txt).
get_target_property(brutalis_format_targets brutalis JUCE_ACTIVE_PLUGIN_TARGETS)
set(brutalis_target_states "")
foreach(plugin_target IN ITEMS brutalis ${brutalis_format_targets})
    list(APPEND brutalis_target_states
        "${plugin_target}=$<BOOL:$<TARGET_PROPERTY:${plugin_target},EXCLUDE_FROM_ALL>>")
endforeach()
list(JOIN brutalis_target_states "," brutalis_target_states)
add_test(NAME "brutalis_plugin_tests:plugin targets are built unless sanitizing"
    COMMAND ${CMAKE_COMMAND}
            -DEXPECT_EXCLUDED=$<BOOL:${BRUTALIS_SANITIZERS}>
            -DTARGET_STATES=${brutalis_target_states}
            -P ${PROJECT_SOURCE_DIR}/tests/cmake/check_plugin_targets_in_all.cmake)
