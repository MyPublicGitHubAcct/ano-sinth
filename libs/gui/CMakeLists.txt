# anoesis::anoesis_gui: reusable JUCE UI components. Knows nothing about
# Brutalis's parameters; it gets what it needs through configuration.
#
# Packaged as a JUCE module rather than a static library: JUCE module sources
# must be compiled exactly once per final binary, and the plugin target always
# compiles them. A module's sources are compiled into whichever binary links it,
# so there is never a second copy. Link it PRIVATE only (see JUCE's CMake API
# docs on juce_add_module).
juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/anoesis_gui ALIAS_NAMESPACE anoesis)
