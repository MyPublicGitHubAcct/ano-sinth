# Validates the built Audio Unit with Apple's auval (-strict). macOS only.
# auval only sees installed AUs, so tools/auval-test.sh installs the component
# into ~/Library/Audio/Plug-Ins/Components for the duration of the test and
# always removes it afterwards (restoring any copy that was there before).
# See docs/development/plugin-validation.md.
#
# Skipped in sanitizer builds: an ASan-instrumented plugin can't be loaded into
# a host that wasn't built with ASan.

if(BRUTALIS_SANITIZERS)
    message(STATUS "auval: skipped (sanitizer build)")
    return()
endif()

# Brutalis is an instrument, so its AU type is 'aumu' (kAudioUnitType_MusicDevice).
set(au_type aumu)

add_test(NAME "auval:AU"
    COMMAND "${PROJECT_SOURCE_DIR}/tools/auval-test.sh"
        # JUCE stores the bundle path as a generator expression; GENEX_EVAL
        # expands it fully.
        "$<GENEX_EVAL:$<TARGET_PROPERTY:brutalis_AU,JUCE_PLUGIN_ARTEFACT_FILE>>"
        ${au_type})
set_tests_properties("auval:AU" PROPERTIES
    # The codes go in the environment rather than on the command line, and the
    # script masks them in auval's output.
    ENVIRONMENT "BRUTALIS_AU_MANUFACTURER=${BRUTALIS_MANUFACTURER_CODE};BRUTALIS_AU_SUBTYPE=${BRUTALIS_PLUGIN_CODE}"
    # It installs into a shared per-user folder, so it must never run in
    # parallel with another test that does.
    RESOURCE_LOCK au-components
    RUN_SERIAL TRUE
    TIMEOUT 300
    LABELS "auval")
