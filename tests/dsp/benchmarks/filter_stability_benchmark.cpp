// CPU cost of the ladder's solve where Newton fails and the nested fallback
// runs (LadderSolver, docs/dsp/filter-stability.md): one LadderFilter channel
// for a 512-sample block, Warm Drive on (tanh) and off (cubic):
//
// - normal: the filter benchmarks' settings (LP24 at 1 kHz, k 2, drive 2, a
//   110 Hz saw at -6 dBFS), where Newton converges and the fallback never
//   runs (the filter's ordinary cost, with the input guard);
// - the corner: the cutoff held at 0.495 fs, k 8, self-oscillating after an
//   impulse, where every sample falls back (the worst case);
// - F3's top within the knobs' ranges: a full-scale saw at drive 16 (F8's
//   +24 dB), k 6 (F5's Resonance 10), the cutoff at 0.465 fs (20.5 kHz at
//   44.1 kHz), where stage 1 found Newton failing from 0.315 fs;
// - the DSP's ceiling of k: k 64, drive 32, a full-scale saw at 0.30 fs.
//
// Tagged [!benchmark][filter-stability]: run with
// tools/bench.sh "[filter-stability]" in relwithdebinfo.

#include <anoesis/dsp/filters/ladder_filter.hpp>

#include <anoesis/test/buffer.hpp>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using anoesis::dsp::LadderFilter;
using anoesis::test::Buffer;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;

/// n samples of a 110 Hz saw from -amplitude to +amplitude.
std::vector<float> sawInput(float amplitude, std::size_t n)
{
    std::vector<float> x(n);
    double phase = 0.0;
    for (auto& v : x)
    {
        v = amplitude * static_cast<float>((2.0 * phase) - 1.0);
        phase += 110.0 / static_cast<double>(rate);
        phase -= std::floor(phase);
    }
    return x;
}

LadderFilter makeFilter(bool warm, float cutoff, float k, float drive)
{
    LadderFilter f;
    f.setWarm(warm);
    f.setCutoff(cutoff);
    f.setResonance(k);
    f.setDrive(drive);
    f.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    return f;
}

/// The filter on a looping input, starting `skip` blocks in (past the
/// transient from rest).
void benchmarkInput(const std::string& name, LadderFilter& f, const std::vector<float>& input, std::size_t skip)
{
    Buffer buffer{1, blockSize};
    std::size_t offset = 0;
    const auto nextBlock = [&]
    {
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), blockSize, buffer.channel(0).begin());
        offset = (offset + blockSize) % input.size();
    };
    for (std::size_t i = 0; i < skip; ++i)
    {
        nextBlock();
        f.process(buffer.block());
    }
    BENCHMARK(name.c_str())
    {
        nextBlock();
        f.process(buffer.block());
        return buffer.channel(0)[blockSize - 1];
    };
}

} // namespace

TEST_CASE("Filter stability: CPU of the solve where the fallback runs", "[!benchmark][filter-stability]")
{
    // 96 blocks: a whole number of blocks, close to a whole number of saw periods.
    const auto quiet = sawInput(0.5f, 96 * blockSize);
    const auto loud = sawInput(1.0f, 96 * blockSize);
    const std::vector<float> silence(blockSize, 0.0f);
    for (const bool warm : {true, false})
    {
        const std::string label = warm ? "Warm on" : "Warm off";
        {
            auto f = makeFilter(warm, 1000.0f, 2.0f, 2.0f);
            benchmarkInput("stability: " + label + ", normal (1 kHz, k 2, drive 2), x512", f, quiet, 8);
        }
        {
            auto f = makeFilter(warm, 0.495f * rate, 8.0f, 1.0f);
            Buffer kick{1, blockSize};
            kick.channel(0)[0] = 0.5f;
            f.process(kick.block());
            benchmarkInput("stability: " + label + ", corner (0.495 fs, k 8, self-oscillating), x512", f, silence, 8);
        }
        {
            auto f = makeFilter(warm, 0.465f * rate, 6.0f, 16.0f);
            benchmarkInput("stability: " + label + ", F3's top (0.465 fs, k 6, drive 16, full-scale saw), x512", f,
                           loud, 8);
        }
        {
            auto f = makeFilter(warm, 0.30f * rate, 64.0f, 32.0f);
            benchmarkInput("stability: " + label + ", ceiling (0.30 fs, k 64, drive 32, full-scale saw), x512", f, loud,
                           8);
        }
    }
}
