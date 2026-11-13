# Shared JUCE settings for every target that compiles JUCE module sources (the
# plugin and the JUCE-based test runners). Each final binary compiles JUCE
# once, so these settings must be identical across all of them.

include_guard(GLOBAL)

function(anoesis_configure_juce_target target)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1)
    target_link_libraries(${target} PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    anoesis_set_warnings(${target})
endfunction()
