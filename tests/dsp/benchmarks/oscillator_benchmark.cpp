// CPU cost of the minBLEP oscillator (decision D6) and the PolyBLEP reference,
// for every periodic waveform.
// The wavetable candidate's numbers (it was removed after D6) are in the
// baselines doc.
//
// Tagged [!benchmark]: run with tools/bench.sh "[oscillators]" in
// relwithdebinfo. Each benchmark renders one 512-sample block. Results and the
// decision: docs/performance/baselines.md, docs/dsp/band-limited-oscillators.md

#include "../oscillators/oscillator_support.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

using namespace anoesis::test::d6;

namespace
{

constexpr std::size_t blockSize = 512;
constexpr float rate = 48000.0f;
using Block = std::array<float, blockSize>;

template <typename Osc>
Osc preparedOscillator(Waveform shape, float hz)
{
    auto osc = Candidate<Osc>::make();
    osc.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
    osc.setWaveform(shape);
    osc.setFrequency(hz);
    return osc;
}

template <typename Osc>
void benchmarkCandidate()
{
    const std::string name = Candidate<Osc>::name;
    Block out{};

    const auto steady = [&](const char* label, Waveform shape, float hz)
    {
        auto osc = preparedOscillator<Osc>(shape, hz);
        BENCHMARK(name + ", " + label + " x512")
        {
            for (auto& sample : out)
            {
                sample = osc.next();
            }
            return out[blockSize - 1];
        };
    };
    // Every waveform at an ordinary pitch and at 12 kHz, where minBLEP's
    // cost per edge dominates (the saw and square also at 5 kHz, as in D6).
    steady("saw 440 Hz", Waveform::saw, 440.0f);
    steady("saw 5 kHz", Waveform::saw, 5000.0f);
    steady("saw 12 kHz", Waveform::saw, 12000.0f);
    steady("square 440 Hz", Waveform::pulse, 440.0f);
    steady("square 5 kHz", Waveform::pulse, 5000.0f);
    steady("square 12 kHz", Waveform::pulse, 12000.0f);
    steady("HPulse 440 Hz", Waveform::hpulse, 440.0f);
    steady("HPulse 12 kHz", Waveform::hpulse, 12000.0f);
    steady("QPulse 440 Hz", Waveform::qpulse, 440.0f);
    steady("QPulse 12 kHz", Waveform::qpulse, 12000.0f);
    steady("triangle 440 Hz", Waveform::triangle, 440.0f);
    steady("triangle 12 kHz", Waveform::triangle, 12000.0f);
    steady("sine 440 Hz", Waveform::sine, 440.0f);
    steady("sine 12 kHz", Waveform::sine, 12000.0f);
    steady("TriSaw 440 Hz", Waveform::trisaw, 440.0f);
    steady("TriSaw 12 kHz", Waveform::trisaw, 12000.0f);

    // A new pitch every sample (vibrato, +-1 semitone), through setFrequency:
    // the path pitch modulation and portamento will take.
    {
        auto osc = preparedOscillator<Osc>(Waveform::saw, 440.0f);
        Block pitch{};
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            pitch[i] = 440.0f * std::exp2(std::sin(static_cast<float>(i) * 0.05f) / 12.0f);
        }
        BENCHMARK(name + ", saw 440 Hz, pitch set every sample x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                osc.setFrequency(pitch[i]);
                out[i] = osc.next();
            }
            return out[blockSize - 1];
        };
    }

    // One note's worth: 4 oscillators x 8 unison voices, saws at 110, 220,
    // 440, and 880 Hz, each detuned over +-20 cents. Cost per oscillator =
    // this / 32.
    {
        std::vector<Osc> bank;
        for (const float base : {110.0f, 220.0f, 440.0f, 880.0f})
        {
            for (int voice = 0; voice < 8; ++voice)
            {
                const auto cents = -20.0f + (40.0f * static_cast<float>(voice) / 7.0f);
                bank.push_back(preparedOscillator<Osc>(Waveform::saw, base * std::exp2(cents / 1200.0f)));
            }
        }
        BENCHMARK(name + ", 32 saws (4 osc x 8 unison) x512")
        {
            out.fill(0.0f);
            for (auto& osc : bank)
            {
                for (auto& sample : out)
                {
                    sample += osc.next();
                }
            }
            return out[blockSize - 1];
        };
    }
}

} // namespace

TEST_CASE("Benchmark: band-limited oscillators", "[!benchmark][dsp][oscillators]")
{
    benchmarkCandidate<PolyBlep2>();
    benchmarkCandidate<PolyBlep4>();
    benchmarkCandidate<MinBlep32>();
    benchmarkCandidate<MinBlep64>();
}
