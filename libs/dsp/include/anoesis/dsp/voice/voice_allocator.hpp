// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

/// The voice allocator: Sylenth1's Polyphony (0-16 notes) over a fixed pool
/// of voices (inventory §7, "Voice allocation"). Chosen by the project owner
/// from measured candidates (docs/dsp/voice-allocation.md):
///
/// - A note-on takes a free voice while fewer than Polyphony notes are busy.
///   Otherwise it steals the oldest voice whose key is up (released), else
///   the oldest; and retriggers it in place: the voice's noteOn() on a
///   sounding voice (for dsp::Voice: the slots as Retrig says, the
///   envelopes from their level, the filters and grid kept).
/// - A key played again takes a new voice; the old note's release overlaps
///   (the developer, 2016). A note-off releases every voice on that key
///   whose key is down, whatever the limit.
/// - A voice is free once isActive() turns false: checked at every event
///   and at each voice's own points every 32 samples from its latest
///   note-on, never at host-block boundaries, so the output doesn't depend
///   on the host's block size. A freed voice is stop()ped, so its next note
///   starts from silence, and isn't rendered.
/// - Polyphony lowered: sounding notes play on, and only new notes obey (a
///   new note above the limit steals). Polyphony 0 ignores new notes.
/// - The Voices readout: activeVoices(), the sum of each busy voice's
///   activeOscillatorVoices(); maximumVoices(), voice 0's
///   oscillatorVoicesPerNote() times Polyphony (every voice plays the same
///   patch).
///
/// The host block is split at its events (sorted by offset); every busy
/// voice is rendered exactly up to each event, in pieces that stop at its
/// check points, with the knobs advanced to each piece's start. Each voice
/// is constructed in place from the constructor's arguments followed by its
/// seed (voiceSeed()). Nothing allocates; prepare() is the voices'.
///
/// Mono Legato, portamento, the hold pedal and the arpeggiator's voices are
/// M1.4 task 3's and M1.6's.
/// Doc: docs/dsp/voice-allocation.md
namespace anoesis::dsp
{

/// A voice's seed: the instance's, mixed with the voice's index, so every
/// voice's random sequences differ.
[[nodiscard]] constexpr std::uint64_t voiceSeed(std::uint64_t seed, std::size_t index) noexcept
{
    return seed ^ (0xD1B54A32D192ED03U * (static_cast<std::uint64_t>(index) + 1));
}

/// What VoiceAllocator needs from a voice (dsp::Voice has it): the
/// lifecycle, a note-on that retriggers a sounding voice in place, stop()
/// for a finished voice, the readout's counts, and knobs that can be
/// advanced into a block.
template <typename V>
concept AllocatableVoice = requires(V& v, const V& cv, float note, const V::Knobs& knobs, const AudioBlock& block,
                                    const ProcessSpec& spec, std::size_t samples) {
    v.prepare(spec);
    v.reset();
    v.noteOn(note, knobs);
    v.noteOff();
    v.stop();
    v.process(block, knobs);
    { cv.isActive() } -> std::convertible_to<bool>;
    { cv.activeOscillatorVoices() } -> std::convertible_to<std::size_t>;
    { cv.oscillatorVoicesPerNote() } -> std::convertible_to<std::size_t>;
    { knobs.advanced(samples) } -> std::convertible_to<typename V::Knobs>;
};

enum class VoiceEventKind : std::uint8_t
{
    noteOn,    ///< value: the key
    noteOff,   ///< value: the key
    polyphony, ///< value: the new limit, clamped to [0, maxVoices]
};

/// An event at a sample offset within the block it is passed with.
struct VoiceEvent
{
    std::size_t offset = 0;
    VoiceEventKind kind = VoiceEventKind::noteOn;
    int value = 0;
};

template <AllocatableVoice V, std::size_t MaxVoices = 16>
class VoiceAllocator
{
public:
    using Knobs = V::Knobs;
    static constexpr std::size_t maxVoices = MaxVoices;
    /// A busy voice is checked for having finished every this many samples
    /// from its latest note-on (and at every event).
    static constexpr std::size_t checkSamples = 32;

    /// Voice i is constructed as V{args..., voiceSeed(seed, i)}.
    template <typename... Args>
    explicit VoiceAllocator(std::uint64_t seed, const Args&... args) noexcept
        : voices(make(seed, std::make_index_sequence<MaxVoices>{}, args...))
    {
    }

    VoiceAllocator(const VoiceAllocator&) = delete;
    VoiceAllocator& operator=(const VoiceAllocator&) = delete;
    VoiceAllocator(VoiceAllocator&&) = delete;
    VoiceAllocator& operator=(VoiceAllocator&&) = delete;
    ~VoiceAllocator() = default;

    [[nodiscard]] V& voice(std::size_t index) noexcept
    {
        return voices[index];
    }
    [[nodiscard]] const V& voice(std::size_t index) const noexcept
    {
        return voices[index];
    }

    /// Calls `configure` on every voice (a patch, the same on all).
    template <typename F>
    void forEachVoice(const F& configure)
    {
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            configure(voices[i]);
        }
    }

    /// Prepares every voice; then as reset().
    void prepare(const ProcessSpec& spec) noexcept
    {
        for (auto& v : voices)
        {
            v.prepare(spec);
        }
        clear();
    }

    /// Every voice reset() (as freshly prepared) and free; Polyphony kept.
    void reset() noexcept
    {
        for (auto& v : voices)
        {
            v.reset();
        }
        clear();
    }

    /// The limit, clamped to [0, maxVoices], from now (between blocks; within
    /// a block, a polyphony event).
    void setPolyphony(std::size_t notes) noexcept
    {
        limit = std::min(notes, MaxVoices);
    }
    [[nodiscard]] std::size_t polyphony() const noexcept
    {
        return limit;
    }

    /// Adds the busy voices into `out`, handling `events` (sorted by offset;
    /// an offset past the block counts as its end) at their samples, with
    /// the knobs positioned at the block's first sample.
    void process(const AudioBlock& out, std::span<const VoiceEvent> events, const Knobs& knobs) noexcept
    {
        const auto n = out.numSamples();
        std::size_t from = 0;
        std::size_t e = 0;
        while (from < n)
        {
            while (e < events.size() && events[e].offset <= from)
            {
                handle(events[e], knobs.advanced(from));
                ++e;
            }
            const auto to = e < events.size() ? std::min(n, events[e].offset) : n;
            for (std::size_t i = 0; i < MaxVoices; ++i)
            {
                if (records[i].busy)
                {
                    render(i, out, from, to, knobs);
                }
            }
            from = to;
        }
        for (; e < events.size(); ++e)
        {
            handle(events[e], knobs.advanced(n));
        }
    }

    /// The Voices readout: busy notes; their active oscillator voices; the
    /// maximum (voices per note x Polyphony).
    [[nodiscard]] std::size_t activeNotes() const noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(records, &Record::busy));
    }
    [[nodiscard]] std::size_t activeVoices() const noexcept
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            total += records[i].busy ? static_cast<std::size_t>(voices[i].activeOscillatorVoices()) : 0;
        }
        return total;
    }
    [[nodiscard]] std::size_t maximumVoices() const noexcept
    {
        return static_cast<std::size_t>(voices[0].oscillatorVoicesPerNote()) * limit;
    }

    /// A voice's state: busy (playing a note, its key down or released), its
    /// key (-1 when free), its key down.
    [[nodiscard]] bool isBusy(std::size_t index) const noexcept
    {
        return records[index].busy;
    }
    [[nodiscard]] int noteOf(std::size_t index) const noexcept
    {
        return records[index].busy ? records[index].note : -1;
    }
    [[nodiscard]] bool isKeyDown(std::size_t index) const noexcept
    {
        return records[index].busy && records[index].keyDown;
    }

    /// Counters since prepare() or reset(): note-ons that stole a voice, and
    /// note-ons ignored at Polyphony 0.
    [[nodiscard]] std::size_t steals() const noexcept
    {
        return stealCount;
    }
    [[nodiscard]] std::size_t ignored() const noexcept
    {
        return ignoredCount;
    }

private:
    struct Record
    {
        bool busy = false;
        bool keyDown = false;
        int note = 0;
        std::uint64_t age = 0;      ///< note-on order
        std::size_t sinceCheck = 0; ///< samples since the last check point
    };

    template <std::size_t... I, typename... Args>
    static std::array<V, MaxVoices> make(std::uint64_t seed, std::index_sequence<I...> /*indices*/,
                                         const Args&... args) noexcept
    {
        return {V{args..., voiceSeed(seed, I)}...};
    }

    void clear() noexcept
    {
        records = {};
        counter = 0;
        stealCount = 0;
        ignoredCount = 0;
    }

    void free(std::size_t i) noexcept
    {
        voices[i].stop();
        records[i] = {};
    }

    void freeFinished() noexcept
    {
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            if (records[i].busy && !voices[i].isActive())
            {
                free(i);
            }
        }
    }

    void handle(const VoiceEvent& event, const Knobs& knobs) noexcept
    {
        freeFinished();
        switch (event.kind)
        {
        case VoiceEventKind::noteOn:
            noteOn(event.value, knobs);
            break;
        case VoiceEventKind::noteOff:
            noteOff(event.value);
            break;
        case VoiceEventKind::polyphony:
            setPolyphony(static_cast<std::size_t>(std::max(event.value, 0)));
            break;
        }
    }

    void noteOn(int key, const Knobs& knobs) noexcept
    {
        if (limit == 0)
        {
            ++ignoredCount;
            return;
        }
        std::size_t chosen = MaxVoices;
        if (activeNotes() < limit)
        {
            chosen = firstFree();
        }
        if (chosen == MaxVoices)
        {
            chosen = victim();
            ++stealCount;
        }
        records[chosen] = {.busy = true, .keyDown = true, .note = key, .age = ++counter, .sinceCheck = 0};
        voices[chosen].noteOn(static_cast<float>(key), knobs);
    }

    void noteOff(int key) noexcept
    {
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            auto& r = records[i];
            if (r.busy && r.keyDown && r.note == key)
            {
                r.keyDown = false;
                voices[i].noteOff();
            }
        }
    }

    [[nodiscard]] std::size_t firstFree() const noexcept
    {
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            if (!records[i].busy)
            {
                return i;
            }
        }
        return MaxVoices;
    }

    /// The oldest busy voice whose key is up, else the oldest. Only called
    /// with at least one voice busy (Polyphony >= 1 notes are).
    [[nodiscard]] std::size_t victim() const noexcept
    {
        std::size_t best = 0;
        bool found = false;
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            const auto& r = records[i];
            if (!r.busy)
            {
                continue;
            }
            const auto& b = records[best];
            const bool better = !found || (r.keyDown != b.keyDown ? !r.keyDown : r.age < b.age);
            if (better)
            {
                best = i;
                found = true;
            }
        }
        return best;
    }

    /// Voice i over [from, to) of `out`, in pieces that stop at its check
    /// points; freed (and no longer rendered) at the first one it has
    /// finished by.
    void render(std::size_t i, const AudioBlock& out, std::size_t from, std::size_t to, const Knobs& knobs) noexcept
    {
        auto& r = records[i];
        while (from < to)
        {
            const auto length = std::min(to - from, checkSamples - r.sinceCheck);
            voices[i].process(out.subBlock(from, length), knobs.advanced(from));
            from += length;
            r.sinceCheck = (r.sinceCheck + length) % checkSamples;
            if (r.sinceCheck == 0 && !voices[i].isActive())
            {
                free(i);
                return;
            }
        }
    }

    std::array<V, MaxVoices> voices;
    std::array<Record, MaxVoices> records{};
    std::size_t limit = MaxVoices;
    std::uint64_t counter = 0;
    std::size_t stealCount = 0;
    std::size_t ignoredCount = 0;
};

} // namespace anoesis::dsp
