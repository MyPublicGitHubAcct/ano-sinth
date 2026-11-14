// The oscillator controls report: tuning accuracy over the whole
// Octave/Note/Fine range, the pan law, the volume taper, and the smoothing
// ramp, measured through OscillatorSlot and printed as Markdown. Hidden from
// the default run; run it in an optimised build (a few seconds):
//
//   cmake --build --preset relwithdebinfo --target anoesis_dsp_tests
//   build/relwithdebinfo/tests/dsp/anoesis_dsp_tests "[controls-report]"
//
// Results: docs/dsp/oscillator-controls.md

#include "oscillator_support.hpp"

#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/measure.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
using anoesis::dsp::halfCycle;
using anoesis::dsp::panGains;
using anoesis::dsp::PanLaw;
using anoesis::dsp::ProcessSpec;
using anoesis::dsp::StereoGain;
using anoesis::dsp::Tuning;

namespace
{

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

ProcessSpec specAt(float sampleRate)
{
    return {.sampleRate = sampleRate, .maxBlockSize = 512, .numChannels = 2};
}

double targetHz(float note, const Tuning& tuning, float referenceHz)
{
    const double n =
        static_cast<double>(note) + (12.0 * tuning.octaves) + tuning.semitones + static_cast<double>(tuning.fine);
    return static_cast<double>(referenceHz) * std::exp2((n - 69.0) / 12.0);
}

double incrementHz(std::uint32_t increment, double sampleRate)
{
    return static_cast<double>(increment) * sampleRate / 4294967296.0;
}

double cents(double ratio)
{
    return 1200.0 * std::log2(ratio);
}

double rms(std::span<const float> x)
{
    double sum = 0.0;
    for (const float v : x)
    {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

std::string describe(float note, const Tuning& t)
{
    std::ostringstream s;
    s << "note " << note << ", oct " << t.octaves << ", note " << t.semitones << ", fine " << t.fine;
    return s.str();
}

struct Worst
{
    double cents = 0.0;
    std::string where;
    double hz = 0.0;
    int count = 0;

    void add(double error, double hz0, const std::string& at)
    {
        ++count;
        if (std::abs(error) > std::abs(cents))
        {
            cents = error;
            where = at;
            hz = hz0;
        }
    }
};

// The arithmetic: every base note 0-127, every octave and note offset, fine
// in hundredths, at the slot's increment against the double-precision target.
void reportTuningArithmetic()
{
    std::cout << "\n### Tuning: the increment against the target (every note, octave, note offset, fine in "
                 "0.01)\n\n"
              << "| Rate | Master tune (A4) | Pitches checked | Clamped at Nyquist | Worst error, target >= 20 Hz "
                 "(cents) | Worst error, any target (cents) | Fixed-point floor at 20 Hz / 1 Hz (cents) |\n"
              << "|---|---:|---:|---:|---:|---:|---:|\n";
    for (const float rate : {44100.0f, 48000.0f})
    {
        for (const float reference : {432.0f, 440.0f, 443.5f})
        {
            Slot slot{minBlepTable64()};
            slot.prepare(specAt(rate));
            slot.setWaveform(Waveform::sine);
            slot.setReferenceHz(reference);
            Worst audible;
            Worst any;
            int clamped = 0;
            for (int note = 0; note <= 127; ++note)
            {
                for (int octaves = -3; octaves <= 3; ++octaves)
                {
                    for (int semitones = -7; semitones <= 7; ++semitones)
                    {
                        for (int hundredth = -100; hundredth <= 100; ++hundredth)
                        {
                            const float fine = static_cast<float>(hundredth) / 100.0f;
                            const Tuning tuning{.octaves = octaves, .semitones = semitones, .fine = fine};
                            slot.setTuning(tuning);
                            const auto n = static_cast<float>(note);
                            // Only the increment is needed: computed the way
                            // process() sets it, without rendering.
                            const auto target = targetHz(n, tuning, reference);
                            const auto increment = anoesis::dsp::incrementFor(slot.frequencyFor(n), rate);
                            if (target >= static_cast<double>(rate) / 2.0)
                            {
                                clamped += increment == halfCycle ? 1 : 0;
                                continue;
                            }
                            const auto error = cents(incrementHz(increment, rate) / target);
                            any.add(error, target, describe(n, tuning));
                            if (target >= 20.0)
                            {
                                audible.add(error, target, describe(n, tuning));
                            }
                        }
                    }
                }
            }
            const auto floorAt = [rate](double hz)
            {
                return cents(1.0 + (static_cast<double>(rate) / 4294967296.0 / hz));
            };
            std::cout << "| " << rate << " | " << reference << " | " << any.count << " | " << clamped << " | "
                      << std::setprecision(2) << std::scientific << audible.cents << " (" << audible.hz << " Hz) | "
                      << any.cents << " (" << any.hz << " Hz) | " << floorAt(20.0) << " / " << floorAt(1.0)
                      << std::defaultfloat << std::setprecision(6) << " |\n";
        }
    }
}

// The signal: a sine rendered through the slot, its frequency measured from
// its zero crossings, against the target and against the increment.
void reportTuningMeasured()
{
    std::cout << "\n### Tuning: measured frequency of the rendered sine (fine in quarters; targets below 5 kHz)\n\n"
              << "| Rate | Renders | Worst measured vs target (cents) | at | Worst measured vs increment "
                 "(relative) | at |\n"
              << "|---|---:|---:|---|---:|---|\n";
    constexpr float reference = 443.5f;
    for (const float rate : {44100.0f, 48000.0f})
    {
        Worst worst;
        double worstAgainstIncrement = 0.0;
        double worstAgainstIncrementHz = 0.0;
        std::size_t worstAgainstIncrementLength = 0;
        for (const float note : {21.0f, 60.0f, 96.0f})
        {
            for (int octaves = -3; octaves <= 3; ++octaves)
            {
                for (int semitones = -7; semitones <= 7; ++semitones)
                {
                    for (int quarter = -4; quarter <= 4; ++quarter)
                    {
                        const float fine = static_cast<float>(quarter) / 4.0f;
                        const Tuning tuning{.octaves = octaves, .semitones = semitones, .fine = fine};
                        const auto target = targetHz(note, tuning, reference);
                        if (target >= 5000.0)
                        {
                            continue;
                        }
                        Slot slot{minBlepTable64()};
                        slot.prepare(specAt(rate));
                        slot.setWaveform(Waveform::sine);
                        slot.setReferenceHz(reference);
                        slot.setTuning(tuning);
                        slot.setPan(-1.0f);
                        slot.reset();
                        // At least half a second, and at least 4 cycles.
                        const auto length = static_cast<std::size_t>(
                            std::max(0.5 * static_cast<double>(rate), 4.5 * static_cast<double>(rate) / target));
                        anoesis::test::Buffer out{2, length};
                        slot.process(out.block(), note);
                        const auto measured = measureFrequency(out.channel(0), rate);
                        worst.add(cents(measured / target), target, describe(note, tuning));
                        const auto played = incrementHz(slot.phaseIncrement(), rate);
                        if (std::abs((measured / played) - 1.0) > worstAgainstIncrement)
                        {
                            worstAgainstIncrement = std::abs((measured / played) - 1.0);
                            worstAgainstIncrementHz = target;
                            worstAgainstIncrementLength = length;
                        }
                    }
                }
            }
        }
        std::cout << "| " << rate << " | " << worst.count << " | " << std::setprecision(2) << std::scientific
                  << worst.cents << " | " << worst.where << " (" << std::defaultfloat << worst.hz << " Hz) | "
                  << std::scientific << worstAgainstIncrement << std::defaultfloat << std::setprecision(6) << " | "
                  << worstAgainstIncrementHz << " Hz, " << worstAgainstIncrementLength << " samples |\n";
    }
}

// Left and right levels of a sine through the slot, against the law.
void reportPan()
{
    constexpr float rate = 48000.0f;
    std::cout << "\n### Pan: measured channel levels of a sine (equal-power law)\n\n"
              << "| Knob (0-10) | Position | Left (measured) | Left (law) | Right (measured) | Right (law) | "
                 "L^2 + R^2 (dB) | L + R, mono sum (dB) |\n"
              << "|---:|---:|---:|---:|---:|---:|---:|---:|\n";
    const double unityRms = 1.0 / std::numbers::sqrt2;
    for (int step = 0; step <= 8; ++step)
    {
        const float knob = 1.25f * static_cast<float>(step);
        const float position = (knob / 5.0f) - 1.0f;
        Slot slot{minBlepTable64()};
        slot.prepare(specAt(rate));
        slot.setWaveform(Waveform::sine);
        slot.setReferenceHz(375.0f); // A4 = 375 Hz: 128 samples per cycle, so whole cycles below
        slot.setPan(position);
        slot.reset();
        anoesis::test::Buffer out{2, 4096 + 128};
        slot.process(out.block(), 69.0f);
        const auto left = rms(out.channel(0).subspan(128, 4096)) / unityRms;
        const auto right = rms(out.channel(1).subspan(128, 4096)) / unityRms;
        const auto law = panGains(position, PanLaw::equalPower);
        std::cout << std::fixed << std::setprecision(2) << "| " << knob << " | " << position << std::setprecision(5)
                  << " | " << left << " | " << law.left << " | " << right << " | " << law.right << " | "
                  << std::setprecision(3) << toDb(std::sqrt((left * left) + (right * right))) << " | "
                  << toDb(left + right) << " |\n"
                  << std::defaultfloat << std::setprecision(6);
    }
}

// Gain against the volume taper, and the smoothing ramp.
void reportVolume()
{
    constexpr float rate = 48000.0f;
    std::cout << "\n### Volume: measured gain against the linear taper (knob / 10)\n\n"
              << "| Knob (0-10) | Gain (taper) | Measured gain | Measured (dB) | Taper (dB) |\n"
              << "|---:|---:|---:|---:|---:|\n";
    double unity = 0.0;
    for (const float knob : {10.0f, 7.5f, 5.0f, 2.5f, 1.0f, 0.0f})
    {
        Slot slot{minBlepTable64()};
        slot.prepare(specAt(rate));
        slot.setWaveform(Waveform::saw);
        slot.setReferenceHz(375.0f);
        slot.setPan(-1.0f);
        slot.setVolume(knob / 10.0f);
        slot.reset();
        anoesis::test::Buffer out{2, 4096 + 128};
        slot.process(out.block(), 69.0f);
        const auto level = rms(out.channel(0).subspan(128, 4096));
        if (knob >= 10.0f)
        {
            unity = level;
        }
        const auto gain = level / unity;
        std::cout << std::fixed << std::setprecision(2) << "| " << knob << " | " << knob / 10.0f << std::setprecision(7)
                  << " | " << gain << std::setprecision(2) << " | " << toDb(gain) << " | "
                  << toDb(static_cast<double>(knob) / 10.0) << " |\n"
                  << std::defaultfloat << std::setprecision(6);
    }

    std::cout << "\n### Smoothing: ramp length of a volume, pan, or invert change\n\n"
              << "| Rate | Change | Samples until the gain lands | ms |\n|---:|---|---:|---:|\n";
    for (const float sampleRate : {44100.0f, 48000.0f, 96000.0f})
    {
        for (const char* change : {"volume 1 -> 0.5", "pan hard left -> hard right", "invert off -> on"})
        {
            StereoGain gain;
            gain.prepare(specAt(sampleRate));
            gain.setPan(-1.0f);
            gain.reset();
            const std::string what = change;
            if (what.starts_with("volume"))
            {
                gain.setGain(0.5f);
            }
            else if (what.starts_with("pan"))
            {
                gain.setPan(1.0f);
            }
            else
            {
                gain.setInverted(true);
            }
            std::size_t samples = 0;
            const std::vector<float> one{1.0f};
            while (gain.isSmoothing())
            {
                anoesis::test::Buffer sink{2, 1};
                gain.process(one, sink.block());
                ++samples;
            }
            std::cout << "| " << sampleRate << " | " << change << " | " << samples << " | "
                      << 1000.0 * static_cast<double>(samples) / static_cast<double>(sampleRate) << " |\n";
        }
    }
}

} // namespace

TEST_CASE("Oscillator controls report: tuning, pan, volume, smoothing", "[.][controls-report]")
{
    reportTuningArithmetic();
    reportTuningMeasured();
    reportPan();
    reportVolume();
    SUCCEED();
}
