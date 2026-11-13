# Validates the built VST3 with pluginval at the strictest level (10). Runs as
# part of ctest after the plugin is built. See cmake/AnoesisPluginval.cmake and
# docs/development/plugin-validation.md.
#
# Skipped in sanitizer builds: an ASan-instrumented plugin can't be loaded into
# a host that wasn't built with ASan.
#
# AU is not validated here. macOS only finds AUs that are installed in a
# Components folder, and the build doesn't install anything. That is a
# separate auval step (M0.6/M1.12).

if(BRUTALIS_SANITIZERS)
    message(STATUS "pluginval: skipped (sanitizer build)")
    return()
endif()

anoesis_get_pluginval(pluginval_exe)

set(BRUTALIS_PLUGINVAL_STRICTNESS 10 CACHE STRING "pluginval strictness level (1-10)")

add_test(NAME "pluginval:VST3"
    COMMAND "${pluginval_exe}"
        --strictness-level ${BRUTALIS_PLUGINVAL_STRICTNESS}
        --output-dir "${CMAKE_CURRENT_BINARY_DIR}/logs"
        # JUCE stores the bundle path as a generator expression; GENEX_EVAL
        # expands it fully.
        --validate "$<GENEX_EVAL:$<TARGET_PROPERTY:brutalis_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")
set_tests_properties("pluginval:VST3" PROPERTIES
    TIMEOUT 600
    LABELS "pluginval")
