// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Part solo and skipping as built (M1.4 task 5, stage 2; docs/dsp/
// part-solo-and-skipping.md): the library's voice with Solo's ramps as M1.8's
// engine will assemble them, for the tests, the report and the benchmarks.
//
// LibrarySoloEngine is the stage-1 SoloEngine (solo_candidates.hpp) with the
// library in place of the prototype: dsp::VoiceAllocator over dsp::Voice, the
// Mix A/B ramps read through VoiceKnobs::mix, and dsp::PartSolo's two ramps
// through VoiceKnobs::solo. It takes the same EngineEvents and splits each
// host block at them in the same way, so the two render the same sequence.
// decidedContext() is the prototype's switches in the configuration the
// project owner chose.

#pragma once

#include "solo_candidates.hpp"

#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/voice/part_solo.hpp>
#include <anoesis/dsp/voice/voice.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace anoesis::test::solo
{

/// The prototype's switches as decided (2026-11-22): the fed filter frozen,
/// the slots skipped with their phases advanced, the test per segment with no
/// hysteresis, the mono path on, Solo a ramped gain under the filter-output
/// reading.
[[nodiscard]] inline SkipContext decidedContext() noexcept
{
    SkipContext c;
    c.filterSkip = FilterSkip::freeze;
    c.skipOscillators = true;
    c.advancePhase = true;
    c.timing = SkipTiming::perSegment;
    c.mono = true;
    c.soloReading = SoloReading::filterOutput;
    c.soloByRouting = false;
    return c;
}

/// The prototype with nothing switched on: dsp::Voice as it was before stage
/// 2 (no skip, no mono path, and no solo ramps: routing mode with Solo off
/// leaves the routes alone and the solo pointers null).
[[nodiscard]] inline SkipContext asItWasContext() noexcept
{
    SkipContext c;
    c.soloByRouting = true;
    return c;
}

/// 16 dsp::Voices through dsp::VoiceAllocator, with Mix A/B (10 ms) and
/// dsp::PartSolo, splitting each host block at its non-note events. The
/// output is written, not added to. `withSolo` false leaves VoiceKnobs::solo
/// null (a caller without Solo).
class LibrarySoloEngine
{
public:
    using Voice = dsp::Voice<>;
    using Allocator = dsp::VoiceAllocator<Voice, 16>;
    static constexpr float rampSeconds = 0.01f;

    explicit LibrarySoloEngine(const dsp::MinBlepTable& table, bool withSolo = true, std::uint64_t seed = 1)
        : allocator(std::make_unique<Allocator>(seed, table)), soloKnobs(withSolo)
    {
        for (auto& r : mixRamps)
        {
            r.setRampTime(rampSeconds);
            r.setCurrentAndTarget(10.0f);
        }
    }

    [[nodiscard]] Allocator& voices() noexcept
    {
        return *allocator;
    }
    [[nodiscard]] FilterKnobs& filterKnobs() noexcept
    {
        return filters;
    }
    [[nodiscard]] const dsp::PartSolo& solo() const noexcept
    {
        return partSolo;
    }
    void setMix(std::size_t part, float internal) noexcept
    {
        mixRamps[part].setCurrentAndTarget(internal);
    }
    /// Solo and the selected part at once, with no ramp (after prepare()).
    void setSolo(bool on, std::size_t selected) noexcept
    {
        partSolo.setImmediately(on, selected);
    }

    void prepare(const dsp::ProcessSpec& spec)
    {
        for (auto& r : mixRamps)
        {
            r.prepare(spec);
        }
        partSolo.prepare(spec);
        allocator->prepare({.sampleRate = spec.sampleRate, .maxBlockSize = spec.maxBlockSize, .numChannels = 2});
        notes.assign(256, {});
        clock = 0;
    }

    void process(const dsp::AudioBlock& out, std::span<const EngineEvent> events) noexcept
    {
        const auto n = out.numSamples();
        out.clear();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            std::size_t count = 0;
            std::size_t to = n;
            while (e < events.size())
            {
                const auto at = std::min(events[e].offset, n);
                if (events[e].kind != EngineEventKind::voice)
                {
                    if (at > from)
                    {
                        to = at;
                        break;
                    }
                    apply(events[e]);
                }
                else if (count < notes.size())
                {
                    notes[count] = events[e].voice;
                    notes[count].offset = at - from;
                    ++count;
                }
                ++e;
            }
            render(out.subBlock(from, to - from), std::span{notes}.first(count));
            from = to;
        }
        std::size_t count = 0;
        for (; e < events.size(); ++e)
        {
            if (events[e].kind != EngineEventKind::voice)
            {
                apply(events[e]);
            }
            else if (count < notes.size())
            {
                notes[count] = events[e].voice;
                notes[count].offset = 0;
                ++count;
            }
        }
        if (count > 0)
        {
            allocator->process(out.subBlock(n, 0), std::span{notes}.first(count), knobs());
        }
    }

private:
    [[nodiscard]] dsp::VoiceKnobs knobs() const noexcept
    {
        auto k = filters.knobs();
        k.mix = {mixRamps.data(), mixRamps.data() + 1};
        if (soloKnobs)
        {
            k.solo = partSolo.gains();
        }
        k.time = clock;
        return k;
    }

    void apply(const EngineEvent& event) noexcept
    {
        switch (event.kind)
        {
        case EngineEventKind::voice:
            break;
        case EngineEventKind::mix:
            mixRamps[std::min<std::size_t>(event.part, 1)].setTarget(event.value);
            break;
        case EngineEventKind::solo:
            partSolo.setSolo(event.value > 0.5f);
            break;
        case EngineEventKind::select:
            partSolo.selectPart(event.part);
            break;
        case EngineEventKind::slotVolume:
            allocator->forEachVoice([&](Voice& v) { v.slot(event.part).setVolume(event.value); });
            break;
        case EngineEventKind::slotPan:
            allocator->forEachVoice([&](Voice& v) { v.slot(event.part).setPan(event.value); });
            break;
        }
    }

    void render(const dsp::AudioBlock& piece, std::span<const dsp::VoiceEvent> pieceNotes) noexcept
    {
        const auto length = piece.numSamples();
        allocator->process(piece, pieceNotes, knobs());
        for (auto& r : mixRamps)
        {
            r.skip(length);
        }
        partSolo.skip(length);
        clock += length;
    }

    std::unique_ptr<Allocator> allocator;
    bool soloKnobs = true;
    FilterKnobs filters;
    std::array<dsp::LinearSmoother, 2> mixRamps{};
    dsp::PartSolo partSolo;
    std::vector<dsp::VoiceEvent> notes;
    std::uint64_t clock = 0;
};

} // namespace anoesis::test::solo
