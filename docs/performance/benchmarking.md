# Benchmarking: measuring CPU cost

Phase 3 optimises for CPU. "Profile first" (CLAUDE.md) means every optimisation needs a number before and a number after. This page explains how the benchmark harness produces those numbers, and the traps that make microbenchmarks lie. The numbers themselves are in [baselines.md](baselines.md).

## Theory

### What a microbenchmark measures

A microbenchmark times a small piece of code in isolation, such as `fastTanh` over a 512-sample block. It answers "how fast is *this*?", not "how fast is the plugin?". Its value is in *comparisons*: the same code before and after a change, or two implementations of the same thing, on the same machine and build.

Code that runs for nanoseconds can't be timed in one shot: the clock's resolution is too coarse, and one run is noisy. A sound harness therefore:

1. **warms up** (caches, branch predictors, CPU frequency);
2. **estimates the clock's resolution and overhead**;
3. runs enough **iterations per sample** that each sample is long compared with the clock resolution;
4. takes **many samples** and reports the mean with a **confidence interval** (Catch2 bootstraps it), plus outliers.

### How microbenchmarks lie

- **Dead-code elimination.** If the result is never used, the optimiser deletes the work. Every benchmark here *returns* a value, which Catch2 hides from the optimiser.
- **Constant folding.** If the inputs are compile-time constants, the optimiser may compute (or simplify) the work at compile time. This happened while writing the denormal benchmark. A variant that restarted from a `constexpr` state ran 20% faster than the identical loop with run-time inputs. The fix is to read inputs through `volatile` or from data the compiler can't see through.
- **Unrealistic batching.** One call per sample hides per-call overhead that a per-block design would amortise, and vice versa. Our benchmarks process a 512-sample block, a typical host block, so they match how the code will be called.
- **Debug builds.** Unoptimised timings are meaningless for comparisons. Benchmarks run in `relwithdebinfo` (`-O2 -g`).
- **The machine.** Frequency scaling, thermal limits, other processes, and **virtualisation** all add noise. Record the machine with every result, and compare only results from the same one. Repeat runs to see the noise level.
- **Hardware-specific effects.** Some costs exist on one CPU and not another. Denormals are the classic example: very slow on x86, nearly free on Apple Silicon. A baseline from one architecture says nothing about the other.

### Real-time budget

At 48 kHz one sample lasts 20,833 ns. A process costing *t* ns per sample per channel uses *t*/20,833 of one core in real time. That percentage is the most useful way to read a result. A full patch (16 voices × 2 parts × 2 oscillators with unison, filters, effects) must fit comfortably in a fraction of a core.

## How it is applied here

- **Benchmarks are Catch2 `BENCHMARK`s** inside the normal test executables, in `tests/<lib>/benchmarks/`. Each `TEST_CASE` is tagged **`[!benchmark]`**, which Catch2 hides unless asked for, so `catch_discover_tests` never registers them and `ctest` never runs them.
- **A CTest check keeps that true.** `anoesis_dsp_tests:benchmarks exist and are hidden from the default run` ([tests/cmake/check_benchmarks_hidden.cmake](../../tests/cmake/check_benchmarks_hidden.cmake)) lists the `[!benchmark]` test cases and the default ones. It fails if there are no benchmarks, or if any benchmark appears in the default run. It was written first and failed ("No [!benchmark] test cases found") before the benchmarks existed.
- **Naming:** a benchmark that processes N samples per run ends in `xN` (for example `fastTanh x512`), so the report can divide by N.
- **Running:** [tools/bench.sh](../../tools/bench.sh) configures and builds `relwithdebinfo` (only the test target), runs `[!benchmark]` with 100 samples each, writes Catch2's XML to `build/relwithdebinfo/benchmarks.xml`, and prints the machine, compiler, and commit, followed by a Markdown table ([tools/bench_table.py](../../tools/bench_table.py)):

  ```sh
  tools/bench.sh                   # everything
  tools/bench.sh "[oversampling]"  # one area
  ```

- **Recording:** paste the output into [baselines.md](baselines.md) with the date. Keep earlier entries: the history is the point.

## Trade-offs and alternatives

- **Catch2 `BENCHMARK` versus Google Benchmark or nanobench.** Google Benchmark has richer reporting (counters, complexity fits) but would be a new dependency. Catch2 is already here, its statistics are sound (bootstrap confidence intervals, outlier classification), and benchmarks sit next to the tests of the code they measure.
- **Microbenchmarks versus profiling the plugin.** Both are needed. Microbenchmarks give stable, comparable numbers for building blocks. Profiling a real patch in a host (M1.8 benchmarks, Phase 3) shows where the time actually goes. Phase 3 starts with the profile and uses these benchmarks to verify each fix.
- **Running benchmarks in CI.** Not done. Shared CI runners are too noisy for meaningful CPU numbers, and the CI policy runs only on version tags anyway. Baselines are taken on a known machine and recorded by hand.

## Tests

- [tests/cmake/check_benchmarks_hidden.cmake](../../tests/cmake/check_benchmarks_hidden.cmake), registered in [tests/dsp/CMakeLists.txt](../../tests/dsp/CMakeLists.txt): the benchmarks exist and are hidden.
- [tests/dsp/benchmarks/foundations_benchmark.cpp](../../tests/dsp/benchmarks/foundations_benchmark.cpp): the M1.0 benchmarks themselves. They compile in every build, so they can't rot, but they run only on request.

## References

- Catch2 documentation: `external/Catch2/docs/benchmarks.md`.
- Chandler Carruth, "Tuning C++: Benchmarks, and CPUs, and Compilers! Oh My!", CppCon 2015.
- Andrey Akinshin, *Pro .NET Benchmarking* (2019): measurement methodology that applies to any language, including statistics and environment noise.
- Brendan Gregg, *Systems Performance* (2nd ed., 2020), ch. 12: benchmarking pitfalls.
