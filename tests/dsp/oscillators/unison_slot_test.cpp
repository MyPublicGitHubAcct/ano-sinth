// Tests for unison in OscillatorSlot: 0-8 voices per oscillator, detune and
// stereo spread, Pan with Stereo (O8), the level law, Phase and Retrig for
// every voice, Noise, voice-count changes mid-note, and the real-time and
// lifecycle contracts. The laws themselves: unison_layout_test.cpp.
// Measurements: unison_report.cpp. Doc: docs/dsp/oscillator-unison.md

#include "../support/allocation_counter.hpp"
#include "oscillator_support.hpp"

#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/lifecycle.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using anoesis::dsp::AudioBlock;
using anoesis::dsp::maxUnisonVoices;
using anoesis::dsp::panGains;
using anoesis::dsp::phaseFromDegrees;
using anoesis::dsp::ProcessSpec;
using anoesis::dsp::StereoGains;
using anoesis::dsp::unisonLayout;
using anoesis::dsp::unisonPosition;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using Catch::Matchers::WithinULP;

namespace
{

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

constexpr float rate = 48000.0f;
constexpr std::size_t allVoices = maxUnisonVoices;

ProcessSpec specAt(float sampleRate)
{
    return {.sampleRate = sampleRate, .maxBlockSize = 512, .numChannels = 2};
}

Slot preparedSlot(Waveform shape, int voices = 1, float sampleRate = rate)
{
    Slot slot{minBlepTable64()};
    slot.setVoices(voices);
    slot.prepare(specAt(sampleRate));
    slot.setWaveform(shape);
    return slot;
}

Buffer render(Slot& slot, float note, std::size_t numSamples, std::size_t blockSize = 512)
{
    Buffer out{2, numSamples};
    for (std::size_t start = 0; start < numSamples; start += blockSize)
    {
        slot.process(out.block().subBlock(start, std::min(blockSize, numSamples - start)), note);
    }
    return out;
}

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

bool bitIdentical(std::span<const float> a, std::span<const float> b)
{
    return std::ranges::equal(a, b, sameBits);
}

bool allZero(std::span<const float> x)
{
    return std::ranges::all_of(x, [](float s) { return sameBits(s, 0.0f); });
}

double incrementHz(std::uint32_t increment, double sampleRate)
{
    return static_cast<double>(increment) * sampleRate / 4294967296.0;
}

double cents(double ratio)
{
    return 1200.0 * std::log2(ratio);
}

double meanSquare(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return sum / static_cast<double>(x.size());
}

double correlation(std::span<const float> a, std::span<const float> b)
{
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return ab / std::sqrt(aa * bb);
}

/// The reference model of an N-voice slot with distinct per-voice gains:
/// bare oscillators at the slot's own increments, each started by `start`
/// (restart() or jumpTo()), each added into the bus with the slot's gains
/// for that voice, in voice order. `events` are (sample, action) pairs.
struct Reference
{
    std::vector<MinBlep64> voices;
    std::vector<StereoGains> gains;

    Reference(const Slot& slot, Waveform shape)
    {
        for (std::size_t v = 0; v < slot.voices(); ++v)
        {
            voices.push_back(Candidate<MinBlep64>::make());
            voices.back().prepare({.sampleRate = rate, .maxBlockSize = 512, .numChannels = 1});
            voices.back().setWaveform(shape);
            voices.back().setIncrement(slot.phaseIncrement(v));
            gains.push_back(slot.voiceGains(v));
        }
    }

    void renderInto(Buffer& out, std::size_t from, std::size_t to)
    {
        for (std::size_t i = from; i < to; ++i)
        {
            for (std::size_t v = 0; v < voices.size(); ++v)
            {
                const float y = voices[v].next();
                out.channel(0)[i] += gains[v].left * y;
                out.channel(1)[i] += gains[v].right * y;
            }
        }
    }
};

void requireClose(const Buffer& a, const Buffer& b, double tolerance)
{
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        for (std::size_t i = 0; i < a.numSamples(); ++i)
        {
            CAPTURE(ch, i);
            REQUIRE_THAT(a.channel(ch)[i], WithinAbs(b.channel(ch)[i], tolerance));
        }
    }
}

/// A sine voice found in a spectrum: its frequency (interpolated) and
/// amplitude.
struct Peak
{
    double hz = 0.0;
    double amplitude = 0.0;
};

/// The peak nearest each expected frequency, located to a fraction of a bin
/// by a parabola through the log power of the three bins at the maximum (the
/// Blackman-Harris main lobe is close to Gaussian). Amplitude from the power
/// in the main lobe (spectrum.hpp's normalisation).
std::vector<Peak> findPeaks(std::span<const float> signal, std::span<const double> expectedHz)
{
    const auto window = blackmanHarrisWindow(signal.size());
    const auto spectrum = powerSpectrum(signal, rate, window);
    std::vector<Peak> peaks;
    for (const double hz : expectedHz)
    {
        auto best = spectrum.nearestBin(hz);
        for (std::size_t k = best - 3; k <= best + 3; ++k)
        {
            if (spectrum.power[k] > spectrum.power[best])
            {
                best = k;
            }
        }
        const double a = std::log(spectrum.power[best - 1]);
        const double b = std::log(spectrum.power[best]);
        const double c = std::log(spectrum.power[best + 1]);
        const double offset = 0.5 * (a - c) / (a - (2.0 * b) + c);
        peaks.push_back({
            .hz = (static_cast<double>(best) + offset) * spectrum.binWidth(),
            .amplitude = std::sqrt(2.0 * spectrum.powerAround(spectrum.frequencyOf(best), spectrum.mainLobeBins)),
        });
    }
    return peaks;
}

/// The frequencies voice v of N plays at `note`, in double, from the layout
/// and `spread` semitones: the target the slot's increments are checked
/// against.
double targetHz(float note, float spread, std::size_t count, std::size_t v)
{
    const double offset = static_cast<double>(spread) * static_cast<double>(unisonLayout(count).detune[v]);
    return 440.0 * std::exp2((static_cast<double>(note) - 69.0 + offset) / 12.0);
}

} // namespace

TEST_CASE("Unison: every voice plays the note plus its detune, linear in semitones", "[dsp][oscillator][unison]")
{
    // Each increment against the double-precision target: the slot's pitch
    // tolerance (oscillator_slot_test.cpp) plus the detune ratio's exp2 and
    // one multiply in float (3e-7), plus one fixed-point unit.
    constexpr float note = 57.3f;
    for (std::size_t count = 1; count <= allVoices; ++count)
    {
        for (const float spread : {0.0f, 0.05f, 1.0f, 3.3219f, 12.0f, -2.0f})
        {
            auto slot = preparedSlot(Waveform::saw, static_cast<int>(count));
            slot.setDetune(spread);
            Buffer out{2, 1};
            slot.process(out.block(), note);
            REQUIRE(slot.voices() == count);
            for (std::size_t v = 0; v < count; ++v)
            {
                CAPTURE(count, spread, v);
                const double target = targetHz(note, spread, count, v);
                const double played = incrementHz(slot.phaseIncrement(v), rate);
                REQUIRE_THAT(played / target, WithinAbs(1.0, 2.0e-6 + (rate / 4294967296.0 / target)));
            }
        }
    }

    // With no detune, every voice plays exactly the one-voice pitch, and a
    // NaN detune counts as none.
    auto single = preparedSlot(Waveform::saw, 1);
    auto stack = preparedSlot(Waveform::saw, 8);
    auto nanDetune = preparedSlot(Waveform::saw, 8);
    nanDetune.setDetune(std::numeric_limits<float>::quiet_NaN());
    Buffer out{2, 1};
    single.process(out.block(), note);
    stack.process(out.block(), note);
    nanDetune.process(out.block(), note);
    for (std::size_t v = 0; v < allVoices; ++v)
    {
        CHECK(stack.phaseIncrement(v) == single.phaseIncrement());
        CHECK(nanDetune.phaseIncrement(v) == single.phaseIncrement());
    }
}

TEST_CASE("Unison: the rendered voices are at their pitches, with equal amplitude", "[dsp][oscillator][unison]")
{
    // Sines, so each voice is one spectral line. 2^17 samples at 48 kHz:
    // 0.37 Hz bins; at a spread of 1.5 semitones over 8 voices the lines are
    // 11 Hz apart.
    constexpr float note = 69.0f;
    constexpr float spread = 1.5f;
    constexpr std::size_t length = 131072;
    for (std::size_t count = 2; count <= allVoices; ++count)
    {
        CAPTURE(count);
        auto slot = preparedSlot(Waveform::sine, static_cast<int>(count));
        slot.setDetune(spread);
        slot.setRetrigger(false);
        slot.setRandomSeed(count);
        slot.noteOn();
        const auto out = render(slot, note, length + 4096);
        std::vector<double> expected;
        expected.reserve(count);
        for (std::size_t v = 0; v < count; ++v)
        {
            expected.push_back(incrementHz(slot.phaseIncrement(v), rate));
        }
        const auto peaks = findPeaks(out.channel(0).subspan(4096, length), expected);
        for (std::size_t v = 0; v < count; ++v)
        {
            CAPTURE(v, peaks[v].hz, expected[v]);
            CHECK(std::abs(cents(peaks[v].hz / expected[v])) < 0.05);
            CHECK(std::abs(cents(expected[v] / targetHz(note, spread, count, v))) < 0.003);
            // Stereo 0: every voice at the centre, -3 dB per side.
            CHECK_THAT(peaks[v].amplitude, WithinAbs(std::sqrt(0.5), 2e-3));
        }
    }

    // Two voices at Stereo 10 are one per channel, so each channel's
    // frequency can be measured exactly.
    auto pair = preparedSlot(Waveform::sine, 2);
    pair.setDetune(0.7f);
    pair.setStereo(1.0f);
    const auto out = render(pair, 60.0f, 96000);
    CHECK_THAT(measureFrequency(out.channel(0), rate) / incrementHz(pair.phaseIncrement(0), rate),
               WithinAbs(1.0, 1e-8));
    CHECK_THAT(measureFrequency(out.channel(1), rate) / incrementHz(pair.phaseIncrement(1), rate),
               WithinAbs(1.0, 1e-8));
    CHECK(pair.phaseIncrement(0) < pair.phaseIncrement(1)); // the lower voice is on the left
}

TEST_CASE("Unison: each voice's gains are the pan law at Pan + width x side, clipped (O8)", "[dsp][oscillator][unison]")
{
    for (std::size_t count = 1; count <= allVoices; ++count)
    {
        for (const float width : {0.0f, 0.3f, 1.0f})
        {
            for (const float pan : {-1.0f, -0.4f, 0.0f, 0.5f, 1.0f})
            {
                for (const bool inverted : {false, true})
                {
                    auto slot = preparedSlot(Waveform::saw, static_cast<int>(count));
                    slot.setStereo(width);
                    slot.setPan(pan);
                    slot.setVolume(0.8f);
                    slot.setInverted(inverted);
                    for (std::size_t v = 0; v < count; ++v)
                    {
                        CAPTURE(count, width, pan, inverted, v);
                        const auto law = panGains(unisonPosition(pan, width, unisonLayout(count).side[v]));
                        const float volume = inverted ? -0.8f : 0.8f;
                        CHECK_THAT(slot.voiceGains(v).left, WithinULP(volume * law.left, 0));
                        CHECK_THAT(slot.voiceGains(v).right, WithinULP(volume * law.right, 0));
                    }
                }
            }
        }
    }

    // O8's distinguishing case: 2 voices, Stereo 10, Pan +0.5. The left voice
    // moves to -0.5; the right one would be at 1.5 and is clipped to 1.
    auto slot = preparedSlot(Waveform::saw, 2);
    slot.setStereo(1.0f);
    slot.setPan(0.5f);
    CHECK_THAT(slot.voiceGains(0).left, WithinAbs(0.92388, 1e-5));
    CHECK_THAT(slot.voiceGains(0).right, WithinAbs(0.38268, 1e-5));
    CHECK_THAT(slot.voiceGains(1).left, WithinULP(0.0f, 0));
    CHECK_THAT(slot.voiceGains(1).right, WithinULP(1.0f, 0));
}

TEST_CASE("Unison: the rendered stereo image matches each voice's gains", "[dsp][oscillator][unison]")
{
    constexpr std::size_t length = 131072;
    auto slot = preparedSlot(Waveform::sine, 8);
    slot.setDetune(1.5f);
    slot.setStereo(0.6f);
    slot.setPan(-0.3f);
    slot.reset(); // settle the gains
    const auto out = render(slot, 69.0f, length + 4096);
    std::vector<double> expected;
    expected.reserve(allVoices);
    for (std::size_t v = 0; v < allVoices; ++v)
    {
        expected.push_back(incrementHz(slot.phaseIncrement(v), rate));
    }
    const auto left = findPeaks(out.channel(0).subspan(4096, length), expected);
    const auto right = findPeaks(out.channel(1).subspan(4096, length), expected);
    for (std::size_t v = 0; v < allVoices; ++v)
    {
        CAPTURE(v);
        CHECK_THAT(left[v].amplitude, WithinAbs(slot.voiceGains(v).left, 2e-3));
        CHECK_THAT(right[v].amplitude, WithinAbs(slot.voiceGains(v).right, 2e-3));
    }
}

TEST_CASE("Unison: N in-phase voices are exactly N times one voice (no normalisation)", "[dsp][oscillator][unison]")
{
    // Retrig on, Phase 0, Detune 0, Stereo 0: the voices are identical, so
    // the stack is N x one voice, +6.02 dB per doubling (inventory O15).
    for (const auto shape : {Waveform::saw, Waveform::pulse, Waveform::sine})
    {
        auto one = preparedSlot(shape, 1);
        one.noteOn();
        const auto reference = render(one, 50.0f, 2000);
        for (std::size_t count = 1; count <= allVoices; ++count)
        {
            CAPTURE(shapeName(shape), count);
            auto slot = preparedSlot(shape, static_cast<int>(count));
            slot.noteOn();
            const auto out = render(slot, 50.0f, 2000);
            const auto n = static_cast<float>(count);
            for (std::size_t i = 0; i < out.numSamples(); ++i)
            {
                REQUIRE_THAT(out.channel(0)[i], WithinAbs(n * reference.channel(0)[i], 1e-6));
                REQUIRE_THAT(out.channel(1)[i], WithinAbs(n * reference.channel(1)[i], 1e-6));
            }
        }
    }
}

TEST_CASE("Unison: with Retrig on, a note-on starts every voice at Phase", "[dsp][oscillator][unison][phase]")
{
    // From silence (restart) and while sounding (a band-limited jump), against
    // bare oscillators at the slot's increments and gains.
    for (const auto shape : {Waveform::saw, Waveform::triangle, Waveform::sine})
    {
        CAPTURE(shapeName(shape));
        auto slot = preparedSlot(shape, 5);
        slot.setDetune(0.7f);
        slot.setStereo(0.8f);
        slot.setPan(-0.2f);
        slot.setPhaseDegrees(135.0f);
        slot.reset();
        Buffer probe{2, 1};
        slot.process(probe.block(), 62.0f); // sets the increments
        slot.stop();

        Reference reference{slot, shape};
        Buffer expected{2, 1500};
        for (auto& voice : reference.voices)
        {
            voice.restart(phaseFromDegrees(135.0f));
        }
        reference.renderInto(expected, 0, 700);
        for (auto& voice : reference.voices)
        {
            voice.jumpTo(phaseFromDegrees(135.0f));
        }
        reference.renderInto(expected, 700, 1500);

        slot.noteOn();
        Buffer out{2, 1500};
        slot.process(out.block().subBlock(0, 700), 62.0f);
        slot.noteOn();
        slot.process(out.block().subBlock(700, 800), 62.0f);
        requireClose(out, expected, 1e-6);
    }
}

TEST_CASE("Unison: with Retrig off, each voice starts at its own seeded random phase",
          "[dsp][oscillator][unison][phase]")
{
    // Voices from silence draw their phases in voice order from the slot's
    // Random (inventory O10): reproducible with the seed, independent of
    // each other.
    auto slot = preparedSlot(Waveform::saw, 4);
    slot.setDetune(0.3f);
    slot.setStereo(1.0f);
    slot.setRetrigger(false);
    slot.setRandomSeed(77);
    slot.reset();
    Buffer probe{2, 1};
    slot.process(probe.block(), 55.0f);
    slot.stop();
    slot.setRandomSeed(77);

    Reference reference{slot, Waveform::saw};
    anoesis::dsp::Random random{77};
    for (auto& voice : reference.voices)
    {
        voice.restart(random.nextUint32());
    }
    Buffer expected{2, 1000};
    reference.renderInto(expected, 0, 1000);

    slot.noteOn();
    const auto out = render(slot, 55.0f, 1000);
    requireClose(out, expected, 1e-6);

    // A second note-on while sounding leaves every voice running.
    slot.noteOn();
    Buffer more{2, 500};
    Buffer moreExpected{2, 500};
    reference.renderInto(moreExpected, 0, 500);
    slot.process(more.block(), 55.0f);
    requireClose(more, moreExpected, 1e-6);

    // The same seed renders the same; another seed does not.
    const auto renderSeed = [](std::uint64_t seed)
    {
        auto s = preparedSlot(Waveform::saw, 8);
        s.setDetune(0.2f);
        s.setRetrigger(false);
        s.setRandomSeed(seed);
        s.noteOn();
        return render(s, 48.0f, 2000);
    };
    const auto a = renderSeed(5);
    const auto b = renderSeed(5);
    const auto c = renderSeed(6);
    CHECK(bitIdentical(a.channel(0), b.channel(0)));
    CHECK_FALSE(bitIdentical(a.channel(0), c.channel(0)));
}

TEST_CASE("Unison: free-running voices are uncorrelated; retriggered ones are identical",
          "[dsp][oscillator][unison][phase]")
{
    // Two voices, Detune 0, Stereo 10: one voice per channel, so the
    // channels' correlation is the voices'. For saws at a random phase
    // offset d, it is 1 - 6 d (1 - d), whose mean over uniform d is 0 and
    // standard deviation 0.447; over 64 seeds the mean has sigma 0.056.
    double sum = 0.0;
    double powerRatio = 0.0;
    constexpr int seeds = 64;
    for (int seed = 0; seed < seeds; ++seed)
    {
        auto slot = preparedSlot(Waveform::saw, 2);
        slot.setStereo(1.0f);
        slot.setRetrigger(false);
        slot.setRandomSeed(static_cast<std::uint64_t>(seed));
        slot.noteOn();
        const auto out = render(slot, 45.0f, 8192);
        const double r = correlation(out.channel(0).subspan(4096), out.channel(1).subspan(4096));
        CHECK(r < 0.9999); // never in step
        sum += r;

        // The level of 8 free-running voices: power N x one voice on average.
        auto eight = preparedSlot(Waveform::saw, 8);
        eight.setRetrigger(false);
        eight.setRandomSeed(static_cast<std::uint64_t>(seed));
        eight.noteOn();
        auto one = preparedSlot(Waveform::saw, 1);
        one.noteOn();
        powerRatio += meanSquare(render(eight, 45.0f, 8192).channel(0).subspan(4096)) /
                      meanSquare(render(one, 45.0f, 8192).channel(0).subspan(4096));
    }
    CHECK_THAT(sum / seeds, WithinAbs(0.0, 0.2));
    CHECK_THAT(powerRatio / seeds, WithinRel(8.0, 0.2));

    auto locked = preparedSlot(Waveform::saw, 2);
    locked.setStereo(1.0f);
    locked.noteOn();
    const auto out = render(locked, 45.0f, 4096);
    CHECK(bitIdentical(out.channel(0), out.channel(1)));
}

TEST_CASE("Unison: Noise is one source per slot, mixed to N noise voices' power and width",
          "[dsp][oscillator][unison][noise]")
{
    constexpr std::uint64_t seed = 31;
    const auto noiseSlot = [](int voices, float width, float pan)
    {
        auto slot = preparedSlot(Waveform::noise, voices);
        slot.setRandomSeed(seed);
        slot.setStereo(width);
        slot.setPan(pan);
        slot.reset();
        return slot;
    };

    SECTION("one voice hard left is the noise sequence, every other draw")
    {
        auto slot = noiseSlot(1, 0.0f, -1.0f);
        const auto out = render(slot, 60.0f, 1000);
        anoesis::dsp::Random draws{seed ^ Slot::noiseSeedSalt};
        for (std::size_t i = 0; i < out.numSamples(); ++i)
        {
            const float n1 = draws.nextBipolar();
            static_cast<void>(draws.nextBipolar());
            REQUIRE(sameBits(out.channel(0)[i], n1));
        }
        CHECK(allZero(out.channel(1)));
    }

    SECTION("power and correlation follow the voices")
    {
        // Uniform noise has power 1/3 per unit gain squared.
        struct Case
        {
            int voices;
            float width;
            float pan;
        };
        for (const auto& c : {
                 Case{.voices = 8, .width = 0.0f, .pan = 0.0f},
                 Case{.voices = 8, .width = 1.0f, .pan = 0.0f},
                 Case{.voices = 4, .width = 0.5f, .pan = 0.2f},
                 Case{.voices = 3, .width = 1.0f, .pan = 0.7f},
             })
        {
            CAPTURE(c.voices, c.width, c.pan);
            auto slot = noiseSlot(c.voices, c.width, c.pan);
            const auto out = render(slot, 60.0f, 65536);
            double left = 0.0;
            double right = 0.0;
            double cross = 0.0;
            for (std::size_t v = 0; v < slot.voices(); ++v)
            {
                const auto g = slot.voiceGains(v);
                left += static_cast<double>(g.left) * g.left;
                right += static_cast<double>(g.right) * g.right;
                cross += static_cast<double>(g.left) * g.right;
            }
            CHECK_THAT(meanSquare(out.channel(0)), WithinRel(left / 3.0, 0.03));
            CHECK_THAT(meanSquare(out.channel(1)), WithinRel(right / 3.0, 0.03));
            CHECK_THAT(correlation(out.channel(0), out.channel(1)), WithinAbs(cross / std::sqrt(left * right), 0.02));
        }
        // Stereo 0: both channels the same.
        auto mono = noiseSlot(8, 0.0f, 0.0f);
        const auto out = render(mono, 60.0f, 4096);
        CHECK(bitIdentical(out.channel(0), out.channel(1)));
        // Stereo alone, changed afterwards, widens it: 4 voices each side.
        mono.setStereo(1.0f);
        mono.reset();
        const auto wide = render(mono, 60.0f, 65536);
        CHECK_THAT(correlation(wide.channel(0), wide.channel(1)), WithinAbs(0.0, 0.02));
    }

    SECTION("Detune, Phase, Retrig, and note-ons do nothing; Inv negates; 0 voices is silence")
    {
        auto plain = noiseSlot(5, 0.7f, 0.1f);
        const auto expected = render(plain, 60.0f, 3000);

        auto other = noiseSlot(5, 0.7f, 0.1f);
        other.setDetune(2.0f);
        other.setPhaseDegrees(90.0f);
        other.setRetrigger(false);
        other.noteOn();
        Buffer out{2, 3000};
        other.process(out.block().subBlock(0, 1000), 72.0f);
        other.noteOn();
        other.process(out.block().subBlock(1000, 2000), 30.0f);
        CHECK(bitIdentical(out.channel(0), expected.channel(0)));
        CHECK(bitIdentical(out.channel(1), expected.channel(1)));

        auto inverted = noiseSlot(5, 0.7f, 0.1f);
        inverted.setInverted(true);
        inverted.reset();
        const auto negated = render(inverted, 60.0f, 3000);
        for (std::size_t i = 0; i < negated.numSamples(); ++i)
        {
            REQUIRE(sameBits(negated.channel(0)[i], -expected.channel(0)[i]));
            REQUIRE(sameBits(negated.channel(1)[i], -expected.channel(1)[i]));
        }

        auto off = noiseSlot(0, 0.7f, 0.1f);
        const auto silent = render(off, 60.0f, 1000);
        CHECK(allZero(silent.channel(0)));
        CHECK(allZero(silent.channel(1)));
    }
}

TEST_CASE("Unison: 0 voices is silence and renders nothing", "[dsp][oscillator][unison]")
{
    for (const auto shape : {Waveform::saw, Waveform::sine, Waveform::noise})
    {
        CAPTURE(shapeName(shape));
        auto slot = preparedSlot(shape, 0);
        slot.noteOn();
        const auto out = render(slot, 60.0f, 2048);
        CHECK(allZero(out.channel(0)));
        CHECK(allZero(out.channel(1)));
        CHECK(slot.voices() == 0);
        CHECK(slot.soundingVoices() == 0);
        for (std::size_t v = 0; v < allVoices; ++v)
        {
            CHECK_FALSE(slot.isVoiceRunning(v)); // nothing rendered
        }
    }
    // Negative counts are 0, and counts above 8 are 8.
    auto slot = preparedSlot(Waveform::saw, -3);
    CHECK(slot.voices() == 0);
    slot.setVoices(12);
    CHECK(slot.voices() == 8);
}

TEST_CASE("Unison: changing the voice count mid-note fades voices in and out, without clicks",
          "[dsp][oscillator][unison]")
{
    // A sum of sines at about 220 Hz has a second difference of at most
    // sum(gains) x w^2 (w = 0.029 rad per sample): about 6e-3 for 8 voices.
    // A click (a step of 0.01 or more) shows as a second difference of the
    // step's size. Every change below stays within the smooth bound.
    auto slot = preparedSlot(Waveform::sine, 4);
    slot.setDetune(0.3f);
    slot.setStereo(0.7f);
    slot.setRetrigger(false);
    slot.noteOn();
    Buffer out{2, 8000};
    const auto block = out.block();
    std::size_t sounding8 = 0;
    std::size_t soundingDuringFade = 0;
    std::size_t soundingAfterFade = 0;
    std::size_t runningAfterFade = 0;
    std::size_t runningAtZero = 0;
    const auto running = [&slot]
    {
        std::size_t count = 0;
        for (std::size_t v = 0; v < allVoices; ++v)
        {
            count += slot.isVoiceRunning(v) ? 1U : 0U;
        }
        return count;
    };
    std::size_t allocations = 1;
    {
        const AllocationCounter counter;
        slot.process(block.subBlock(0, 1000), 57.0f);
        slot.setVoices(8);
        sounding8 = slot.soundingVoices();
        slot.process(block.subBlock(1000, 2000), 57.0f);
        slot.setVoices(3);
        slot.process(block.subBlock(3000, 100), 57.0f);
        soundingDuringFade = slot.soundingVoices();
        slot.process(block.subBlock(3100, 1900), 57.0f);
        soundingAfterFade = slot.soundingVoices();
        runningAfterFade = running();
        slot.setVoices(0);
        slot.process(block.subBlock(5000, 1000), 57.0f);
        runningAtZero = running();
        slot.setVoices(6);
        slot.process(block.subBlock(6000, 2000), 57.0f);
        allocations = counter.count();
    }
    CHECK(allocations == 0);
    CHECK(sounding8 == 8);
    CHECK(soundingDuringFade == 8); // voices 3..7 fading out over 10 ms
    CHECK(soundingAfterFade == 3);
    CHECK(runningAfterFade == 3); // the faded voices' oscillators stopped
    CHECK(runningAtZero == 0);
    // From sample 1000: before it is the note's own onset from silence.
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        double worst = 0.0;
        for (std::size_t i = 1000; i < out.numSamples(); ++i)
        {
            const auto x = out.channel(ch);
            worst = std::max(worst, std::abs(static_cast<double>(x[i]) - (2.0 * x[i - 1]) + x[i - 2]));
        }
        CAPTURE(ch);
        CHECK(worst < 7e-3);
    }
    // 0 voices, after the fade, is exact silence.
    CHECK(allZero(out.channel(0).subspan(5600, 400)));
    CHECK(allZero(out.channel(1).subspan(5600, 400)));
    // The voices that came back sound.
    CHECK(meanSquare(out.channel(0).subspan(7000)) > 0.1);
}

TEST_CASE("Unison: a voice brought back while fading out keeps running, without a click", "[dsp][oscillator][unison]")
{
    // 8 -> 4 starts voices 4..7 fading; 4 -> 8 halfway through the fade
    // catches them: their oscillators carry on and their gains ramp back up.
    // Restarting them from silence would step each from 0 to its waveform at
    // half gain, a click.
    auto slot = preparedSlot(Waveform::sine, 8);
    slot.setDetune(0.3f);
    slot.setStereo(0.7f);
    slot.setRetrigger(false);
    slot.noteOn();
    Buffer out{2, 3000};
    slot.process(out.block().subBlock(0, 1000), 57.0f);
    slot.setVoices(4);
    slot.process(out.block().subBlock(1000, 240), 57.0f);
    slot.setVoices(8);
    slot.process(out.block().subBlock(1240, 1760), 57.0f);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        double worst = 0.0;
        const auto x = out.channel(ch);
        for (std::size_t i = 1000; i < out.numSamples(); ++i)
        {
            worst = std::max(worst, std::abs(static_cast<double>(x[i]) - (2.0 * x[i - 1]) + x[i - 2]));
        }
        CAPTURE(ch);
        CHECK(worst < 7e-3);
    }
    CHECK(slot.soundingVoices() == 8);
}

TEST_CASE("Unison: a voice count set before the note sounds is as if prepared with it", "[dsp][oscillator][unison]")
{
    // After the note-on but before its first sample nothing has been heard,
    // so the new voices start with the note (at Phase, or drawing the next
    // random phases in voice order, as a note-on does) and do not fade in.
    for (const bool retrigger : {true, false})
    {
        CAPTURE(retrigger);
        auto changed = preparedSlot(Waveform::saw, 2);
        changed.setRetrigger(retrigger);
        changed.setDetune(0.4f);
        changed.setStereo(0.5f);
        changed.noteOn();
        changed.setVoices(7); // after the note-on, before any sample
        auto fresh = preparedSlot(Waveform::saw, 7);
        fresh.setRetrigger(retrigger);
        fresh.setDetune(0.4f);
        fresh.setStereo(0.5f);
        fresh.reset();
        fresh.noteOn();
        const auto a = render(changed, 50.0f, 2000);
        const auto b = render(fresh, 50.0f, 2000);
        CHECK(bitIdentical(a.channel(0), b.channel(0)));
        CHECK(bitIdentical(a.channel(1), b.channel(1)));
    }
}

TEST_CASE("Unison: aliasing at 8 voices is no worse than one voice's", "[dsp][oscillator][unison][aliasing]")
{
    // Each voice is a band-limited oscillator and the stack is their sum, so
    // its aliases are the voices' own: the stack adds no new ones. Measured
    // below 20 kHz at 44.1 kHz with a Kaiser window (beta 20, main lobe
    // +-6.5 bins), masking +-8 bins around every harmonic of every voice.
    //
    // The voices are rendered alone as bare oscillators at the stack's
    // increments, gains, and phase (Retrig on, Phase 0), so the stack is
    // exactly their sum. Then, at every unmasked bin, the stack's power is at
    // most the coherent sum of the voices' (sum of amplitudes, squared): no
    // new components. Two voices' aliases can share a bin, and with the same
    // start phase add in phase, so the worst line may reach two of the
    // worst single-voice line: +6.02 dB. Measured: 0 dB at MIDI 100 and 108,
    // +2.3 dB at MIDI 90 (two lines at 13.35 kHz).
    constexpr double fs = 44100.0;
    constexpr std::size_t length = 65536;
    constexpr std::size_t settle = 4096;
    const auto window = kaiserWindow(length, 20.0);
    const auto maskHarmonics = [](const Spectrum& spectrum, double f0, std::vector<bool>& masked)
    {
        for (std::size_t harmonic = 1; static_cast<double>(harmonic) * f0 < fs / 2.0; ++harmonic)
        {
            const auto bin = spectrum.nearestBin(static_cast<double>(harmonic) * f0);
            for (std::size_t k = bin > 8 ? bin - 8 : 0; k <= std::min(bin + 8, masked.size() - 1); ++k)
            {
                masked[k] = true;
            }
        }
    };
    const auto dcMask = [](std::size_t size)
    {
        std::vector<bool> masked(size, false);
        for (std::size_t k = 0; k <= 8; ++k)
        {
            masked[k] = true;
        }
        return masked;
    };
    const auto worstOf = [](const Spectrum& spectrum, const std::vector<bool>& masked)
    {
        double worst = 0.0;
        for (std::size_t k = 0; k < spectrum.power.size() && spectrum.frequencyOf(k) < 20000.0; ++k)
        {
            if (!masked[k])
            {
                worst = std::max(worst, spectrum.power[k]);
            }
        }
        return worst;
    };
    for (const float note : {90.0f, 100.0f, 108.0f})
    {
        CAPTURE(note);
        auto stack = preparedSlot(Waveform::saw, 8, static_cast<float>(fs));
        stack.setDetune(0.5f);
        stack.noteOn();
        const auto many = render(stack, note, length + settle);
        const auto spectrum = powerSpectrum(many.channel(0).subspan(settle, length), fs, window);
        auto stackMask = dcMask(spectrum.power.size());

        std::vector<double> amplitudeSum(spectrum.power.size(), 0.0);
        double worstOne = 0.0;
        for (std::size_t v = 0; v < allVoices; ++v)
        {
            auto osc = Candidate<MinBlep64>::make();
            osc.prepare({.sampleRate = static_cast<float>(fs), .maxBlockSize = 512, .numChannels = 1});
            osc.setWaveform(Waveform::saw);
            osc.setIncrement(stack.phaseIncrement(v));
            osc.restart(0);
            std::vector<float> one(length + settle);
            for (auto& y : one)
            {
                y = stack.voiceGains(v).left * osc.next();
            }
            const auto alone = powerSpectrum(std::span<const float>{one}.subspan(settle, length), fs, window);
            const double f0 = incrementHz(stack.phaseIncrement(v), fs);
            auto ownMask = dcMask(alone.power.size());
            maskHarmonics(alone, f0, ownMask);
            maskHarmonics(spectrum, f0, stackMask);
            worstOne = std::max(worstOne, worstOf(alone, ownMask));
            for (std::size_t k = 0; k < alone.power.size(); ++k)
            {
                amplitudeSum[k] += std::sqrt(alone.power[k]);
            }
        }
        // Float rounding (each sample of the sum is rounded once more than
        // the voices') sits far below: allow -140 dB re the fundamental.
        const double fundamental = spectrum.powerAround(incrementHz(stack.phaseIncrement(0), fs), 8);
        for (std::size_t k = 0; k < spectrum.power.size() && spectrum.frequencyOf(k) < 20000.0; ++k)
        {
            if (!stackMask[k])
            {
                CAPTURE(k);
                REQUIRE(spectrum.power[k] <= (amplitudeSum[k] * amplitudeSum[k] * 1.001) + (1e-14 * fundamental));
            }
        }
        const double worstManyDb = 10.0 * std::log10(worstOf(spectrum, stackMask) / fundamental);
        const double worstOneDb = 10.0 * std::log10(worstOne / fundamental);
        CAPTURE(worstManyDb, worstOneDb);
        CHECK(worstManyDb <= worstOneDb + 6.03);
        CHECK(worstManyDb < -88.0); // D6: -91 dB for one voice over MIDI 0-127
    }
}

TEST_CASE("Unison: stable, bounded, and denormal-free at the extremes", "[dsp][oscillator][unison]")
{
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf = std::numeric_limits<float>::infinity();
    // 8 voices of the worst peak under jumps (2.61, oscillator-phase.md) at
    // unity gain on one side.
    constexpr float bound = 8.0f * 2.7f;
    for (const auto shape : {Waveform::saw, Waveform::hpulse, Waveform::sine, Waveform::noise})
    {
        for (const float spread : {0.0f, 3.3219f, 48.0f, -48.0f, 1e6f, nan, inf, -inf})
        {
            for (const float note : {-100.0f, 0.0f, 100.0f, 140.0f})
            {
                CAPTURE(shapeName(shape), spread, note);
                auto slot = preparedSlot(shape, 8);
                slot.setDetune(spread);
                slot.setStereo(1.0f);
                slot.setPan(1.0f);
                slot.setRetrigger(false);
                slot.noteOn();
                Buffer out{2, 3000};
                slot.process(out.block().subBlock(0, 1000), note);
                slot.noteOn();
                slot.setVoices(0);
                slot.process(out.block().subBlock(1000, 2000), note);
                for (std::size_t ch = 0; ch < 2; ++ch)
                {
                    for (const float y : out.channel(ch))
                    {
                        REQUIRE(std::isfinite(y));
                        REQUIRE(std::abs(y) <= bound);
                        REQUIRE(std::fpclassify(y) != FP_SUBNORMAL);
                    }
                    CHECK(allZero(out.channel(ch).subspan(2000)));
                }
            }
        }
    }
}

TEST_CASE("Unison: a pitch per sample equals the same constant pitch per block", "[dsp][oscillator][unison]")
{
    auto perBlock = preparedSlot(Waveform::trisaw, 6);
    auto perSample = preparedSlot(Waveform::trisaw, 6);
    for (auto* slot : {&perBlock, &perSample})
    {
        slot->setDetune(0.9f);
        slot->setStereo(0.4f);
        slot->noteOn();
    }
    const auto a = render(perBlock, 64.5f, 1500);
    Buffer b{2, 1500};
    const std::vector<float> notes(1500, 64.5f);
    perSample.process(b.block(), notes);
    CHECK(bitIdentical(a.channel(0), b.channel(0)));
    CHECK(bitIdentical(a.channel(1), b.channel(1)));
}

TEST_CASE("Unison: output does not depend on the block size", "[dsp][oscillator][unison]")
{
    const auto renderIn = [](std::size_t blockSize)
    {
        auto slot = preparedSlot(Waveform::saw, 5);
        slot.setDetune(0.5f);
        slot.setStereo(0.9f);
        slot.setRetrigger(false);
        slot.noteOn();
        Buffer out{2, 3000};
        for (std::size_t start = 0; start < 3000; start += blockSize)
        {
            if (start <= 1200 && 1200 < start + blockSize)
            {
                // Split at sample 1200 for a voice-count change there.
                slot.process(out.block().subBlock(start, 1200 - start), 52.0f);
                slot.setVoices(8);
                slot.process(out.block().subBlock(1200, start + blockSize - 1200), 52.0f);
                continue;
            }
            slot.process(out.block().subBlock(start, std::min(blockSize, 3000 - start)), 52.0f);
        }
        return out;
    };
    const auto expected = renderIn(3000);
    for (const std::size_t blockSize : {1U, 17U, 64U, 100U, 512U})
    {
        CAPTURE(blockSize);
        const auto out = renderIn(blockSize);
        CHECK(bitIdentical(out.channel(0), expected.channel(0)));
        CHECK(bitIdentical(out.channel(1), expected.channel(1)));
    }
}

TEST_CASE("Unison: noise does not depend on the block size, through 0 voices and back",
          "[dsp][oscillator][unison][noise]")
{
    // The noise stops drawing at the exact sample its mix reaches 0, so the
    // sequence after the voices come back is the same however the block is
    // split.
    const auto renderIn = [](std::size_t blockSize)
    {
        auto slot = preparedSlot(Waveform::noise, 4);
        slot.setStereo(0.6f);
        Buffer out{2, 3000};
        const auto block = out.block();
        const auto run = [&](std::size_t from, std::size_t to)
        {
            for (std::size_t start = from; start < to; start += blockSize)
            {
                slot.process(block.subBlock(start, std::min(blockSize, to - start)), 60.0f);
            }
        };
        run(0, 300);
        slot.setVoices(0);
        run(300, 2000);
        slot.setVoices(3);
        run(2000, 3000);
        return out;
    };
    const auto expected = renderIn(3000);
    for (const std::size_t blockSize : {1U, 7U, 64U, 100U, 512U})
    {
        CAPTURE(blockSize);
        const auto out = renderIn(blockSize);
        CHECK(bitIdentical(out.channel(0), expected.channel(0)));
        CHECK(bitIdentical(out.channel(1), expected.channel(1)));
    }
}

TEST_CASE("Unison: no allocation in any control, note-on, or process call", "[dsp][oscillator][unison]")
{
    auto slot = preparedSlot(Waveform::saw, 1);
    Buffer out{2, 512};
    const std::vector<float> notes(512, 60.0f);
    std::size_t allocations = 1;
    {
        const AllocationCounter counter;
        for (int voices = 0; voices <= 8; ++voices)
        {
            slot.setVoices(voices);
            slot.setDetune(0.1f * static_cast<float>(voices));
            slot.setStereo(0.1f * static_cast<float>(voices));
            slot.setPan(-0.5f);
            slot.setVolume(0.9f);
            slot.setInverted(voices % 2 == 0);
            slot.setRetrigger(voices % 3 != 0);
            slot.setPhaseDegrees(30.0f);
            slot.setWaveform(voices == 4 ? Waveform::noise : Waveform::pulse);
            slot.noteOn();
            slot.process(out.block(), 60.0f);
            slot.process(out.block(), notes);
        }
        slot.stop();
        slot.reset();
        slot.prepare(specAt(rate));
        allocations = counter.count();
    }
    CHECK(allocations == 0);
}

TEST_CASE("Unison: OscillatorSlot honours the lifecycle contract", "[dsp][oscillator][unison]")
{
    // 8 voices with detune and stereo, note-ons every 300 samples, the voice
    // count changing every 700, retriggered or free-running, and noise.
    struct UnisonWithNotes
    {
        Slot slot{minBlepTable64()};
        std::size_t untilNote = 0;
        std::size_t sample = 0;
        UnisonWithNotes(Waveform shape, bool retrigger) noexcept
        {
            slot.setWaveform(shape);
            slot.setVoices(8);
            slot.setDetune(0.6f);
            slot.setStereo(0.8f);
            slot.setPan(0.2f);
            slot.setRetrigger(retrigger);
            slot.setPhaseDegrees(60.0f);
            slot.setRandomSeed(9);
        }
        void prepare(const ProcessSpec& spec) noexcept
        {
            slot.setVoices(8);
            slot.prepare(spec);
            untilNote = 0;
            sample = 0;
        }
        void reset() noexcept
        {
            slot.setVoices(8);
            slot.reset();
            untilNote = 0;
            sample = 0;
        }
        void process(const AudioBlock& block) noexcept
        {
            for (std::size_t i = 0; i < block.numSamples(); ++i, ++sample)
            {
                if (untilNote == 0)
                {
                    slot.noteOn();
                    untilNote = 300;
                }
                if (sample % 700 == 699)
                {
                    slot.setVoices(static_cast<int>((sample / 700) % 9));
                }
                --untilNote;
                slot.process(block.subBlock(i, 1), 50.0f);
            }
        }
    };
    for (const auto shape : {Waveform::saw, Waveform::noise})
    {
        for (const bool retrigger : {true, false})
        {
            CAPTURE(shapeName(shape), retrigger);
            const auto result = checkLifecycleContract([shape, retrigger] { return UnisonWithNotes{shape, retrigger}; },
                                                       specAt(rate), 3000);
            CHECK(result.resetMatchesFreshPrepare);
            CHECK(result.rePrepareMatchesFreshPrepare);
        }
    }
}
