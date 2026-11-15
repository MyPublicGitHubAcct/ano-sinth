# Tests for libs/dsp. No JUCE, mirroring the library.
add_executable(anoesis_dsp_tests
    library_test.cpp
    benchmarks/foundations_benchmark.cpp
    benchmarks/drift_benchmark.cpp
    benchmarks/envelope_benchmark.cpp
    benchmarks/filter_benchmark.cpp
    benchmarks/keytrack_benchmark.cpp
    benchmarks/lfo_benchmark.cpp
    benchmarks/oscillator_benchmark.cpp
    benchmarks/tempo_sync_benchmark.cpp
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
    filters/filter_report.cpp
    filters/keytrack_report.cpp
    filters/ladder_filter_test.cpp
    generators/white_noise_test.cpp
    math/decibels_test.cpp
    math/fast_math_test.cpp
    math/pan_test.cpp
    math/pitch_test.cpp
    math/random_test.cpp
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
    resampling/oversampler_test.cpp
    support/allocation_counter.cpp)

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
