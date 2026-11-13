# CTest check: the benchmark harness exists and stays out of the default run.
#
#   cmake -DTEST_EXECUTABLE=<path> -P check_benchmarks_hidden.cmake
#
# Benchmarks are tagged [!benchmark], which Catch2 hides unless asked for, so
# catch_discover_tests never registers them and `ctest` never runs them (they
# are slow, and meaningless in Debug). This check fails if there are no
# benchmarks at all, or if one leaks into the default test list.

execute_process(
    COMMAND "${TEST_EXECUTABLE}" --list-tests "[!benchmark]" --verbosity quiet
    OUTPUT_VARIABLE benchmark_list
    RESULT_VARIABLE benchmark_result)
execute_process(
    COMMAND "${TEST_EXECUTABLE}" --list-tests --verbosity quiet
    OUTPUT_VARIABLE default_list
    RESULT_VARIABLE default_result)

if(NOT benchmark_result EQUAL 0 OR NOT default_result EQUAL 0)
    message(FATAL_ERROR "Listing tests failed (${benchmark_result}, ${default_result})")
endif()

string(STRIP "${benchmark_list}" benchmark_list)
if(benchmark_list STREQUAL "")
    message(FATAL_ERROR "No [!benchmark] test cases found in ${TEST_EXECUTABLE}")
endif()

string(REPLACE "\n" ";" benchmarks "${benchmark_list}")
string(REPLACE "\n" ";" defaults "${default_list}")
list(LENGTH benchmarks count)
foreach(name IN LISTS benchmarks)
    if(name IN_LIST defaults)
        message(FATAL_ERROR "Benchmark '${name}' is in the default test run. Tag it [!benchmark].")
    endif()
endforeach()
message(STATUS "${count} benchmark test case(s), all hidden from the default run")
