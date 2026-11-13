# Brutalis: top-level build.
#
# Layout and dependency rules: docs/development/project-structure.md
# Build instructions:          docs/development/building.md

cmake_minimum_required(VERSION 3.25)

# --- Platform baseline (decision D8) -----------------------------------------
# Must be set before project() so the compiler checks use it. Ignored off Apple.
set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(Brutalis
    VERSION 0.1.0
    DESCRIPTION "Virtual analog synthesizer by Anoesis Audio"
    LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# --- Options ------------------------------------------------------------------
option(BRUTALIS_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors in project code" ON)
option(BRUTALIS_SANITIZERS           "Build everything with ASan + UBSan (Clang/GCC only)" OFF)
option(BRUTALIS_BUILD_TESTS          "Build the Catch2 test targets" ON)
option(BRUTALIS_BUILD_PLUGIN         "Build the plugin target (needs JUCE)" ON)
option(BRUTALIS_REQUIRE_REAL_IDENTITY "Fail configure if any placeholder identity value is in use (release builds)" OFF)
option(BRUTALIS_ALLOW_ANY_COMPILER   "Allow a compiler other than the platform's standard one" OFF)
option(BRUTALIS_PLUGINVAL            "Validate the built plugin with pluginval as a CTest test" ON)
option(BRUTALIS_AUVAL               "Validate the built AU with auval as a CTest test (macOS; installs it temporarily)" ON)
set(BRUTALIS_PLUGINVAL_EXECUTABLE "" CACHE FILEPATH "Use this pluginval binary instead of downloading the pinned release")

# --- Language standard --------------------------------------------------------
# The highest standard JUCE and Catch2 both support on every target compiler.
# See docs/development/building.md#language-standard before changing it.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(AnoesisCompiler)
include(AnoesisJuce)
include(AnoesisPluginval)
anoesis_check_platform_compiler()

if(BRUTALIS_SANITIZERS)
    anoesis_enable_sanitizers()
endif()

# --- Product identity ---------------------------------------------------------
include(BrutalisIdentity)

# --- Libraries ----------------------------------------------------------------
# libs/dsp is pure C++ with no JUCE dependency. It is added before JUCE so it
# cannot accidentally pick anything up from it.
add_subdirectory(libs/dsp)

if(BRUTALIS_BUILD_PLUGIN)
    add_subdirectory(external/JUCE EXCLUDE_FROM_ALL)
    add_subdirectory(libs/gui)
    add_subdirectory(libs/interop)
    add_subdirectory(plugin)
endif()

# --- Tests --------------------------------------------------------------------
if(BRUTALIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(external/Catch2 EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2/extras")
    add_subdirectory(tests)
endif()
