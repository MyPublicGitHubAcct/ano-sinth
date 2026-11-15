// CPU cost of the stability round's solver candidates
// (tests/dsp/filters/filter_stability_candidates.hpp, docs/dsp/filter-stability.md):
// one channel for a 512-sample block at 48 kHz, Warm Drive on (tanh) and off
// (cubic), at the filter benchmarks' normal settings (LP24 at 1 kHz, k 2,
// drive 2, a 110 Hz saw at -6 dBFS) and at the hard corner (the cutoff held
// at 0.495 fs, k 8, self-oscillating after an impulse, where the as-built
// solver fails); the two NaN guards on the as-built solver; and LadderFilter
// itself for scale. Tagged [!benchmark][filter-stability]: run with
// tools/bench.sh "[filter-stability]" in relwithdebinfo.

#include "../filters/filter_stability_candidates.hpp"

#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

using namespace anoesis::test::stability;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

std::vector<float> sawInput()
{
    std::vector<float> x(blockSize);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = static_cast<float>(phase - 0.5);
        phase += 110.0 / static_cast<double>(rate);
        phase -= std::floor(phase);
    }
    return x;
}

template <typename Sat, typename Solver>
void benchmarkCandidate(const std::string& name, const std::vector<float>& input)
{
    const std::string warm = std::is_same_v<Sat, dsp::TanhSaturator> ? "Warm on" : "Warm off";
    {
        CandidateFilter<Sat, Solver> f;
        f.setResonance(2.0f);
        f.setDrive(2.0f);
        f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        f.setCutoff(1000.0f);
        Buffer buffer{1, blockSize};
        BENCHMARK("stability: " + name + ", " + warm + ", normal (1 kHz, k 2, drive 2), x512")
        {
            std::ranges::copy(input, buffer.channel(0).begin());
            f.process(buffer.block());
            return buffer.channel(0)[blockSize - 1];
        };
    }
    {
        CandidateFilter<Sat, Solver> f;
        f.setResonance(8.0f);
        f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        f.setCutoff(0.495f * rate);
        static_cast<void>(f.processSample(0, 0.5f));
        for (int i = 0; i < 4096; ++i)
        {
            static_cast<void>(f.processSample(0, 0.0f));
        }
        Buffer buffer{1, blockSize};
        BENCHMARK("stability: " + name + ", " + warm + ", corner (0.495 fs, k 8, self-oscillating), x512")
        {
            std::ranges::fill(buffer.channel(0), 0.0f);
            f.process(buffer.block());
            return buffer.channel(0)[blockSize - 1];
        };
    }
}

template <typename Sat>
void benchmarkAll(const std::vector<float>& input)
{
    benchmarkCandidate<Sat, NewtonAsBuilt<>>("as built", input);
    benchmarkCandidate<Sat, NewtonDamped<8>>("damped, 8", input);
    benchmarkCandidate<Sat, NewtonDamped<32>>("damped, 32", input);
    benchmarkCandidate<Sat, NewtonStepLimited<>>("step-limited", input);
    benchmarkCandidate<Sat, NewtonPreviousGuess<>>("previous guess", input);
    benchmarkCandidate<Sat, NewtonOutputVariable<>>("output variable", input);
    benchmarkCandidate<Sat, NewtonBracketFallback<false>>("nested fallback from Newton", input);
    benchmarkCandidate<Sat, NewtonBracketFallback<true>>("nested fallback from the guess", input);
    benchmarkCandidate<Sat, NestedOnly>("nested alone", input);
    benchmarkCandidate<Sat, NewtonInputAdaa<>>("ADAA on the input", input);
    benchmarkCandidate<Sat, SanitizedInput<NewtonAsBuilt<>>>("as built, input sanitised", input);
}

} // namespace

TEST_CASE("Filter stability: CPU of the solver candidates", "[!benchmark][filter-stability]")
{
    const auto input = sawInput();
    benchmarkAll<dsp::TanhSaturator>(input);
    benchmarkAll<dsp::CubicSaturator>(input);

    // The per-block state check, on the as-built solver at normal settings.
    CandidateFilter<dsp::TanhSaturator, NewtonAsBuilt<>> checked;
    checked.setResonance(2.0f);
    checked.setDrive(2.0f);
    checked.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    checked.setCutoff(1000.0f);
    Buffer buffer{1, blockSize};
    BENCHMARK("stability: as built, Warm on, state checked per block, normal (1 kHz, k 2, drive 2), x512")
    {
        std::ranges::copy(input, buffer.channel(0).begin());
        checked.process(buffer.block());
        checked.recoverNonFinite();
        return buffer.channel(0)[blockSize - 1];
    };

    // LadderFilter itself at the same normal settings, for scale.
    anoesis::dsp::LadderFilter built;
    built.setCutoff(1000.0f);
    built.setResonance(2.0f);
    built.setDrive(2.0f);
    built.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    BENCHMARK("stability: LadderFilter, Warm on, normal (1 kHz, k 2, drive 2), x512")
    {
        std::ranges::copy(input, buffer.channel(0).begin());
        built.process(buffer.block());
        return buffer.channel(0)[blockSize - 1];
    };
}
