# The Brutalis plugin: the only product-specific code. It wires the libraries
# together and owns the identity (names, codes, bundle ID).

# Public product info for C++ code. Holds only the public values; the private
# codes and bundle ID go to JUCE below and never into a header.
configure_file(brutalis_info.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/brutalis_info.hpp" @ONLY)

# Everything the plugin and its tests share: sources, include paths, and
# library links. Both the plugin and the test runner are final binaries, so
# each compiles these sources (and JUCE) once. Nothing else may link this.
add_library(brutalis_plugin_common INTERFACE)
target_sources(brutalis_plugin_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/source/plugin_processor.cpp)
target_include_directories(brutalis_plugin_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(brutalis_plugin_common INTERFACE
    anoesis::dsp
    anoesis::anoesis_gui
    anoesis::anoesis_interop
    juce::juce_audio_utils)

# AU exists only on macOS.
set(brutalis_formats VST3 Standalone)
if(APPLE)
    list(APPEND brutalis_formats AU)
endif()

juce_add_plugin(brutalis
    PRODUCT_NAME             "${BRUTALIS_PRODUCT_NAME}"
    COMPANY_NAME             "${BRUTALIS_COMPANY_NAME}"
    BUNDLE_ID                "${BRUTALIS_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE "${BRUTALIS_MANUFACTURER_CODE}"
    PLUGIN_CODE              "${BRUTALIS_PLUGIN_CODE}"
    VERSION                  "${PROJECT_VERSION}"
    FORMATS                  ${brutalis_formats}
    IS_SYNTH                 TRUE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE
    NEEDS_WEB_BROWSER        FALSE
    NEEDS_CURL               FALSE)

target_link_libraries(brutalis PRIVATE brutalis_plugin_common)
anoesis_configure_juce_target(brutalis)

# Sanitizer builds leave the plugin out of the default build. No host can load
# an ASan-instrumented plugin, so pluginval and auval are skipped there, and
# brutalis_plugin_tests already compiles the plugin's sources (through
# brutalis_plugin_common) under the sanitizers. Building the shared code and
# the format wrappers would only compile JUCE once more, under the slowest
# flags, for nothing. They can still be built by name.
if(BRUTALIS_SANITIZERS)
    get_target_property(brutalis_format_targets brutalis JUCE_ACTIVE_PLUGIN_TARGETS)
    set_target_properties(brutalis ${brutalis_format_targets} PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()
