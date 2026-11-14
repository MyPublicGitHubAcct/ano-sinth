// CPU cost of the minBLEP oscillator (decision D6) and the PolyBLEP reference,
// for every periodic waveform, of the oscillator slot (tuning, volume, pan,
// invert) around it, of retriggers and phase jumps, and of unison (0-8
// voices per slot, noise, and a 32-voice note).
// The wavetable candidate's numbers (it was removed after D6) are in the
// baselines doc.
//
// Tagged [!benchmark]: run with tools/bench.sh "[oscillators]" in
// relwithdebinfo. Each benchmark renders one 512-sample block. Results and the
// decision: docs/performance/baselines.md, docs/dsp/band-limited-oscillators.md

#include "../oscillators/oscillator_support.hpp"

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>

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

namespace
{

using Slot = anoesis::dsp::OscillatorSlot<anoesis::dsp::minBlepTaps>;

Slot preparedSlot(Waveform shape)
{
    Slot slot{minBlepTable64()};
    slot.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
    slot.setWaveform(shape);
    slot.setTuning({.octaves = 1, .semitones = -5, .fine = 0.13f});
    slot.setPan(0.3f);
    slot.setVolume(0.8f);
    slot.reset();
    return slot;
}

// A stereo voice bus.
struct Bus
{
    Block left{};
    Block right{};
    std::array<float*, 2> pointers{left.data(), right.data()};

    [[nodiscard]] anoesis::dsp::AudioBlock block() noexcept
    {
        return {pointers.data(), 2, blockSize};
    }
};

} // namespace

TEST_CASE("Benchmark: oscillator slot (tuning, volume, pan, invert)", "[!benchmark][dsp][oscillators]")
{
    // The slot's tuning is +7.13 semitones (preparedSlot), so note 61.87
    // plays 440 Hz: an ordinary pitch. The bare oscillator runs at exactly
    // the slot's frequency, in the same run, both as it is (mono out) and
    // with the same stereo gain written inline: the fair baseline for the
    // slot, which must produce two channels.
    constexpr float note = 61.87f;
    const float hz = preparedSlot(Waveform::saw).frequencyFor(note);
    const auto gains = anoesis::dsp::panGains(0.3f);
    const float gainLeft = 0.8f * gains.left;
    const float gainRight = 0.8f * gains.right;
    const auto bareOscillator = [](Waveform shape, float frequency)
    {
        auto osc = Candidate<MinBlep64>::make();
        osc.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        osc.setWaveform(shape);
        osc.setFrequency(frequency);
        return osc;
    };
    Block out{};
    Bus bus;
    {
        auto osc = bareOscillator(Waveform::saw, hz);
        BENCHMARK("minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512")
        {
            for (auto& sample : out)
            {
                sample = osc.next();
            }
            return out[blockSize - 1];
        };
    }
    {
        auto osc = bareOscillator(Waveform::saw, hz);
        BENCHMARK("minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                const float x = osc.next();
                bus.left[i] += gainLeft * x;
                bus.right[i] += gainRight * x;
            }
            return bus.left[blockSize - 1];
        };
    }
    {
        auto slot = preparedSlot(Waveform::saw);
        BENCHMARK("Oscillator slot, saw 440 Hz, settled x512")
        {
            slot.process(bus.block(), note);
            return bus.left[blockSize - 1];
        };
    }
    {
        // A new volume every block, so nearly every sample is mid-ramp.
        auto slot = preparedSlot(Waveform::saw);
        float volume = 0.8f;
        BENCHMARK("Oscillator slot, saw 440 Hz, volume ramping x512")
        {
            volume = volume > 0.5f ? 0.4f : 0.8f;
            slot.setVolume(volume);
            slot.process(bus.block(), note);
            return bus.left[blockSize - 1];
        };
    }
    {
        // Vibrato, +-1 semitone: a note per sample, so one noteToFrequency
        // (exp2) per sample. Its baseline converts the same notes inline.
        Block notes{};
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            notes[i] = note + std::sin(static_cast<float>(i) * 0.05f);
        }
        const auto slot0 = preparedSlot(Waveform::saw);
        auto osc = bareOscillator(Waveform::saw, hz);
        BENCHMARK("minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                osc.setFrequency(slot0.frequencyFor(notes[i]));
                const float x = osc.next();
                bus.left[i] += gainLeft * x;
                bus.right[i] += gainRight * x;
            }
            return bus.left[blockSize - 1];
        };
        auto slot = preparedSlot(Waveform::saw);
        BENCHMARK("Oscillator slot, saw 440 Hz, pitch per sample x512")
        {
            slot.process(bus.block(), notes);
            return bus.left[blockSize - 1];
        };
    }
    {
        // Where corners come every few samples. No tuning here, so the note
        // is the pitch.
        const float highNote = anoesis::dsp::frequencyToNote(12000.0f);
        auto osc = bareOscillator(Waveform::trisaw, anoesis::dsp::noteToFrequency(highNote));
        BENCHMARK("minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                const float x = osc.next();
                bus.left[i] += gainLeft * x;
                bus.right[i] += gainRight * x;
            }
            return bus.left[blockSize - 1];
        };
        auto slot = preparedSlot(Waveform::trisaw);
        slot.setTuning({});
        BENCHMARK("Oscillator slot, TriSaw 12 kHz, settled x512")
        {
            slot.process(bus.block(), highNote);
            return bus.left[blockSize - 1];
        };
    }
    {
        // One note's worth, as the 32-saw bank above: 4 oscillators at
        // 110, 220, 440, and 880 Hz (notes 45, 57, 69, 81), each with 8
        // voices fined over +-20 cents, all into one bus.
        std::vector<Slot> bank;
        std::array<float, 32> notes{};
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            bank.push_back(preparedSlot(Waveform::saw));
            const auto voice = static_cast<float>(i % 8);
            bank.back().setTuning({.fine = -0.2f + (0.4f * voice / 7.0f)});
            const std::size_t oscillator = i / 8;
            notes[i] = 45.0f + (12.0f * static_cast<float>(oscillator));
        }
        BENCHMARK("Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512")
        {
            bus.left.fill(0.0f);
            bus.right.fill(0.0f);
            for (std::size_t i = 0; i < bank.size(); ++i)
            {
                bank[i].process(bus.block(), notes[i]);
            }
            return bus.left[blockSize - 1];
        };
    }
}

TEST_CASE("Benchmark: retrigger and phase jumps", "[!benchmark][dsp][oscillators]")
{
    // The band-limited jump (oscillator-phase.md). Each row has a twin
    // without jumps in the benchmark case above (same waveform and pitch),
    // so the cost of one jump is (this - that) x 512 / the jumps per block.
    Block out{};
    const auto withJumps = [&](const char* label, Waveform shape, float hz, std::size_t every)
    {
        auto osc = Candidate<MinBlep64>::make();
        osc.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        osc.setWaveform(shape);
        osc.setFrequency(hz);
        BENCHMARK(std::string{"minBLEP 64-tap, "} + label + " x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                if (i % every == 0)
                {
                    osc.jumpTo(0);
                }
                out[i] = osc.next();
            }
            return out[blockSize - 1];
        };
    };
    withJumps("saw 440 Hz, retrigger every 64 samples", Waveform::saw, 440.0f, 64);
    withJumps("square 440 Hz, retrigger every 64 samples", Waveform::pulse, 440.0f, 64);
    withJumps("triangle 440 Hz, retrigger every 64 samples", Waveform::triangle, 440.0f, 64);
    withJumps("sine 440 Hz, retrigger every 64 samples", Waveform::sine, 440.0f, 64);
    withJumps("TriSaw 440 Hz, retrigger every 64 samples", Waveform::trisaw, 440.0f, 64);

    // Hard sync: a jump every 43.97 samples (a 1.09 kHz master), so the
    // jumps fall between samples, as Part 2's sync will make them.
    {
        auto osc = Candidate<MinBlep64>::make();
        osc.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 1});
        osc.setWaveform(Waveform::saw);
        osc.setFrequency(1000.0f);
        constexpr double period = 43.97; // samples: a 1091.6 Hz master
        double nextSync = 0.0;
        double time = 0.0;
        BENCHMARK("minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master x512")
        {
            for (std::size_t i = 0; i < blockSize; ++i)
            {
                if (nextSync <= time)
                {
                    osc.jumpTo(0, static_cast<float>(time - nextSync));
                    nextSync += period;
                }
                time += 1.0;
                out[i] = osc.next();
            }
            return out[blockSize - 1];
        };
    }

    // The slot: a note-on at the start of every block (Retrig on), against
    // the settled row in the slot benchmark case.
    {
        auto slot = preparedSlot(Waveform::saw);
        Bus bus;
        BENCHMARK("Oscillator slot, saw 440 Hz, retriggered every block x512")
        {
            slot.noteOn();
            slot.process(bus.block(), 61.87f);
            return bus.left[blockSize - 1];
        };
    }
}

TEST_CASE("Benchmark: unison", "[!benchmark][dsp][oscillators]")
{
    // OscillatorSlot with 0-8 unison voices (oscillator-unison.md): Detune
    // 0.2 semitones, Stereo 0.5 (every voice its own gains), Retrig off.
    // Divide a row by its voice count for the cost per voice; compare the
    // 1-voice rows with "Oscillator slot, saw 440 Hz, settled".
    const auto unisonSlot = [](Waveform shape, int voices)
    {
        Slot slot{minBlepTable64()};
        slot.setVoices(voices);
        slot.prepare({.sampleRate = rate, .maxBlockSize = blockSize, .numChannels = 2});
        slot.setWaveform(shape);
        slot.setDetune(0.2f);
        slot.setStereo(0.5f);
        slot.setRetrigger(false);
        slot.noteOn();
        return slot;
    };
    Bus bus;
    const float highNote = anoesis::dsp::frequencyToNote(12000.0f);
    for (const float note : {69.0f, highNote})
    {
        for (const int voices : {1, 2, 4, 8})
        {
            auto slot = unisonSlot(Waveform::saw, voices);
            BENCHMARK("Unison slot, saw " + std::string{note < 100.0f ? "440 Hz" : "12 kHz"} + ", " +
                      std::to_string(voices) + (voices == 1 ? " voice" : " voices") + " x512")
            {
                slot.process(bus.block(), note);
                return bus.left[blockSize - 1];
            };
        }
    }
    {
        std::vector<float> notes(blockSize);
        for (std::size_t i = 0; i < blockSize; ++i)
        {
            notes[i] = 69.0f + std::sin(static_cast<float>(i) * 0.05f);
        }
        auto slot = unisonSlot(Waveform::saw, 8);
        BENCHMARK("Unison slot, saw 440 Hz, 8 voices, pitch per sample x512")
        {
            slot.process(bus.block(), notes);
            return bus.left[blockSize - 1];
        };
    }
    {
        auto slot = unisonSlot(Waveform::saw, 0);
        BENCHMARK("Unison slot, 0 voices x512")
        {
            slot.process(bus.block(), 69.0f);
            return bus.left[blockSize - 1];
        };
    }
    {
        auto slot = unisonSlot(Waveform::noise, 8);
        BENCHMARK("Unison slot, noise, 8 voices x512")
        {
            slot.process(bus.block(), 69.0f);
            return bus.left[blockSize - 1];
        };
    }
    {
        // One note's worth: 4 slots at 110, 220, 440, and 880 Hz, 8 voices
        // each, into one bus. The slots are in one array, allocated once.
        std::array<Slot, 4> note{
            unisonSlot(Waveform::saw, 8),
            unisonSlot(Waveform::saw, 8),
            unisonSlot(Waveform::saw, 8),
            unisonSlot(Waveform::saw, 8),
        };
        BENCHMARK("Unison: 32-voice note (4 slots x 8 voices) into one bus x512")
        {
            bus.left.fill(0.0f);
            bus.right.fill(0.0f);
            for (std::size_t i = 0; i < note.size(); ++i)
            {
                note[i].process(bus.block(), 45.0f + (12.0f * static_cast<float>(i)));
            }
            return bus.left[blockSize - 1];
        };
    }
}
