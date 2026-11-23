# Tests for libs/dsp. No JUCE, mirroring the library.
add_executable(anoesis_dsp_tests
    library_test.cpp
    benchmarks/amp_link_benchmark.cpp
    benchmarks/destination_benchmark.cpp
    benchmarks/foundations_benchmark.cpp
    benchmarks/drift_benchmark.cpp
    benchmarks/envelope_benchmark.cpp
    benchmarks/filter_benchmark.cpp
    benchmarks/filter_control_benchmark.cpp
    benchmarks/filter_stability_benchmark.cpp
    benchmarks/keytrack_benchmark.cpp
    benchmarks/legato_benchmark.cpp
    benchmarks/lfo_benchmark.cpp
    benchmarks/mixer_benchmark.cpp
    benchmarks/modulation_benchmark.cpp
    benchmarks/modulation_routes_benchmark.cpp
    benchmarks/oscillator_benchmark.cpp
    benchmarks/solo_benchmark.cpp
    benchmarks/source_benchmark.cpp
    benchmarks/tempo_sync_benchmark.cpp
    benchmarks/voice_allocation_benchmark.cpp
    benchmarks/voice_benchmark.cpp
    benchmarks/voice_sources_benchmark.cpp
    core/allocation_counter_test.cpp
    core/audio_block_test.cpp
    core/denormals_test.cpp
    core/processor_test.cpp
    control/adsr_test.cpp
    control/envelope_report.cpp
    control/lfo_report.cpp
    control/lfo_test.cpp
    control/note_value_test.cpp
    control/smoother_test.cpp
    control/stereo_gain_test.cpp
    control/tempo_sync_report.cpp
    control/tempo_sync_test.cpp
    filters/filter_control_report.cpp
    filters/filter_control_test.cpp
    filters/filter_report.cpp
    filters/filter_stability_report.cpp
    filters/keytrack_include_order_test.cpp
    filters/keytrack_report.cpp
    filters/keytrack_test.cpp
    filters/ladder_filter_test.cpp
    filters/ladder_stability_test.cpp
    generators/white_noise_test.cpp
    math/decibels_test.cpp
    math/fast_math_test.cpp
    math/pan_test.cpp
    math/pitch_test.cpp
    math/random_test.cpp
    modulation/controller_ramp_test.cpp
    modulation/destination_candidates_test.cpp
    modulation/destination_report.cpp
    modulation/global_routes_test.cpp
    modulation/modulation_report.cpp
    modulation/modulation_test.cpp
    modulation/registry_size_functions.cpp
    modulation/registry_size_table.cpp
    modulation/registry_size_table_dsp.cpp
    modulation/registry_size_types.cpp
    modulation/registry_test.cpp
    modulation/route_compiler_test.cpp
    modulation/routing_test.cpp
    modulation/source_candidates_test.cpp
    modulation/source_report.cpp
    modulation/source_bound_test.cpp
    modulation/voice_destinations_test.cpp
    modulation/voice_sources_test.cpp
    oscillators/antialiasing_report.cpp
    oscillators/band_limited_oscillator_test.cpp
    oscillators/band_limiting_residuals_test.cpp
    oscillators/drift_report.cpp
    oscillators/oscillator_controls_report.cpp
    oscillators/oscillator_phase_report.cpp
    oscillators/oscillator_phase_test.cpp
    oscillators/oscillator_range_report.cpp
    oscillators/oscillator_range_test.cpp
    oscillators/oscillator_slot_test.cpp
    oscillators/pitch_drift_test.cpp
    oscillators/unison_layout_test.cpp
    oscillators/unison_report.cpp
    oscillators/unison_slot_test.cpp
    oscillators/waveform_test.cpp
    output/output_stage_test.cpp
    resampling/oversampler_test.cpp
    support/allocation_counter.cpp
    voice/allocation_report.cpp
    voice/amp_link_report.cpp
    voice/glide_test.cpp
    voice/legato_test.cpp
    voice/legato_report.cpp
    voice/mix_test.cpp
    voice/mixer_report.cpp
    voice/solo_report.cpp
    voice/solo_test.cpp
    voice/voice_allocator_test.cpp
    voice/voice_report.cpp
    voice/voice_test.cpp)

# anoesis::dsp comes in through anoesis::test_support. Naming it here as well
# makes Apple's linker warn about a duplicate library.
target_link_libraries(anoesis_dsp_tests PRIVATE anoesis::test_support Catch2::Catch2WithMain)
anoesis_set_warnings(anoesis_dsp_tests)
anoesis_register_tests(anoesis_dsp_tests)

# The benchmark harness: benchmarks exist and are hidden from the default run.
# Run them with tools/bench.sh (docs/performance/benchmarking.md).
add_test(NAME "anoesis_dsp_tests:benchmarks exist and are hidden from the default run"
    COMMAND ${CMAKE_COMMAND} -DTEST_EXECUTABLE=$<TARGET_FILE:anoesis_dsp_tests>
            -P ${PROJECT_SOURCE_DIR}/tests/cmake/check_benchmarks_hidden.cmake)
