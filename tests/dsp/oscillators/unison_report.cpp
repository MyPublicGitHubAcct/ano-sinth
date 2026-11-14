// The unison report: each voice's pitch, the stereo image and how Pan
// combines with Stereo (O8), the level against the voice count, the
// headroom, aliasing at 8 voices, and noise, measured through OscillatorSlot
// and printed as Markdown. Hidden from the default run; run it in an
// optimised build (a few seconds):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[unison-report]"
//
// Results: docs/dsp/oscillator-unison.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>
#include <anoesis/test/spectrum.hpp>
#include <anoesis/test/window.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <span>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using anoesis::dsp::maxUnisonVoices;
using anoesis::dsp::unisonLayout;

namespace
{

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

constexpr float rate = 48000.0f;

Slot preparedSlot(Waveform shape, int voices, float sampleRate = rate)
{
    Slot slot{minBlepTable64()};
    slot.setVoices(voices);
    slot.prepare({.sampleRate = sampleRate, .maxBlockSize = 512, .numChannels = 2});
    slot.setWaveform(shape);
    return slot;
}

Buffer render(Slot& slot, float note, std::size_t numSamples)
{
    Buffer out{2, numSamples};
    for (std::size_t start = 0; start < numSamples; start += 512)
    {
        slot.process(out.block().subBlock(start, std::min<std::size_t>(512, numSamples - start)), note);
    }
    return out;
}

double incrementHz(std::uint32_t increment, double sampleRate)
{
    return static_cast<double>(increment) * sampleRate / 4294967296.0;
}

double meanSquare(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v) * v;
    }
    return sum / static_cast<double>(x.size());
}

double peakOf(std::span<const float> x)
{
    double peak = 0.0;
    for (const float v : x)
    {
        peak = std::max(peak, static_cast<double>(std::abs(v)));
    }
    return peak;
}

double toDbPower(double ratio)
{
    return 10.0 * std::log10(ratio);
}

// The interpolated frequency and the amplitude of the spectral line nearest
// `hz` (see unison_slot_test.cpp's findPeaks).
struct Line
{
    double hz = 0.0;
    double amplitude = 0.0;
};

Line lineNear(const Spectrum& spectrum, double hz)
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
    return {
        .hz = (static_cast<double>(best) + offset) * spectrum.binWidth(),
        .amplitude = std::sqrt(2.0 * spectrum.powerAround(spectrum.frequencyOf(best), spectrum.mainLobeBins)),
    };
}

void reportPitch()
{
    // The provisional knob law (decided 2026-11-14, inventory O13; it lives
    // in the parameter table): outer = 3.3219 x^2 x g(N) semitones, g from
    // 1 at 2 voices to 0.66 at 8, linear in N.
    std::cout << "\n### Voice pitches\n\n"
              << "48 kHz, sines at A2 (110 Hz), Retrig off. The spread is the provisional knob law at Detune 10, "
                 "3.3219 x g(N) semitones. Each voice's line measured in a 2^18-point Blackman-Harris FFT "
                 "(interpolated) against its target in double; the increment error is the fixed-point pitch the "
                 "voice plays.\n\n"
              << "| Voices | g(N) | Spread (st) | Outer voice (Hz) | Offsets (st, lowest first) | "
                 "Worst increment error (cents) | Worst measured error (cents) |\n"
              << "|---:|---:|---:|---:|---|---:|---:|\n";
    constexpr float note = 45.0f;
    constexpr std::size_t length = 262144;
    const auto window = blackmanHarrisWindow(length);
    for (std::size_t count = 1; count <= maxUnisonVoices; ++count)
    {
        const double g = count < 2 ? 1.0 : 1.0 - ((1.0 - 0.66) * static_cast<double>(count - 2) / 6.0);
        const auto spread = static_cast<float>(3.3219 * g);
        auto slot = preparedSlot(Waveform::sine, static_cast<int>(count));
        slot.setDetune(spread);
        slot.setRetrigger(false);
        slot.setRandomSeed(count);
        slot.noteOn();
        const auto out = render(slot, note, length + 4096);
        const auto spectrum = powerSpectrum(out.channel(0).subspan(4096, length), rate, window);
        double worstIncrement = 0.0;
        double worstMeasured = 0.0;
        std::cout << std::fixed << std::setprecision(2) << "| " << count << " | " << g << " | " << spread << " | ";
        std::cout << incrementHz(slot.phaseIncrement(count - 1), rate) << " | ";
        for (std::size_t v = 0; v < count; ++v)
        {
            const double offset = static_cast<double>(spread) * static_cast<double>(unisonLayout(count).detune[v]);
            const double target = 110.0 * std::exp2(offset / 12.0);
            const double played = incrementHz(slot.phaseIncrement(v), rate);
            worstIncrement = std::max(worstIncrement, std::abs(1200.0 * std::log2(played / target)));
            const auto line = lineNear(spectrum, played);
            worstMeasured = std::max(worstMeasured, std::abs(1200.0 * std::log2(line.hz / target)));
            std::cout << (v > 0 ? ", " : "") << std::showpos << offset << std::noshowpos;
        }
        std::cout << std::setprecision(5) << " | " << worstIncrement << " | " << worstMeasured << " |\n";
    }
}

void reportStereo()
{
    std::cout << "\n### Stereo image\n\n"
              << "Each voice's left and right amplitude, measured from its line in the spectrum (sines, 8 voices, "
                 "Detune 1.5 st, Pan centre), against the equal-power law at Pan + width x side.\n\n"
              << "| Width | Sides (lowest first) | Left amplitudes | Right amplitudes | Worst error |\n"
              << "|---:|---|---|---|---:|\n";
    constexpr std::size_t length = 131072;
    const auto window = blackmanHarrisWindow(length);
    for (const float width : {0.0f, 0.5f, 1.0f})
    {
        auto slot = preparedSlot(Waveform::sine, 8);
        slot.setDetune(1.5f);
        slot.setStereo(width);
        slot.setRetrigger(false);
        slot.noteOn();
        const auto out = render(slot, 69.0f, length + 4096);
        const auto left = powerSpectrum(out.channel(0).subspan(4096, length), rate, window);
        const auto right = powerSpectrum(out.channel(1).subspan(4096, length), rate, window);
        std::cout << std::setprecision(1) << "| " << width << " | ";
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            const float side = unisonLayout(8).side[v];
            char letter = 'C';
            if (side < 0.0f)
            {
                letter = 'L';
            }
            else if (side > 0.0f)
            {
                letter = 'R';
            }
            std::cout << letter;
        }
        double worst = 0.0;
        std::cout << std::setprecision(3) << " | ";
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            const auto line = lineNear(left, incrementHz(slot.phaseIncrement(v), rate));
            worst = std::max(worst, std::abs(line.amplitude - slot.voiceGains(v).left));
            std::cout << (v > 0 ? ", " : "") << line.amplitude;
        }
        std::cout << " | ";
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
        {
            const auto line = lineNear(right, incrementHz(slot.phaseIncrement(v), rate));
            worst = std::max(worst, std::abs(line.amplitude - slot.voiceGains(v).right));
            std::cout << (v > 0 ? ", " : "") << line.amplitude;
        }
        std::cout << std::scientific << std::setprecision(1) << " | " << worst << std::fixed << " |\n";
    }

    std::cout << "\n**Pan with Stereo (O8: an offset, clipped).** 2 voices, Stereo 10. The lower voice is on the "
                 "left, the upper on the right. Gains as set; the M1.13 measurement reads the same four numbers "
                 "from the demo.\n\n"
              << "| Pan | Lower voice L / R | Upper voice L / R | Balance (b) would give, lower | "
                 "Squeeze (c) would give, lower |\n"
              << "|---:|---|---|---|---|\n";
    for (const float pan : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f})
    {
        auto slot = preparedSlot(Waveform::saw, 2);
        slot.setStereo(1.0f);
        slot.setPan(pan);
        const auto lower = slot.voiceGains(0);
        const auto upper = slot.voiceGains(1);
        const auto balance = anoesis::dsp::panGains(pan, anoesis::dsp::PanLaw::balance);
        const auto squeeze = anoesis::dsp::panGains(pan - (1.0f - std::abs(pan)));
        std::cout << std::setprecision(1) << "| " << pan << std::setprecision(3) << " | " << lower.left << " / "
                  << lower.right << " | " << upper.left << " / " << upper.right << " | " << balance.left << " / 0 | "
                  << squeeze.left << " / " << squeeze.right << " |\n";
    }
}

void reportLevel()
{
    std::cout
        << "\n### Level against the voice count\n\n"
        << "48 kHz, saw at A1 (55 Hz), Stereo 0, Pan centre, Volume 1, after 4096 samples. In phase: Retrig "
           "on, Phase 0, Detune 0. Free-running: Retrig off, Detune 0.3 st, the mean power over 32 seeds. "
           "dB relative to 1 voice. No normalisation predicts +6.02 dB per doubling in phase and +3.01 dB "
           "free-running.\n\n"
        << "| Voices | In phase: RMS (dB) | In phase: peak | Free-running: RMS (dB) | Free-running: worst peak |\n"
        << "|---:|---:|---:|---:|---:|\n";
    constexpr std::size_t length = 16384;
    double onePower = 0.0;
    double oneFreePower = 0.0;
    for (const int count : {1, 2, 3, 4, 6, 8})
    {
        auto locked = preparedSlot(Waveform::saw, count);
        locked.noteOn();
        const auto out = render(locked, 33.0f, length + 4096);
        const auto lockedChannel = out.channel(0).subspan(4096);
        const double power = meanSquare(lockedChannel);
        double freePower = 0.0;
        double freePeak = 0.0;
        for (std::uint64_t seed = 0; seed < 32; ++seed)
        {
            auto free = preparedSlot(Waveform::saw, count);
            free.setDetune(0.3f);
            free.setRetrigger(false);
            free.setRandomSeed(seed);
            free.noteOn();
            const auto freeOut = render(free, 33.0f, length + 4096);
            freePower += meanSquare(freeOut.channel(0).subspan(4096)) / 32.0;
            freePeak = std::max(freePeak, peakOf(freeOut.channel(0).subspan(4096)));
        }
        if (count == 1)
        {
            onePower = power;
            oneFreePower = freePower;
        }
        std::cout << std::fixed << std::setprecision(2) << "| " << count << " | " << std::showpos
                  << toDbPower(power / onePower) << std::noshowpos << " | " << peakOf(lockedChannel) << " | "
                  << std::showpos << toDbPower(freePower / oneFreePower) << std::noshowpos << " | " << freePeak
                  << " |\n";
    }

    std::cout << "\n**Headroom.** Peak of one channel, 8 voices in phase (Retrig on, Detune 0), Volume 1, over 4096 "
                 "samples after the onset. Hard pan puts every voice at unity on one side.\n\n"
              << "| Waveform | Note | Pan centre | Pan hard right |\n|---|---:|---:|---:|\n";
    for (const auto shape : {Waveform::saw, Waveform::pulse, Waveform::hpulse, Waveform::qpulse, Waveform::trisaw})
    {
        for (const float note : {33.0f, 96.0f, 120.0f})
        {
            std::cout << "| " << shapeName(shape) << " | " << std::setprecision(0) << note << " |"
                      << std::setprecision(2);
            for (const float pan : {0.0f, 1.0f})
            {
                auto slot = preparedSlot(shape, 8);
                slot.setPan(pan);
                slot.noteOn();
                const auto out = render(slot, note, 4096);
                std::cout << " " << peakOf(out.channel(1)) << " |";
            }
            std::cout << "\n";
        }
    }
}

void reportAliasing()
{
    std::cout << "\n### Aliasing at 8 voices\n\n"
              << "44.1 kHz, saw, Detune 0.5 st, Stereo 0, Retrig on: the worst non-harmonic line below 20 kHz "
                 "(Kaiser beta 20, 2^16 points, +-8 bins masked around every harmonic of every voice), dB relative "
                 "to one voice's fundamental. The single-voice column is the worst of the eight voices rendered "
                 "alone.\n\n"
              << "| Note | Worst single voice (dB) | 8 voices (dB) | Difference |\n|---:|---:|---:|---:|\n";
    constexpr double fs = 44100.0;
    constexpr std::size_t length = 65536;
    constexpr std::size_t settle = 4096;
    const auto window = kaiserWindow(length, 20.0);
    const auto mask = [](const Spectrum& spectrum, double f0, std::vector<bool>& masked)
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
    const auto worstOf = [](const Spectrum& spectrum, const std::vector<bool>& masked)
    {
        double worst = 0.0;
        for (std::size_t k = 9; k < spectrum.power.size() && spectrum.frequencyOf(k) < 20000.0; ++k)
        {
            if (!masked[k])
            {
                worst = std::max(worst, spectrum.power[k]);
            }
        }
        return worst;
    };
    for (const float note : {60.0f, 84.0f, 96.0f, 102.0f, 108.0f, 114.0f})
    {
        auto stack = preparedSlot(Waveform::saw, 8, static_cast<float>(fs));
        stack.setDetune(0.5f);
        stack.noteOn();
        const auto many = render(stack, note, length + settle);
        const auto spectrum = powerSpectrum(many.channel(0).subspan(settle, length), fs, window);
        std::vector<bool> stackMask(spectrum.power.size(), false);
        double worstOne = 0.0;
        for (std::size_t v = 0; v < maxUnisonVoices; ++v)
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
            std::vector<bool> ownMask(alone.power.size(), false);
            const double f0 = incrementHz(stack.phaseIncrement(v), fs);
            mask(alone, f0, ownMask);
            mask(spectrum, f0, stackMask);
            worstOne = std::max(worstOne, worstOf(alone, ownMask));
        }
        const double fundamental = spectrum.powerAround(incrementHz(stack.phaseIncrement(0), fs), 8);
        const double one = toDbPower(worstOne / fundamental);
        const double many8 = toDbPower(worstOf(spectrum, stackMask) / fundamental);
        std::cout << std::setprecision(0) << "| " << note << std::setprecision(1) << " | " << one << " | " << many8
                  << " | " << std::showpos << many8 - one << std::noshowpos << " |\n";
    }
}

void reportNoise()
{
    std::cout << "\n### Noise\n\n"
              << "One source per slot, 2^17 samples at 48 kHz, Pan centre. RMS of each channel against N uniform "
                 "noise voices at the voices' gains (sqrt(sum of gains^2 / 3)), and the left/right correlation "
                 "against theirs.\n\n"
              << "| Voices | Width | RMS left | Expected | RMS right | Correlation | Expected |\n"
              << "|---:|---:|---:|---:|---:|---:|---:|\n";
    constexpr std::size_t length = 131072;
    for (const int count : {1, 2, 4, 8})
    {
        for (const float width : {0.0f, 0.5f, 1.0f})
        {
            auto slot = preparedSlot(Waveform::noise, count);
            slot.setStereo(width);
            slot.reset();
            const auto out = render(slot, 60.0f, length);
            double l2 = 0.0;
            double r2 = 0.0;
            double lr = 0.0;
            for (std::size_t v = 0; v < slot.voices(); ++v)
            {
                const auto g = slot.voiceGains(v);
                l2 += static_cast<double>(g.left) * g.left;
                r2 += static_cast<double>(g.right) * g.right;
                lr += static_cast<double>(g.left) * g.right;
            }
            double ab = 0.0;
            for (std::size_t i = 0; i < length; ++i)
            {
                ab += static_cast<double>(out.channel(0)[i]) * out.channel(1)[i];
            }
            const double left = meanSquare(out.channel(0));
            const double right = meanSquare(out.channel(1));
            const double measuredCorrelation = ab / static_cast<double>(length) / std::sqrt(left * right);
            std::cout << std::setprecision(1) << "| " << count << " | " << width << std::setprecision(3) << " | "
                      << std::sqrt(left) << " | " << std::sqrt(l2 / 3.0) << " | " << std::sqrt(right) << " | "
                      << measuredCorrelation << " | " << lr / std::sqrt(l2 * r2) << " |\n";
        }
    }
}

} // namespace

TEST_CASE("Unison report: pitches, stereo, level, aliasing, noise", "[.][unison-report]")
{
    reportPitch();
    reportStereo();
    reportLevel();
    reportAliasing();
    reportNoise();
    SUCCEED();
}
