# Catch2 test targets. The layout mirrors libs/ and plugin/: one test
# executable per library, each registered with CTest.
#
# How TDD works here: docs/development/testing.md

include(Catch)

# Registers a test executable's Catch2 test cases with CTest. Discovery runs
# when the tests run (PRE_TEST), not at build time, so a cross-compiled or
# universal binary never has to run during the build.
function(anoesis_register_tests target)
    catch_discover_tests(${target}
        DISCOVERY_MODE PRE_TEST
        TEST_PREFIX "${target}:"
        PROPERTIES TIMEOUT 120)
endfunction()

add_subdirectory(support)
add_subdirectory(dsp)

if(BRUTALIS_BUILD_PLUGIN)
    add_subdirectory(gui)
    add_subdirectory(interop)
    add_subdirectory(plugin)
    if(BRUTALIS_PLUGINVAL)
        add_subdirectory(pluginval)
    endif()
    if(BRUTALIS_AUVAL AND APPLE)
        add_subdirectory(auval)
    endif()
endif()
