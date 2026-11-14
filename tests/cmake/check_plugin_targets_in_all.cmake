# CTest check: the plugin targets are in the default build exactly when they
# can be used.
#
#   cmake -DEXPECT_EXCLUDED=<0|1> -DTARGET_STATES=<t1>=<0|1>,<t2>=<0|1>,...
#         -P check_plugin_targets_in_all.cmake
#
# TARGET_STATES gives each plugin target's EXCLUDE_FROM_ALL value. Sanitizer
# builds leave the plugin (shared code and every format wrapper) out of the
# default build: no host can load an ASan-instrumented plugin, so pluginval and
# auval are skipped there, and brutalis_plugin_tests already compiles the
# plugin's own sources under the sanitizers. Every other build must build them.

# Script mode (-P) starts with no policies set; see check_benchmarks_hidden.cmake.
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED EXPECT_EXCLUDED OR NOT DEFINED TARGET_STATES OR TARGET_STATES STREQUAL "")
    message(FATAL_ERROR "Pass -DEXPECT_EXCLUDED=<0|1> and -DTARGET_STATES=<target>=<0|1>,...")
endif()

string(REPLACE "," ";" states "${TARGET_STATES}")
set(wrong "")
foreach(state IN LISTS states)
    string(REGEX MATCH "^([^=]+)=([01])$" matched "${state}")
    if(NOT matched)
        message(FATAL_ERROR "Malformed target state '${state}'")
    endif()
    if(NOT CMAKE_MATCH_2 STREQUAL EXPECT_EXCLUDED)
        list(APPEND wrong "${CMAKE_MATCH_1}")
    endif()
endforeach()

if(EXPECT_EXCLUDED)
    set(expected "left out of the default build (sanitizer build)")
else()
    set(expected "in the default build")
endif()

if(wrong)
    list(JOIN wrong ", " wrong)
    message(FATAL_ERROR "Expected these plugin targets to be ${expected}: ${wrong}")
endif()
list(LENGTH states count)
message(STATUS "${count} plugin target(s), all ${expected}")
