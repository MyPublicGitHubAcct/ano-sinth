// The range report: the oscillator from 0.01 Hz to beyond Nyquist, as built.
// Above Nyquist and through the fade band (every waveform's level and DC,
// sweeps and a vibrato across Nyquist, a saturator downstream); the bottom
// of the range (period and phase at 0.01 Hz at every rate, and the error of
// the float evaluation there); and the peaks under the stress run that the
// range test bounds. Hidden from the default run; run it in an optimised
// build (about 10 s):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[range-report]"
//
// The long runs (12.4 hours of simulated time at 0.01 Hz, one hour near
// Nyquist) are a separate hidden case, about a minute:
//
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[range-long-report]"
//
// The options for the behaviour above Nyquist, prototyped against the clamp
// that came before, are measured by this report at commit 9fef36a.
// Results: docs/dsp/oscillator-range.md

#include "range_support.hpp"

#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>

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
#include <numbers>
#include <span>
#include <sstream>
#include <string>
#include <vector>

using namespace anoesis::test;
using namespace anoesis::test::d6;
using namespace anoesis::test::range;
using anoesis::dsp::incrementFor;
using anoesis::dsp::waveShape;

namespace
{

constexpr double rate = 48000.0;

double meanOf(std::span<const float> x)
{
    return std::accumulate(x.begin(), x.end(), 0.0) / static_cast<double>(x.size());
}

double rmsAbout(std::span<const float> x, double centre)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += (v - centre) * (v - centre);
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

std::string db(double ratio)
{
    std::ostringstream s;
    if (ratio <= 0.0)
    {
        s << "-inf";
    }
    else
    {
        s << std::fixed << std::setprecision(1) << 20.0 * std::log10(ratio);
    }
    return s.str();
}

std::string fixed(double x, int digits = 3)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(digits) << x;
    return s.str();
}

std::string scientific(double x, int digits = 1)
{
    std::ostringstream s;
    s << std::scientific << std::setprecision(digits) << x;
    return s.str();
}

std::vector<float> renderPath(Waveform shape, std::span<const double> cycles, std::uint32_t startPhase,
                              double sampleRate = rate)
{
    auto osc = preparedOscillator(shape, sampleRate);
    osc.restart(startPhase);
    std::vector<float> y(cycles.size());
    for (std::size_t i = 0; i < cycles.size(); ++i)
    {
        osc.setFrequency(static_cast<float>(cycles[i] * sampleRate));
        y[i] = osc.next();
    }
    return y;
}

// --- Above Nyquist and the fade band ----------------------------------------

void reportAboveNyquist(std::ostream& out)
{
    out << "\n## Above Nyquist\n\n"
           "Each waveform at 0.55 fs, after 0.3 fs, from 256 start phases; 4096 samples from the crossing. "
           "Largest |output - mean| over all of them: 0 means the output is exactly the mean, every sample.\n\n"
           "| Waveform | mean() | Output DC | Largest deviation from the mean |\n|---|---:|---:|---:|\n";
    for (const auto shape : periodicShapes)
    {
        double worst = 0.0;
        double dc = 0.0;
        for (std::uint32_t k = 0; k < 256; ++k)
        {
            auto osc = preparedOscillator(shape);
            osc.restart(k << 24U);
            osc.setFrequency(static_cast<float>(0.3 * rate));
            for (int i = 0; i < 200; ++i)
            {
                static_cast<void>(osc.next());
            }
            osc.setFrequency(static_cast<float>(0.55 * rate));
            std::vector<float> y(4096);
            std::ranges::generate(y, [&] { return osc.next(); });
            const double mean = waveShape(shape).mean();
            dc = meanOf(y);
            for (const float v : y)
            {
                worst = std::max(worst, std::abs(v - mean));
            }
        }
        out << "| " << shapeName(shape) << " | " << fixed(waveShape(shape).mean(), 3) << " | " << fixed(dc, 3) << " | "
            << worst << " |\n";
    }
}

void reportLevelNearNyquist(std::ostream& out)
{
    const std::array fractions{0.30, 0.40, 0.45, 0.46, 0.47, 0.475, 0.48, 0.49, 0.495, 0.499};
    out << "\n## Level through the fade band\n\n"
           "RMS about the mean relative to the ideal fundamental's RMS (above fs/4 the band-limited waveform is "
           "its fundamental alone), 48000 samples, in dB. *Fade* is the fade's gain alone: the sine, which the "
           "kernel does not filter, follows it; the others are the kernel's roll-off times the fade.\n\n"
           "| Waveform |";
    for (const double f : fractions)
    {
        out << " " << f << " fs |";
    }
    out << "\n|---|";
    for (std::size_t i = 0; i < fractions.size(); ++i)
    {
        out << "---:|";
    }
    out << "\n| *Fade* |";
    for (const double f : fractions)
    {
        out << " " << db(anoesis::dsp::nyquistFadeGain(static_cast<std::uint32_t>(f * unitsPerCycle))) << " |";
    }
    out << "\n";
    for (const auto shape : periodicShapes)
    {
        out << "| " << shapeName(shape) << " |";
        const double ideal = idealHarmonic(shape, 1) / std::numbers::sqrt2;
        for (const double f : fractions)
        {
            auto osc = preparedOscillator(shape);
            osc.setFrequency(static_cast<float>(f * rate));
            std::vector<float> y(256 + 48000);
            std::ranges::generate(y, [&] { return osc.next(); });
            const std::span tail{y.data() + 256, 48000};
            out << " " << db(rmsAbout(tail, meanOf(tail)) / ideal) << " |";
        }
        out << "\n";
    }
}

void reportPaths(std::ostream& out)
{
    out << "\n## Sweeps and a vibrato across Nyquist\n\n"
           "The range test's two paths at 48 kHz, "
        << nyquistPathLength
        << " samples each: a sweep from 0.40 fs to 0.60 fs and back, linear in pitch, and a 13 Hz vibrato of +-1 "
           "semitone around 0.485 fs. Power from 10 Hz to 18 kHz about the mean, relative to a full-scale sine "
           "(the tone itself is never below 18 kHz), worst of 16 start phases; the range test allows 6 dB more. "
           "*Above Nyquist*: the largest |output - mean| once the target has been above fs/2 for 10 ms.\n\n"
           "| Waveform | Sweep: 10 Hz-18 kHz | Vibrato: 10 Hz-18 kHz | Above Nyquist |\n|---|---:|---:|---:|\n";
    const auto sweep = sweepThroughNyquist(nyquistPathLength);
    const auto vibrato = vibratoAcrossNyquist(nyquistPathLength, rate);
    for (const auto shape : periodicShapes)
    {
        double worstSweep = -400.0;
        double worstVibrato = -400.0;
        double above = 0.0;
        for (std::uint32_t k = 0; k < 16; ++k)
        {
            const auto a = renderPath(shape, sweep, k << 28U);
            const auto b = renderPath(shape, vibrato, k << 28U);
            worstSweep = std::max(worstSweep, powerBelowDb(a, 18000.0, rate));
            worstVibrato = std::max(worstVibrato, powerBelowDb(b, 18000.0, rate));
            std::size_t aboveFor = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                aboveFor = sweep[i] >= 0.5 ? aboveFor + 1 : 0;
                if (aboveFor > 480)
                {
                    above = std::max(above, static_cast<double>(std::abs(a[i] - waveShape(shape).mean())));
                }
            }
        }
        out << "| " << shapeName(shape) << " | " << fixed(worstSweep, 1) << " dB | " << fixed(worstVibrato, 1)
            << " dB | " << above << " |\n";
    }
}

// A 220 Hz saw at full scale plus one voice whose target is above Nyquist (or
// in the fade band), through tanh(drive x) at 4x oversampling: the power that
// lands in the audible band and is not a harmonic of 220 Hz.
void reportSaturator(std::ostream& out)
{
    constexpr std::size_t n = 65536;
    constexpr double noteHz = 220.0;
    anoesis::dsp::Oversampler oversampler;
    oversampler.setFactor(anoesis::dsp::OversamplingFactor::four);
    oversampler.prepare({.sampleRate = static_cast<float>(rate), .maxBlockSize = 512, .numChannels = 1});
    const auto saturate = [&](std::vector<float> x, float drive)
    {
        oversampler.reset();
        for (std::size_t start = 0; start < x.size(); start += 512)
        {
            Buffer block{1, 512};
            std::copy_n(x.begin() + static_cast<std::ptrdiff_t>(start), 512, block.channel(0).begin());
            const auto inner = oversampler.upsample(block.block());
            for (auto& s : inner.channel(0))
            {
                s = std::tanh(drive * s);
            }
            oversampler.downsample(block.block());
            std::copy_n(block.channel(0).begin(), 512, x.begin() + static_cast<std::ptrdiff_t>(start));
        }
        return x;
    };
    const auto inharmonicDb = [&](std::span<const float> y)
    {
        const std::span tail{y.data() + 4096, n - 8192};
        const auto spectrum = averagedPowerSpectrum(tail, rate, blackmanHarrisWindow(8192));
        const double fundamental = spectrum.powerAround(noteHz, spectrum.mainLobeBins);
        double other = 0.0;
        for (std::size_t k = 1; k < spectrum.power.size() && spectrum.frequencyOf(k) < 20000.0; ++k)
        {
            const double f = spectrum.frequencyOf(k);
            const double harmonic = std::round(f / noteHz) * noteHz;
            if (std::abs(f - harmonic) > static_cast<double>(spectrum.mainLobeBins + 1) * spectrum.binWidth())
            {
                other += spectrum.power[k];
            }
        }
        return powerToDb(other / fundamental);
    };
    const std::vector<double> noteCycles(n, noteHz / rate);
    const auto note = renderPath(Waveform::saw, noteCycles, 0);

    out << "\n## Downstream: a driven saturator at 4x oversampling\n\n"
           "A 220 Hz saw at full scale plus one voice, summed and driven through tanh(drive x) at 4x oversampling "
           "(the library's Oversampler), 48 kHz. The power below 20 kHz that is not a harmonic of 220 Hz, "
           "relative to the 220 Hz fundamental. The clamp, measured at commit 9fef36a, gave -25 to -65 dB here.\n\n"
           "| Upper voice | Drive 1 | Drive 4 | Drive 16 |\n|---|---:|---:|---:|\n";
    struct Row
    {
        const char* name;
        Waveform shape;
        double cycles;
    };
    for (const auto& row : {
             Row{.name = "None (the note alone)", .shape = Waveform::saw, .cycles = 0.0},
             Row{.name = "Saw at 0.55 fs", .shape = Waveform::saw, .cycles = 0.55},
             Row{.name = "HPulse at 0.55 fs", .shape = Waveform::hpulse, .cycles = 0.55},
             Row{.name = "Sine at 0.55 fs", .shape = Waveform::sine, .cycles = 0.55},
             Row{.name = "Sine at 0.47 fs (in the band)", .shape = Waveform::sine, .cycles = 0.47},
             Row{.name = "Sine at 0.44 fs (below it)", .shape = Waveform::sine, .cycles = 0.44},
         })
    {
        out << "| " << row.name << " |";
        std::vector<float> mix = note;
        if (row.cycles > 0.0)
        {
            const std::vector<double> cycles(n, row.cycles);
            const auto upper = renderPath(row.shape, cycles, 0x40000000U);
            for (std::size_t i = 0; i < n; ++i)
            {
                mix[i] += upper[i];
            }
        }
        for (const float drive : {1.0f, 4.0f, 16.0f})
        {
            out << " " << fixed(inharmonicDb(saturate(mix, drive)), 1) << " dB |";
        }
        out << "\n";
    }
}

// --- The bottom of the range --------------------------------------------------

void reportBottom(std::ostream& out)
{
    out << "\n## Period and phase at 0.01 Hz, through the slot\n\n"
           "A sine at note "
        << fixed(noteOf(0.01), 4)
        << " (0.01 Hz at A4 = 440 Hz), Retrig on at Phase 359, hard left, rendered for 1.01 cycles in blocks of "
           "4096: its two upward zero crossings (linear interpolation), against where the integer phase puts "
           "them (the first after 1/360 cycle plus the kernel's delay, the second one period later). *Floor*: one "
           "fixed-point unit, the most the truncated increment can be flat.\n\n"
           "| Rate | Increment | Period (samples) | First crossing: error | Second: error | Measured pitch | "
           "Error | Floor |\n|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    for (const double sampleRate : rates)
    {
        auto slot = preparedSlot(Waveform::sine, 1, sampleRate);
        slot.setPan(-1.0f);
        slot.setPhaseDegrees(359.0f);
        slot.noteOn();
        const auto note = static_cast<float>(noteOf(0.01));
        const std::uint32_t increment = incrementFor(slot.frequencyFor(note), static_cast<float>(sampleRate));
        const double period = unitsPerCycle / static_cast<double>(increment);
        const auto length = static_cast<std::size_t>(period * 1.01);
        Buffer buffer{2, length};
        for (std::size_t start = 0; start < length; start += 4096)
        {
            slot.process(buffer.block().subBlock(start, std::min<std::size_t>(4096, length - start)), note);
        }
        const auto y = buffer.channel(0);
        std::vector<double> crossings;
        for (std::size_t i = 1; i < length; ++i)
        {
            if (y[i - 1] < 0.0f && y[i] >= 0.0f)
            {
                crossings.push_back(static_cast<double>(i - 1) + (y[i - 1] / (y[i - 1] - y[i])));
            }
        }
        const auto startPhase = static_cast<double>(anoesis::dsp::phaseFromDegrees(359.0f));
        const double first = ((unitsPerCycle - startPhase) / static_cast<double>(increment)) + slot.latencySamples();
        const double measured = sampleRate / (crossings.at(1) - crossings.at(0));
        const double floorCents = 1200.0 * std::log2(1.0 + (sampleRate / unitsPerCycle / 0.01));
        out << "| " << sampleRate << " | " << increment << " | " << fixed(period, 1) << " | "
            << fixed(crossings.at(0) - first, 3) << " | " << fixed(crossings.at(1) - first - period, 3) << " | "
            << std::setprecision(9) << measured << " Hz | " << fixed(1200.0 * std::log2(measured / 0.01), 3)
            << " cents | " << fixed(floorCents, 3) << " cents |\n";
    }
}

// The output at 0.01 Hz against the waveform evaluated in double at the
// exact delayed phase: what float evaluation of a phase near 1 costs.
void reportFloatResolution(std::ostream& out)
{
    out << "\n## Float evaluation at 0.01 Hz\n\n"
           "Bare oscillator at 0.01 Hz, 48 kHz (increment 894), one whole cycle (4.8 million samples) skipping 64 "
           "samples around each corner: the largest |output - ideal|, the ideal evaluated in double at the exact "
           "delayed phase (phase - delay x increment, from the integer phase).\n\n"
           "| Waveform | Largest error | In dB re full scale |\n|---|---:|---:|\n";
    for (const auto shape : {Waveform::saw, Waveform::triangle, Waveform::sine, Waveform::trisaw})
    {
        auto osc = preparedOscillator(shape);
        osc.setFrequency(0.01f);
        const auto increment = static_cast<double>(osc.phaseIncrement());
        const double delay = osc.latencySamples();
        const auto length = static_cast<std::size_t>(unitsPerCycle / increment);
        double worst = 0.0;
        for (std::size_t i = 0; i < length; ++i)
        {
            const std::uint32_t phase = osc.nextPhase();
            const float y = osc.next();
            const double t = (static_cast<double>(phase) - (delay * increment)) / unitsPerCycle;
            const double cycle = t - std::floor(t);
            if (i < 64 || distanceToEdge(shape, cycle, increment / unitsPerCycle) < 64.0)
            {
                continue;
            }
            worst = std::max(worst, std::abs(static_cast<double>(y) - idealWave(shape, cycle)));
        }
        out << "| " << shapeName(shape) << " | " << scientific(worst) << " | " << db(worst) << " |\n";
    }
}

// --- Peaks under the stress run ----------------------------------------------

void reportStressPeaks(std::ostream& out)
{
    out << "\n## Peaks under the stress run\n\n"
           "*One voice*: a bare oscillator on the stress path at each of the three extreme detune offsets "
           "(-3.32, 0, and +3.32 semitones), retriggered to phase 0 at each note-on event at its sub-sample time; "
           "and again with a jump to a random phase every 1 to 16 samples. *Slot*: the range test's stress run "
           "itself (up to 8 voices, full width). Largest |sample| over the four rates.\n\n"
           "| Waveform | One voice, note-ons | One voice, random jumps | Slot |\n|---|---:|---:|---:|\n";
    double worstVoice = 0.0;
    for (const auto shape : allWaveforms)
    {
        double retriggered = 0.0;
        double jumped = 0.0;
        double slotPeak = 0.0;
        for (const double sampleRate : rates)
        {
            const auto notes = stressNotes(sampleRate);
            const auto events = stressEvents(notes.size(), static_cast<std::uint64_t>(sampleRate));
            if (anoesis::dsp::isPeriodic(shape))
            {
                for (const float detune : {-3.3219f, 0.0f, 3.3219f})
                {
                    auto a = preparedOscillator(shape, sampleRate);
                    auto b = preparedOscillator(shape, sampleRate);
                    anoesis::dsp::Random random{3};
                    std::size_t next = 0;
                    std::uint32_t untilJump = 1;
                    for (std::size_t i = 0; i < notes.size(); ++i)
                    {
                        const float hz = anoesis::dsp::noteToFrequency(notes[i] + detune);
                        a.setFrequency(hz);
                        b.setFrequency(hz);
                        for (; next < events.size() && events[next].at == i; ++next)
                        {
                            if (events[next].kind == Event::Kind::noteOn)
                            {
                                a.jumpTo(0, events[next].since);
                            }
                        }
                        if (--untilJump == 0)
                        {
                            b.jumpTo(random.nextUint32(), random.nextUnipolar());
                            untilJump = 1 + (random.nextUint32() % 16U);
                        }
                        retriggered = std::max(retriggered, static_cast<double>(std::abs(a.next())));
                        jumped = std::max(jumped, static_cast<double>(std::abs(b.next())));
                    }
                }
            }
            slotPeak =
                std::max(slotPeak, static_cast<double>(renderStress(shape, sampleRate, 4096, notes, events).peak));
        }
        worstVoice = std::max({worstVoice, retriggered, jumped});
        out << "| " << shapeName(shape) << " | " << fixed(retriggered, 2) << " | " << fixed(jumped, 2) << " | "
            << fixed(slotPeak, 2) << " |\n";
    }
    out << "\nWorst single voice: " << fixed(worstVoice, 3) << ".\n";
}

// --- Long runs ----------------------------------------------------------------

// Per cycle at 0.01 Hz: DC, RMS about it, and length, for every whole cycle
// of 2^31 samples (12.4 hours at 48 kHz).
void reportLongBottom(std::ostream& out)
{
    out << "\n## Hours at 0.01 Hz\n\n"
           "Bare oscillator at 0.01 Hz, 48 kHz (increment 894: the phase sequence repeats only after 2^31 "
           "samples), run for 2^31 samples, 12.4 hours. Each whole cycle (from one wrap of the integer phase to "
           "the next) is measured for its DC, its RMS about that, and its length. First and last cycle, and the "
           "largest difference between any cycle and the first.\n\n"
           "| Waveform | Cycles | DC, first / last | RMS, first / last | Largest DC change | Largest RMS change | "
           "Lengths (samples) |\n|---|---:|---:|---:|---:|---:|---:|\n";
    for (const auto shape : {Waveform::saw, Waveform::sine, Waveform::hpulse})
    {
        auto osc = preparedOscillator(shape);
        osc.setFrequency(0.01f);
        constexpr std::uint64_t total = std::uint64_t{1} << 31U;
        double sum = 0.0;
        double sumSquares = 0.0;
        std::uint64_t count = 0;
        bool counting = false;
        std::vector<double> dcs;
        std::vector<double> rmss;
        std::uint64_t shortest = total;
        std::uint64_t longest = 0;
        std::uint32_t previous = osc.nextPhase();
        for (std::uint64_t i = 0; i < total; ++i)
        {
            const float y = osc.next();
            const std::uint32_t phase = osc.nextPhase();
            if (counting)
            {
                sum += y;
                sumSquares += static_cast<double>(y) * y;
                ++count;
            }
            if (phase < previous) // wrapped
            {
                if (counting)
                {
                    const double dc = sum / static_cast<double>(count);
                    dcs.push_back(dc);
                    rmss.push_back(std::sqrt((sumSquares / static_cast<double>(count)) - (dc * dc)));
                    shortest = std::min(shortest, count);
                    longest = std::max(longest, count);
                }
                counting = true;
                sum = 0.0;
                sumSquares = 0.0;
                count = 0;
            }
            previous = phase;
        }
        double dcChange = 0.0;
        double rmsChange = 0.0;
        for (std::size_t c = 0; c < dcs.size(); ++c)
        {
            dcChange = std::max(dcChange, std::abs(dcs[c] - dcs.front()));
            rmsChange = std::max(rmsChange, std::abs(rmss[c] - rmss.front()));
        }
        out << "| " << shapeName(shape) << " | " << dcs.size() << " | " << scientific(dcs.front(), 2) << " / "
            << scientific(dcs.back(), 2) << " | " << fixed(rmss.front(), 7) << " / " << fixed(rmss.back(), 7) << " | "
            << scientific(dcChange) << " | " << scientific(rmsChange) << " | " << shortest << "-" << longest << " |\n";
    }
}

// One hour near Nyquist: DC, level, and pitch over the first and the last
// second.
void reportLongTop(std::ostream& out)
{
    out << "\n## An hour near Nyquist\n\n"
           "Bare oscillator at 20 kHz, 48 kHz (0.417 fs, below the fade band), one hour (172.8 million samples): "
           "DC, RMS about it, and the pitch measured from zero crossings (`measureFrequency`, sine only), over the "
           "second second (the first holds the onset) and the last.\n\n"
           "| Waveform | DC, second 2 / last | RMS, second 2 / last | Pitch, second 2 / last (Hz) | Increment's pitch "
           "|\n|---|---:|---:|---:|---:|\n";
    for (const auto shape : {Waveform::saw, Waveform::sine})
    {
        auto osc = preparedOscillator(shape);
        osc.setFrequency(20000.0f);
        constexpr std::size_t second = 48000;
        constexpr std::size_t hour = 3600 * second;
        std::vector<float> first(second);
        std::vector<float> last(second);
        for (std::size_t i = 0; i < second; ++i)
        {
            static_cast<void>(osc.next()); // past the onset
        }
        std::ranges::generate(first, [&] { return osc.next(); });
        for (std::size_t i = 2 * second; i < hour - second; ++i)
        {
            static_cast<void>(osc.next());
        }
        std::ranges::generate(last, [&] { return osc.next(); });
        const auto pitch = [&](std::span<const float> y)
        {
            return shape == Waveform::sine ? fixed(measureFrequency(y, rate), 6) : std::string{"-"};
        };
        out << "| " << shapeName(shape) << " | " << scientific(meanOf(first), 2) << " / " << scientific(meanOf(last), 2)
            << " | " << fixed(rmsAbout(first, meanOf(first)), 7) << " / " << fixed(rmsAbout(last, meanOf(last)), 7)
            << " | " << pitch(first) << " / " << pitch(last) << " | "
            << fixed(static_cast<double>(osc.phaseIncrement()) * rate / unitsPerCycle, 6) << " |\n";
    }
}

} // namespace

TEST_CASE("Range report: the oscillator from 0.01 Hz to beyond Nyquist", "[.][range-report]")
{
    std::ostringstream out;
    reportAboveNyquist(out);
    reportLevelNearNyquist(out);
    reportPaths(out);
    reportSaturator(out);
    reportBottom(out);
    reportFloatResolution(out);
    reportStressPeaks(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}

TEST_CASE("Range report: hours at 0.01 Hz and an hour near Nyquist", "[.][range-long-report]")
{
    std::ostringstream out;
    reportLongBottom(out);
    reportLongTop(out);
    std::cout << out.str() << std::flush;
    SUCCEED();
}
