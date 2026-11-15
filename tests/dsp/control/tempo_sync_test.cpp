// Tests for tempo sync as chosen in docs/dsp/tempo-sync.md: HostTempo
// (libs/dsp/include/anoesis/dsp/control/host_tempo.hpp), the tempo a synced
// parameter follows, and a synced LFO as a voice drives one: once per block,
// Lfo::setPhaseIncrement(notePhaseIncrement(value, tempo.bpm(), fs)), the
// phase started at the note-on and never moved by the host's position.

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/control/host_tempo.hpp>
#include <anoesis/dsp/control/lfo.hpp>
#include <anoesis/dsp/control/note_value.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using anoesis::dsp::HostTempo;
using anoesis::dsp::Lfo;
using anoesis::dsp::LfoWaveform;
using anoesis::dsp::notePhaseIncrement;
using anoesis::dsp::NoteValue;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float rate = 48000.0f;
constexpr double rateD = 48000.0;

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

/// A synced LFO through a tempo step from 120 to 140 BPM at sample `step`,
/// in blocks of `blockSize` that the host splits at the step. Returns the
/// output; `lfo` is left after the last sample.
std::vector<float> renderSynced(Lfo& lfo, NoteValue value, std::size_t length, std::size_t step, std::size_t blockSize)
{
    HostTempo tempo;
    std::vector<float> out(length);
    std::size_t n = 0;
    while (n < length)
    {
        std::size_t end = std::min(n + blockSize, length);
        if (n < step && step < end)
        {
            end = step;
        }
        tempo.report(n < step ? 120.0 : 140.0);
        lfo.setPhaseIncrement(notePhaseIncrement(value, tempo.bpm(), rateD));
        lfo.process(std::span{out}.subspan(n, end - n));
        n = end;
    }
    return out;
}

Lfo preparedLfo(LfoWaveform waveform)
{
    Lfo lfo{1};
    lfo.setWaveform(waveform);
    lfo.prepare({.sampleRate = rate, .maxBlockSize = 4096, .numChannels = 1});
    return lfo;
}

} // namespace

TEST_CASE("Host tempo: 120 BPM until a host reports one, then the last valid report", "[dsp][tempo]")
{
    HostTempo tempo;
    CHECK_THAT(tempo.bpm(), WithinULP(120.0, 0));
    CHECK_THAT(HostTempo::defaultBpm, WithinULP(120.0, 0));
    tempo.report(93.5);
    CHECK_THAT(tempo.bpm(), WithinULP(93.5, 0));
    tempo.report(174.0);
    CHECK_THAT(tempo.bpm(), WithinULP(174.0, 0));
    tempo.reset();
    CHECK_THAT(tempo.bpm(), WithinULP(120.0, 0));
}

TEST_CASE("Host tempo: 0, negative, NaN, and infinite reports are ignored; the rest clamp to [1, 999] BPM",
          "[dsp][tempo]")
{
    HostTempo tempo;
    tempo.report(140.0);
    for (const double bad : {
             0.0,
             -0.0,
             -60.0,
             std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(),
             -std::numeric_limits<double>::infinity(),
         })
    {
        tempo.report(bad);
        CHECK_THAT(tempo.bpm(), WithinULP(140.0, 0));
    }
    tempo.report(0.25);
    CHECK_THAT(tempo.bpm(), WithinULP(HostTempo::minBpm, 0));
    tempo.report(5000.0);
    CHECK_THAT(tempo.bpm(), WithinULP(HostTempo::maxBpm, 0));
    CHECK_THAT(HostTempo::minBpm, WithinULP(1.0, 0));
    CHECK_THAT(HostTempo::maxBpm, WithinULP(999.0, 0));
}

TEST_CASE("Tempo sync: a synced LFO's phase is exact through a tempo step, with no jump", "[dsp][tempo]")
{
    const NoteValue value = NoteValue::dotted(1, 8);
    constexpr std::size_t step = 96000;
    constexpr std::size_t length = 192000;

    // The phase: the start plus every sample's increment, exactly, the
    // tempo's change landing on the step's sample.
    auto saw = preparedLfo(LfoWaveform::saw);
    const std::uint64_t start = saw.phase();
    static_cast<void>(renderSynced(saw, value, length, step, 512));
    const std::uint64_t at120 = notePhaseIncrement(value, 120.0, rateD);
    const std::uint64_t at140 = notePhaseIncrement(value, 140.0, rateD);
    CHECK(saw.phaseIncrement() == at140);
    CHECK(saw.phase() == start + (std::uint64_t{step} * at120) + (std::uint64_t{length - step} * at140));

    // A sine's largest step around the tempo change is its own slope at the
    // faster rate: no jump. (Its interpolation on the 32-sample grid adds
    // nothing to the largest step of a straight line between grid points.)
    auto sine = preparedLfo(LfoWaveform::sine);
    const auto y = renderSynced(sine, value, length, step, 512);
    const double slope = 2.0 * std::numbers::pi * anoesis::dsp::noteHertz(value, 140.0) / rateD;
    double largest = 0.0;
    for (std::size_t n = step - 64; n < step + 4096; ++n)
    {
        largest = std::max(largest, static_cast<double>(std::abs(y[n] - y[n - 1])));
    }
    // The grid's chord is never steeper than the sine; float rounding of
    // two values near 1 adds 2^-23.
    CHECK(largest <= slope + 0x1p-23);
}

TEST_CASE("Tempo sync: a synced LFO does not depend on the block size", "[dsp][tempo]")
{
    const NoteValue value = NoteValue::dotted(1, 8);
    for (const LfoWaveform waveform : {LfoWaveform::sine, LfoWaveform::saw, LfoWaveform::pulse})
    {
        auto a = preparedLfo(waveform);
        auto b = preparedLfo(waveform);
        auto c = preparedLfo(waveform);
        const auto ya = renderSynced(a, value, 120000, 48000, 1);
        const auto yb = renderSynced(b, value, 120000, 48000, 37);
        const auto yc = renderSynced(c, value, 120000, 48000, 4096);
        CHECK(std::ranges::equal(ya, yb, sameBits));
        CHECK(std::ranges::equal(ya, yc, sameBits));
    }
}

TEST_CASE("Tempo sync: no allocation in the tempo, the conversion, or a synced LFO", "[dsp][tempo]")
{
    auto lfo = preparedLfo(LfoWaveform::triangle);
    HostTempo tempo;
    std::array<float, 256> block{};
    std::size_t count = 0;
    {
        const anoesis::test::AllocationCounter counter;
        for (std::size_t i = 0; i < 64; ++i)
        {
            tempo.report(90.0 + static_cast<double>(i));
            const NoteValue value = anoesis::dsp::syncNoteValues[i % anoesis::dsp::syncNoteValues.size()];
            lfo.setPhaseIncrement(notePhaseIncrement(value, tempo.bpm(), rateD));
            lfo.process(block);
        }
        count = counter.count();
    }
    CHECK(count == 0);
}
