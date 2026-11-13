# anoesis::anoesis_interop: reusable glue between JUCE and a synth engine
# (parameter binding, preset/bank I/O, MIDI learn, host sync). Knows nothing
# about Brutalis's parameters.
#
# Packaged as a JUCE module for the same reason as libs/gui; see its
# CMakeLists.txt. Link it PRIVATE only.
juce_add_module(${CMAKE_CURRENT_SOURCE_DIR}/anoesis_interop ALIAS_NAMESPACE anoesis)
