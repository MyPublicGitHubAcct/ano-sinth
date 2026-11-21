// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Candidate voice structures for M1.4 task 1 (inventory §1, §3 input select,
// §4, §5, §7; docs/dsp/voice-architecture.md), measured by the hidden
// [voice-report] and the [voice] benchmarks on the real DSP classes. Nothing
// here is library code: it is stage 1's prototype, for the project owner to
// choose from before a voice is built in libs/dsp.
//
// One voice: two parts (A, B), each with two OscillatorSlots, a LadderFilter
// and an Adsr (the amp envelope); one PitchDrift shared by the four slots
// (decided 2026-11-14). Each filter's Input Select takes its own part, both
// parts (AB), or nothing (--). Each filter is fed through the Filter Control
// at its own 32-sample grid points counted from the note-on, by the decided
// feed in filter_control_support.hpp (feedFilter), the global knob ramps read
// ahead with valueAfter (KnobRamps).
//
// What the candidates vary:
//
// - Readings (what Sylenth1 does, question 1 and 2 of the task):
//   - CrossEnvelope: where the amp envelopes act on a part's oscillators that
//     feed the other part's filter. The developer (KVR, 2015): "Routing part
//     A and B both through filter A, will make osc B route through amp env B,
//     through filter A through amp env A, so both envelopes".
//   - Unselected: whether a part's oscillators that no filter selects are
//     silent or heard unfiltered.
//   - BypassInput: whether a bypassed filter still takes its Input Select.
//   - AbLaw: how AB sums the two parts.
// - Engine (how we build it; every engine gives the same output bit for bit):
//   - Layout: per-part buffers, each filter in place on its own part's buffer
//     where no other filter needs that part, or per-filter buffers always.
//   - Schedule: every stage in chunks that stop at the voice's grid (32
//     samples of fixed scratch), or each stage over the whole piece of the
//     host block (scratch of maxBlockSize, sized in prepare()).
//   - A mono path when every oscillator is centred (decided at the note-on):
//     the filters run one channel and the output copies it to both.
//   - Skipping parts nothing hears (task 5's saving, measured here only).

#pragma once

#include "../filters/filter_control_support.hpp"

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace anoesis::test::voice
{

namespace dsp = anoesis::dsp;
using filter_control::KnobRamps;

inline constexpr std::size_t partCount = 2;
inline constexpr std::size_t slotsPerPart = 2;
inline constexpr std::size_t stereo = 2;

/// A filter's Input Select. The evidence (inventory §3, V1): each filter
/// offers its own part, AB, or nothing; filter B has no "A only".
enum class Input : std::uint8_t
{
    none, ///< "--": the filter has no input
    own,  ///< A for filter A, B for filter B
    both, ///< AB
};

/// How AB sums the two parts (question 2).
enum class AbLaw : std::uint8_t
{
    plainSum,   ///< 1 + 1: as the oscillators within a part ("added together", manual p. 8)
    half,       ///< 0.5 each: AB of two identical parts is as loud as one
    equalPower, ///< 1/sqrt(2) each: AB of two uncorrelated parts has one part's power
};

[[nodiscard]] constexpr float abGain(AbLaw law) noexcept
{
    switch (law)
    {
    case AbLaw::half:
        return 0.5f;
    case AbLaw::equalPower:
        return 0.70710677f;
    case AbLaw::plainSum:
        break;
    }
    return 1.0f;
}

/// Where the amp envelopes act on a part's oscillators that cross into the
/// other part's filter (question 1).
enum class CrossEnvelope : std::uint8_t
{
    filterPartOnly,       ///< I1: only the filter's part's envelope, after the filter
    sourceThenFilterPart, ///< I2 (the developer, 2015): the source part's envelope before the filter, the
                          ///< filter's part's after
};

/// A part's oscillators that no filter selects (question 1).
enum class Unselected : std::uint8_t
{
    silent,     ///< not heard (a user, 2015: "unless I have Part B routed through it I get no sound from Part B")
    unfiltered, ///< heard dry, through their own part's envelope
};

/// What a bypassed filter passes (question 1).
enum class BypassInput : std::uint8_t
{
    obeysSelect, ///< its Input Select, unfiltered (the same user, 2015, with Filter B on Bypass)
    ownPart,     ///< its own part, whatever the Input Select says
};

struct Readings
{
    CrossEnvelope cross = CrossEnvelope::sourceThenFilterPart;
    Unselected unselected = Unselected::silent;
    BypassInput bypass = BypassInput::obeysSelect;
    AbLaw ab = AbLaw::plainSum;
};

enum class Layout : std::uint8_t
{
    perPart,   ///< filters in place on their part's buffer where nothing else needs it
    perFilter, ///< each filter's input always built in its own buffer
};

enum class Schedule : std::uint8_t
{
    grid32, ///< every stage in chunks that stop at the voice's 32-sample grid: fixed scratch of 32 samples
    block,  ///< each stage over the whole piece: scratch of maxBlockSize, sized in prepare()
};

struct Engine
{
    Layout layout = Layout::perPart;
    Schedule schedule = Schedule::grid32;
    bool monoWhenCentred = false;
    bool skipUnheard = false; ///< task 5: don't render a part nothing hears
};

struct FilterSettings
{
    Input input = Input::own;
    bool bypassed = false;
    dsp::FilterResponse response = dsp::FilterResponse::lp24;
    float drive = 1.0f; ///< the linear gain (F8: 1 to 16)
};

struct Patch
{
    std::array<FilterSettings, partCount> filters{};
    bool warm = true;
};

/// The global knob ramps each filter reads (filter A's Cutoff and Resonance
/// with the Filter Control's, and filter B's), positioned at the start of
/// the piece being rendered, and the Keytrack knob.
struct FilterFeed
{
    const KnobRamps* a = nullptr;
    const KnobRamps* b = nullptr;
    float keytrack = 0.0f; ///< the Keytrack knob, 0-10
};

/// One filter's routing, from the patch and the readings.
struct Route
{
    float own = 0.0f;   ///< the gain on its own part
    float other = 0.0f; ///< the gain on the other part (AB)
    bool active = false;
    bool inPlace = false; ///< filtered in place on its own part's buffer
};

template <std::size_t Taps = dsp::minBlepTaps>
class PrototypeVoice
{
public:
    using Slot = dsp::OscillatorSlot<Taps>;
    static constexpr std::size_t grid = dsp::LadderFilter::gridSamples;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;

    PrototypeVoice(const dsp::MinBlepTable& table, const Readings& readings, const Engine& engine) noexcept
        : slots{Slot{table}, Slot{table}, Slot{table}, Slot{table}}, readings(readings), engine(engine)
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(0x9E3779B97F4A7C15U * (s + 1));
        }
        drift.setSeed(0x3C6EF372FE94F82BU);
        setPatch({});
    }

    PrototypeVoice(const PrototypeVoice&) = delete;
    PrototypeVoice& operator=(const PrototypeVoice&) = delete;
    PrototypeVoice(PrototypeVoice&&) = delete;
    PrototypeVoice& operator=(PrototypeVoice&&) = delete;
    ~PrototypeVoice() = default;

    /// Slots 0-1 are A1, A2; 2-3 are B1, B2.
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] dsp::Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] dsp::PitchDrift& pitchDrift() noexcept
    {
        return drift;
    }
    [[nodiscard]] const std::array<Route, partCount>& routes() const noexcept
    {
        return route;
    }

    void setPatch(const Patch& p) noexcept
    {
        patch = p;
        for (std::size_t f = 0; f < partCount; ++f)
        {
            filters[f].setWarm(p.warm);
            filters[f].setResponse(p.filters[f].response);
            filters[f].setDrive(p.filters[f].drive);
            filters[f].setBypassed(p.filters[f].bypassed);
        }
        updateRoutes();
    }

    /// Allocates the scratch (the only allocation): 32 samples per buffer
    /// on the grid schedule, maxBlockSize on the block schedule.
    void prepare(const dsp::ProcessSpec& spec)
    {
        const dsp::ProcessSpec stereoSpec{
            .sampleRate = spec.sampleRate,
            .maxBlockSize = spec.maxBlockSize,
            .numChannels = stereo,
        };
        for (auto& s : slots)
        {
            s.prepare(stereoSpec);
        }
        for (auto& f : filters)
        {
            f.prepare(stereoSpec);
        }
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
        drift.prepare(spec);
        capacity = engine.schedule == Schedule::grid32 ? grid : spec.maxBlockSize;
        // Parts and filters: 2 x 2 channels each; the two envelopes.
        storage.assign(capacity * ((2 * partCount * stereo) + partCount), 0.0f);
        for (std::size_t p = 0; p < partCount; ++p)
        {
            for (std::size_t c = 0; c < stereo; ++c)
            {
                partPointers[p][c] = storage.data() + (capacity * ((p * stereo) + c));
                filterPointers[p][c] = storage.data() + (capacity * (((partCount + p) * stereo) + c));
            }
            envelopePointers[p] = storage.data() + (capacity * ((2 * partCount * stereo) + p));
        }
    }

    /// The scratch's size in bytes.
    [[nodiscard]] std::size_t scratchBytes() const noexcept
    {
        return storage.size() * sizeof(float);
    }

    /// A note-on at the sample `at` (absolute, for the knob ramps): the
    /// slots, the drift, both envelopes, and the filters fed for this
    /// sample, then reset() so their grid starts here.
    void noteOn(float noteNumber, const FilterFeed& feed, std::size_t at) noexcept
    {
        note = noteNumber;
        for (auto& s : slots)
        {
            s.noteOn();
        }
        drift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        mono = engine.monoWhenCentred && allCentred();
        feedFilters(feed, at);
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
    }

    void noteOff() noexcept
    {
        for (auto& e : envelopes)
        {
            e.gateOff();
        }
    }

    [[nodiscard]] bool isActive() const noexcept
    {
        return envelopes[0].isActive() || envelopes[1].isActive();
    }

    [[nodiscard]] bool isMono() const noexcept
    {
        return mono;
    }

    /// Adds the voice into channels 0 and 1 of `out`, whose first sample is
    /// the absolute sample `at`. The caller splits its block at note events.
    void render(const dsp::AudioBlock& out, const FilterFeed& feed, std::size_t at) noexcept
    {
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n)
        {
            const auto length = engine.schedule == Schedule::grid32 ? std::min(n - done, grid - gridPosition)
                                                                    : std::min(n - done, capacity);
            renderChunk(out.subBlock(done, length), feed, at + done);
            done += length;
        }
    }

private:
    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            auto input = patch.filters[f].input;
            if (patch.filters[f].bypassed && readings.bypass == BypassInput::ownPart)
            {
                input = Input::own;
            }
            switch (input)
            {
            case Input::none:
                route[f] = {};
                break;
            case Input::own:
                route[f] = {
                    .own = 1.0f,
                    .other = 0.0f,
                    .active = true,
                    .inPlace = false,
                };
                break;
            case Input::both:
            {
                const float g = abGain(readings.ab);
                route[f] = {
                    .own = g,
                    .other = g,
                    .active = true,
                    .inPlace = false,
                };
                break;
            }
            }
        }
        for (std::size_t f = 0; f < partCount; ++f)
        {
            const auto& otherFilter = route[1 - f];
            const bool otherReadsMine = otherFilter.active && otherFilter.other > 0.0f;
            route[f].inPlace = engine.layout == Layout::perPart && route[f].active && !otherReadsMine;
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            const bool read =
                (route[p].active && route[p].own > 0.0f) || (route[1 - p].active && route[1 - p].other > 0.0f);
            unselected[p] = !read;
            heard[p] = read || readings.unselected == Unselected::unfiltered;
        }
    }

    /// Every sounding unison voice of every slot at the same gain left and
    /// right, to the bit.
    [[nodiscard]] bool allCentred() const noexcept
    {
        for (const auto& s : slots)
        {
            for (std::size_t v = 0; v < s.voices(); ++v)
            {
                const auto g = s.voiceGains(v);
                if (std::bit_cast<std::uint32_t>(g.left) != std::bit_cast<std::uint32_t>(g.right))
                {
                    return false;
                }
            }
        }
        return true;
    }

    /// Feeds each filter with input for the sample `sample`: the decided
    /// feed, keytrack computed once for both.
    void feedFilters(const FilterFeed& feed, std::size_t sample) noexcept
    {
        const float keytrack = dsp::keytrackOctaves(note, feed.keytrack / 10.0f);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!route[f].active)
            {
                continue;
            }
            const auto& r = f == 0 ? *feed.a : *feed.b;
            const auto from = r.where();
            filter_control::feedFilter(filters[f],
                                       {
                                           .cutoff = KnobRamps::at(r.cutoff, from, sample),
                                           .controlCutoff = KnobRamps::at(r.controlCutoff, from, sample),
                                           .keytrack = keytrack,
                                       },
                                       {
                                           .resonance = KnobRamps::at(r.resonance, from, sample),
                                           .controlResonance = KnobRamps::at(r.controlResonance, from, sample),
                                       });
        }
    }

    [[nodiscard]] dsp::AudioBlock partBlock(std::size_t p, std::size_t channels, std::size_t length) const noexcept
    {
        return {partPointers[p].data(), channels, length};
    }

    [[nodiscard]] dsp::AudioBlock filterBlock(std::size_t f, std::size_t channels, std::size_t length) const noexcept
    {
        return {route[f].inPlace ? partPointers[f].data() : filterPointers[f].data(), channels, length};
    }

    /// One filter's input: own x gOwn + other x gOther, the other part
    /// through its own envelope first under I2.
    void buildInput(std::size_t f, std::size_t channels, std::size_t length) noexcept
    {
        const auto& r = route[f];
        const bool crossEnvelope = readings.cross == CrossEnvelope::sourceThenFilterPart;
        const float* envelopeOther = envelopePointers[1 - f];
        for (std::size_t c = 0; c < channels; ++c)
        {
            const float* own = partPointers[f][c];
            const float* other = partPointers[1 - f][c];
            float* dst = r.inPlace ? partPointers[f][c] : filterPointers[f][c];
            if (r.other > 0.0f)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    const float crossing = crossEnvelope ? envelopeOther[i] * other[i] : other[i];
                    dst[i] = (r.own * own[i]) + (r.other * crossing);
                }
            }
            else if (!r.inPlace)
            {
                std::copy_n(own, length, dst); // gOwn is 1 here
            }
        }
    }

    void renderChunk(const dsp::AudioBlock& out, const FilterFeed& feed, std::size_t at) noexcept
    {
        const auto length = out.numSamples();
        const std::size_t channels = mono ? 1 : stereo;

        // 1. The oscillators, added into their part's (stereo) buffer, at the
        // voice's note plus drift, in runs of constant drift.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            if (heard[p] || !engine.skipUnheard)
            {
                partBlock(p, stereo, length).clear();
            }
        }
        drift.process(length,
                      [&](std::size_t start, std::size_t run, float offset) noexcept
                      {
                          for (std::size_t p = 0; p < partCount; ++p)
                          {
                              if (!heard[p] && engine.skipUnheard)
                              {
                                  continue;
                              }
                              const auto block = partBlock(p, stereo, length).subBlock(start, run);
                              for (std::size_t s = 0; s < slotsPerPart; ++s)
                              {
                                  auto& slot = slots[(p * slotsPerPart) + s];
                                  if (slot.soundingVoices() > 0)
                                  {
                                      slot.process(block, note + offset);
                                  }
                              }
                          }
                      });

        // 2. The amp envelopes' levels.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopePointers[p], length});
        }

        // 3. Each filter's input, by its Input Select (the part filtered in
        // place builds last, so no other input reads it modified).
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (route[f].active && !route[f].inPlace)
            {
                buildInput(f, channels, length);
            }
        }
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (route[f].active && route[f].inPlace)
            {
                buildInput(f, channels, length);
            }
        }

        // 4. The filters in place, split at the voice's grid, each grid point
        // fed for the end of the coming segment.
        std::size_t done = 0;
        while (done < length)
        {
            if (gridPosition == 0)
            {
                feedFilters(feed, at + done + grid);
            }
            const auto segment = std::min(length - done, grid - gridPosition);
            for (std::size_t f = 0; f < partCount; ++f)
            {
                if (route[f].active)
                {
                    filters[f].process(filterBlock(f, channels, length).subBlock(done, segment));
                }
            }
            done += segment;
            gridPosition = (gridPosition + segment) % grid;
        }

        // 5. Each filter's output through its part's envelope (and, under
        // Unselected::unfiltered, an unselected part dry through its own),
        // added to the voice's output.
        const auto left = out.channel(0);
        const auto right = out.channel(1);
        const auto add = [&](const dsp::AudioBlock& y, const float* e) noexcept
        {
            const auto y0 = y.channel(0);
            const auto y1 = y.channel(channels - 1);
            for (std::size_t i = 0; i < length; ++i)
            {
                left[i] += e[i] * y0[i];
                right[i] += e[i] * y1[i];
            }
        };
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (route[f].active)
            {
                add(filterBlock(f, channels, length), envelopePointers[f]);
            }
        }
        if (readings.unselected == Unselected::unfiltered)
        {
            for (std::size_t p = 0; p < partCount; ++p)
            {
                if (unselected[p])
                {
                    add(partBlock(p, channels, length), envelopePointers[p]);
                }
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<dsp::LadderFilter, partCount> filters{};
    std::array<dsp::Adsr, partCount> envelopes{};
    dsp::PitchDrift drift;
    Readings readings;
    Engine engine;
    Patch patch;
    std::array<Route, partCount> route{};
    std::array<bool, partCount> unselected{};
    std::array<bool, partCount> heard{};

    std::vector<float> storage;
    std::array<std::array<float*, stereo>, partCount> partPointers{};
    std::array<std::array<float*, stereo>, partCount> filterPointers{};
    std::array<float*, partCount> envelopePointers{};
    std::size_t capacity = 0;

    float note = 69.0f;
    std::size_t gridPosition = 0;
    bool mono = false;
};

// --- Patches ------------------------------------------------------------------

/// The init patch as far as the evidence goes (V9, F26): A1 a saw with one
/// voice at Volume 10 (unity), the others off; every filter LP24 on its own
/// part, the cutoff knobs at 5, resonance 0, drive 0 (x1); Warm Drive on;
/// both amp envelopes A 0, D 0, S 10, R 0 (a gate).
template <std::size_t Taps>
void setInitPatch(PrototypeVoice<Taps>& v)
{
    for (std::size_t s = 0; s < PrototypeVoice<Taps>::slotCount; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setVoices(s == 0 ? 1 : 0);
    }
    for (std::size_t p = 0; p < partCount; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.0f);
        e.setDecay(0.0f);
        e.setSustain(1.0f);
        e.setRelease(0.0f);
    }
    v.setPatch({});
}

/// The worst case: 4 oscillators x 8 unison saws, detune and stereo spread
/// (Detune 5 and Stereo 10 under the unison knob laws), both filters on AB,
/// LP24 with drive x4, Warm Drive on.
template <std::size_t Taps>
void setWorstPatch(PrototypeVoice<Taps>& v, float stereoWidth = 1.0f)
{
    for (std::size_t s = 0; s < PrototypeVoice<Taps>::slotCount; ++s)
    {
        auto& slot = v.slot(s);
        slot.setWaveform(dsp::Waveform::saw);
        slot.setVoices(8);
        slot.setDetune(0.548f); // 3.3219 x 0.5^2 x g(8) = 0.66
        slot.setStereo(stereoWidth);
        slot.setRetrigger(false);
        slot.setTuning({.octaves = 0, .semitones = s < 2 ? 0 : 7, .fine = 0.0f});
    }
    for (std::size_t p = 0; p < partCount; ++p)
    {
        auto& e = v.envelope(p);
        e.setAttack(0.005f);
        e.setDecay(0.3f);
        e.setSustain(0.7f);
        e.setRelease(0.2f);
    }
    Patch patch;
    for (auto& f : patch.filters)
    {
        f.input = Input::both;
        f.drive = 4.0f;
    }
    v.setPatch(patch);
}

} // namespace anoesis::test::voice
