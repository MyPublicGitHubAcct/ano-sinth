// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/filters/keytrack.hpp>
#include <anoesis/dsp/filters/ladder_filter.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

/// The voice: Sylenth1's per-note audio path up to the mixer (inventory §1,
/// "Voice structure and routing"). Two parts, A and B, each with two
/// oscillators (OscillatorSlot: unison, stereo), a filter (LadderFilter) and
/// an amp envelope (Adsr); one PitchDrift shared by the four slots. Chosen by
/// the project owner from measured candidates (docs/dsp/voice-architecture.md):
///
/// - Each filter's Input Select is its own part, both parts (AB), or "--".
///   AB is a plain sum, as the oscillators within a part are.
/// - A part crossing into the other part's filter goes through its own amp
///   envelope before that filter, and through the filter's part's envelope
///   after it (the developer: "osc B route through amp env B, through filter
///   A through amp env A"). A filter's own part goes through the filter, then
///   its envelope. So, for filter A on AB:
///       out += env A . F_A(g (p_A + env B . p_B)) / g
///   where g is the filter input level (1 by default; inventory V8).
/// - "--" passes nothing and the filter isn't run. A part that no filter
///   reads is silent and its oscillators aren't rendered. A bypassed filter
///   still takes its Input Select, and passes that sum unfiltered.
/// - Each filter is fed through the Filter Control (filter_control.hpp) at
///   the voice's own 32-sample grid, counted from its note-on: at each grid
///   point, the global knob ramps read for the end of the coming segment,
///   plus keytrack from the voice's note, into setCutoffModulation() (with
///   setCutoff() held at the law's anchor) and setResonanceModulation()
///   (with setResonance(0)). A non-finite result keeps the last finite one.
///   The voice's modulation of Cutoff X / AB and Reso X / AB joins the sum
///   in M1.5; until then those terms are 0.
/// - Input Select and Bypass changes apply at the voice's next grid point,
///   so a filter is always on the voice's grid; a filter switched on from
///   "--", or out of Bypass, starts from rest there.
///
/// Rendering runs in chunks that stop at the grid (at most 32 samples): the
/// four slots added into their part's buffer (in drift's runs of constant
/// offset), both envelopes, each filter's input built in its own buffer,
/// the filters in place, and each filter's output times its part's envelope
/// added into channels 0 and 1 of the caller's block. The scratch is fixed
/// (10 buffers of 32 samples), so nothing is allocated, prepare() included;
/// the output does not depend on how the caller splits its blocks.
///
/// Mix A/B, velocity and the voice's output bus are the mixer's (task 4)
/// and the modulation system's (M1.5); the note's glide, bend and master
/// tune are task 3's. About 25 KB at 64 taps.
/// Doc: docs/dsp/voice-architecture.md
namespace anoesis::dsp
{

/// A filter's Input Select (inventory V1): its own part, both parts (AB), or
/// nothing ("--").
enum class FilterInput : std::uint8_t
{
    none, ///< "--": the filter has no input and isn't run
    own,  ///< A for filter A, B for filter B
    both, ///< AB: both parts, summed
};

/// The global knobs that feed a voice's filters through the Filter Control
/// (filter_control.hpp): the parameter layer's ramps, each positioned at the
/// first sample of the block the voice processes next (or of its note-on),
/// read ahead with LinearSmoother::valueAfter(). A null ramp reads as 0.
struct VoiceFilterKnobs
{
    std::array<const LinearSmoother*, 2> cutoff{};    ///< Cutoff A, Cutoff B (knob units)
    std::array<const LinearSmoother*, 2> resonance{}; ///< Resonance A, Resonance B
    const LinearSmoother* controlCutoff = nullptr;    ///< the Filter Control's Cutoff
    const LinearSmoother* controlResonance = nullptr; ///< the Filter Control's Resonance
    float keytrack = 0.0f;                            ///< the Keytrack knob, 0-10
};

template <std::size_t Taps = minBlepTaps>
class Voice
{
public:
    using Slot = OscillatorSlot<Taps>;
    static constexpr std::size_t partCount = 2;
    static constexpr std::size_t slotsPerPart = 2;
    static constexpr std::size_t slotCount = partCount * slotsPerPart;
    static constexpr std::size_t gridSamples = LadderFilter::gridSamples;
    /// The filter input level's range; anything else (NaN, 0, negative, inf)
    /// counts as 1.
    static constexpr float minInputLevel = 1e-3f;
    static constexpr float maxInputLevel = 1e3f;

    /// The table must outlive the voice. `seed` sets every random sequence
    /// (the slots' free-running phases and noise, the drift): give each voice
    /// its own.
    explicit Voice(const MinBlepTable& residuals, std::uint64_t seed = 0) noexcept
        : slots{Slot{residuals}, Slot{residuals}, Slot{residuals}, Slot{residuals}}
    {
        for (std::size_t s = 0; s < slotCount; ++s)
        {
            slots[s].setRandomSeed(seed ^ (0x9E3779B97F4A7C15U * (s + 1)));
        }
        pitchDrift.setSeed(seed ^ 0x3C6EF372FE94F82BU);
        updateRoutes();
    }

    /// Slots 0 and 1 are A1 and A2; 2 and 3 are B1 and B2.
    [[nodiscard]] Slot& slot(std::size_t index) noexcept
    {
        return slots[index];
    }
    [[nodiscard]] const Slot& slot(std::size_t index) const noexcept
    {
        return slots[index];
    }
    /// The amp envelope of part 0 (A) or 1 (B).
    [[nodiscard]] Adsr& envelope(std::size_t part) noexcept
    {
        return envelopes[part];
    }
    [[nodiscard]] PitchDrift& drift() noexcept
    {
        return pitchDrift;
    }
    /// A part's filter, for measurement. The voice owns its controls: use the
    /// setters below.
    [[nodiscard]] const LadderFilter& filter(std::size_t part) const noexcept
    {
        return filters[part];
    }

    /// From the next grid point while a note sounds; at once before the
    /// first note-on, at a note-on, and at reset().
    void setFilterInput(std::size_t part, FilterInput input) noexcept
    {
        pendingInputs[part] = input;
    }
    [[nodiscard]] FilterInput filterInput(std::size_t part) const noexcept
    {
        return pendingInputs[part];
    }
    /// As setFilterInput(). Leaving Bypass starts the filter from rest.
    void setFilterBypassed(std::size_t part, bool bypassed) noexcept
    {
        pendingBypass[part] = bypassed;
    }
    /// Crossfaded by the filter over 10 ms.
    void setFilterResponse(std::size_t part, FilterResponse response) noexcept
    {
        filters[part].setResponse(response);
    }
    /// The drive gain (inventory F8), ramped by the filter over 10 ms.
    void setFilterDrive(std::size_t part, float gain) noexcept
    {
        filters[part].setDrive(gain);
    }
    /// Warm Drive, for both filters.
    void setWarm(bool on) noexcept
    {
        for (auto& f : filters)
        {
            f.setWarm(on);
        }
    }
    /// The filter input level g (inventory V8): each filter's input is scaled
    /// by g and its output by 1/g, so the clean level is unchanged and the
    /// saturation's knee moves. 1 by default; values outside
    /// [minInputLevel, maxInputLevel], and NaN, count as 1.
    void setFilterInputLevel(float gain) noexcept
    {
        const bool valid = gain >= minInputLevel && gain <= maxInputLevel; // false for NaN
        inputLevel = valid ? gain : 1.0f;
        outputLevel = 1.0f / inputLevel;
        unityLevel = inputLevel >= 1.0f && inputLevel <= 1.0f; // exactly 1, without -Wfloat-equal
    }
    [[nodiscard]] float filterInputLevel() const noexcept
    {
        return inputLevel;
    }

    /// Stereo throughout; nothing is allocated.
    void prepare(const ProcessSpec& spec) noexcept
    {
        const ProcessSpec stereo{
            .sampleRate = spec.sampleRate,
            .maxBlockSize = spec.maxBlockSize,
            .numChannels = 2,
        };
        for (auto& s : slots)
        {
            s.prepare(stereo);
        }
        for (auto& f : filters)
        {
            f.prepare(stereo);
        }
        for (auto& e : envelopes)
        {
            e.prepare(spec);
        }
        pitchDrift.prepare(spec);
        reset();
    }

    /// As freshly prepared: every random sequence restarts from the seed.
    void reset() noexcept
    {
        for (auto& s : slots)
        {
            s.reset();
        }
        for (auto& e : envelopes)
        {
            e.reset();
        }
        pitchDrift.reset();
        applyPending();
        for (auto& f : filters)
        {
            f.reset();
        }
        gridPosition = 0;
        lastOctaves = {};
        lastK = {};
    }

    /// A note-on before the next process() call, with the knobs positioned at
    /// its sample: the slots, the drift and both envelopes start; pending
    /// routing applies; each filter is fed for this sample and reset(), so
    /// the voice's grid starts here.
    void noteOn(float note, const VoiceFilterKnobs& knobs) noexcept
    {
        voiceNote = note;
        for (auto& s : slots)
        {
            s.noteOn();
        }
        pitchDrift.noteOn();
        for (auto& e : envelopes)
        {
            e.gateOn();
        }
        applyPending();
        feedFilters(knobs, 0);
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

    /// True until both amp envelopes have released to 0.
    [[nodiscard]] bool isActive() const noexcept
    {
        return envelopes[0].isActive() || envelopes[1].isActive();
    }

    /// Adds the voice into channels 0 and 1 of `out` (two channels needed),
    /// with the knobs positioned at its first sample.
    void process(const AudioBlock& out, const VoiceFilterKnobs& knobs) noexcept
    {
        const auto n = out.numSamples();
        std::size_t done = 0;
        while (done < n)
        {
            if (gridPosition == 0)
            {
                applyPendingAtGrid(knobs, done);
                feedFilters(knobs, done + gridSamples);
            }
            const auto length = std::min(n - done, gridSamples - gridPosition);
            renderChunk(out.subBlock(done, length));
            done += length;
            gridPosition = (gridPosition + length) % gridSamples;
        }
    }

private:
    using Chunk = std::array<float, gridSamples>;
    using StereoChunk = std::array<Chunk, 2>;

    struct Route
    {
        bool active = false; ///< the filter has an input (not "--")
        bool both = false;   ///< AB
    };

    void updateRoutes() noexcept
    {
        for (std::size_t f = 0; f < partCount; ++f)
        {
            routes[f] = {.active = inputs[f] != FilterInput::none, .both = inputs[f] == FilterInput::both};
        }
        for (std::size_t p = 0; p < partCount; ++p)
        {
            heard[p] = routes[p].active || routes[1 - p].both;
        }
    }

    void applyPending() noexcept
    {
        inputs = pendingInputs;
        for (std::size_t f = 0; f < partCount; ++f)
        {
            filters[f].setBypassed(pendingBypass[f]);
        }
        updateRoutes();
    }

    /// At a grid point: a filter that comes on (from "--" or out of Bypass)
    /// is fed for this sample and starts from rest, on the voice's grid.
    void applyPendingAtGrid(const VoiceFilterKnobs& knobs, std::size_t ahead) noexcept
    {
        std::array<bool, partCount> wasRunning{};
        for (std::size_t f = 0; f < partCount; ++f)
        {
            wasRunning[f] = routes[f].active && !filters[f].isBypassed();
        }
        applyPending();
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed() && !wasRunning[f])
            {
                feedFilter(f, knobs, ahead, keytrackOctaves(voiceNote, knobs.keytrack / 10.0f));
                filters[f].reset();
            }
        }
    }

    [[nodiscard]] static float read(const LinearSmoother* ramp, std::size_t ahead) noexcept
    {
        return ramp != nullptr ? ramp->valueAfter(ahead) : 0.0f;
    }

    void feedFilter(std::size_t f, const VoiceFilterKnobs& knobs, std::size_t ahead, float keytrack) noexcept
    {
        const float octaves = filterCutoffOctaves({
            .cutoff = read(knobs.cutoff[f], ahead),
            .controlCutoff = read(knobs.controlCutoff, ahead),
            .keytrack = keytrack,
        });
        const float k = filterResonanceK({
            .resonance = read(knobs.resonance[f], ahead),
            .controlResonance = read(knobs.controlResonance, ahead),
        });
        if (std::isfinite(octaves))
        {
            lastOctaves[f] = octaves;
        }
        if (std::isfinite(k))
        {
            lastK[f] = k;
        }
        filters[f].setCutoff(filterControlLaw.anchorHz);
        filters[f].setCutoffModulation(lastOctaves[f]);
        filters[f].setResonance(0.0f);
        filters[f].setResonanceModulation(lastK[f]);
    }

    /// Each running filter, for the sample `ahead` samples into the block.
    void feedFilters(const VoiceFilterKnobs& knobs, std::size_t ahead) noexcept
    {
        const float keytrack = keytrackOctaves(voiceNote, knobs.keytrack / 10.0f); // once for both
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (routes[f].active && !filters[f].isBypassed())
            {
                feedFilter(f, knobs, ahead, keytrack);
            }
        }
    }

    [[nodiscard]] AudioBlock block(StereoChunk& chunk, std::array<float*, 2>& pointers, std::size_t length) noexcept
    {
        pointers = {chunk[0].data(), chunk[1].data()};
        return {pointers.data(), 2, length};
    }

    void renderChunk(const AudioBlock& out) noexcept
    {
        const auto length = out.numSamples();

        // 1. The oscillators of each heard part, added into its buffer at the
        // note plus drift, in runs of constant drift.
        std::array<AudioBlock, partCount> parts{};
        for (std::size_t p = 0; p < partCount; ++p)
        {
            parts[p] = block(partBuffers[p], partPointers[p], length);
            if (heard[p])
            {
                parts[p].clear();
            }
        }
        pitchDrift.process(length,
                           [&](std::size_t start, std::size_t run, float offset) noexcept
                           {
                               for (std::size_t p = 0; p < partCount; ++p)
                               {
                                   if (!heard[p])
                                   {
                                       continue;
                                   }
                                   const auto piece = parts[p].subBlock(start, run);
                                   for (std::size_t s = 0; s < slotsPerPart; ++s)
                                   {
                                       auto& slot = slots[(p * slotsPerPart) + s];
                                       if (slot.soundingVoices() > 0)
                                       {
                                           slot.process(piece, voiceNote + offset);
                                       }
                                   }
                               }
                           });

        // 2. The amp envelopes.
        for (std::size_t p = 0; p < partCount; ++p)
        {
            envelopes[p].process(std::span{envelopeBuffers[p]}.first(length));
        }

        // 3. Each running filter's input, in its own buffer; 4. the filter, in
        // place; 5. its output through its part's envelope, into `out`.
        const auto left = out.channel(0);
        const auto right = out.channel(1);
        for (std::size_t f = 0; f < partCount; ++f)
        {
            if (!routes[f].active)
            {
                continue;
            }
            const auto input = block(filterBuffers[f], filterPointers[f], length);
            buildInput(f, length);
            filters[f].process(input);
            const auto& e = envelopeBuffers[f];
            const auto y0 = input.channel(0);
            const auto y1 = input.channel(1);
            if (unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    left[i] += e[i] * y0[i];
                    right[i] += e[i] * y1[i];
                }
            }
            else
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    left[i] += (e[i] * y0[i]) * outputLevel;
                    right[i] += (e[i] * y1[i]) * outputLevel;
                }
            }
        }
    }

    /// own (x g), or for AB own + the other part through its envelope (x g).
    void buildInput(std::size_t f, std::size_t length) noexcept
    {
        const std::size_t other = 1 - f;
        const auto& envelopeOther = envelopeBuffers[other];
        for (std::size_t c = 0; c < 2; ++c)
        {
            const auto& own = partBuffers[f][c];
            const auto& crossing = partBuffers[other][c];
            auto& dst = filterBuffers[f][c];
            if (routes[f].both)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    // Its own statement, so the product is rounded before the
                    // sum on every compiler (no fused multiply-add).
                    const float enveloped = envelopeOther[i] * crossing[i];
                    dst[i] = own[i] + enveloped;
                }
            }
            else
            {
                std::copy_n(own.begin(), length, dst.begin());
            }
            if (!unityLevel)
            {
                for (std::size_t i = 0; i < length; ++i)
                {
                    dst[i] *= inputLevel;
                }
            }
        }
    }

    std::array<Slot, slotCount> slots;
    std::array<LadderFilter, partCount> filters{};
    std::array<Adsr, partCount> envelopes{};
    PitchDrift pitchDrift;

    std::array<FilterInput, partCount> pendingInputs{FilterInput::own, FilterInput::own};
    std::array<bool, partCount> pendingBypass{};
    std::array<FilterInput, partCount> inputs{FilterInput::own, FilterInput::own};
    std::array<Route, partCount> routes{};
    std::array<bool, partCount> heard{};

    float inputLevel = 1.0f;
    float outputLevel = 1.0f;
    bool unityLevel = true;

    std::array<StereoChunk, partCount> partBuffers{};
    std::array<StereoChunk, partCount> filterBuffers{};
    std::array<Chunk, partCount> envelopeBuffers{};
    std::array<std::array<float*, 2>, partCount> partPointers{};
    std::array<std::array<float*, 2>, partCount> filterPointers{};

    std::array<float, partCount> lastOctaves{};
    std::array<float, partCount> lastK{};
    float voiceNote = 69.0f;
    std::size_t gridPosition = 0;
};

} // namespace anoesis::dsp
