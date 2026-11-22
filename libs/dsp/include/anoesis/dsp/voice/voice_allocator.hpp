// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/voice/pitch_bend.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
///   Otherwise it steals the oldest voice whose key is up and isn't held by
///   the pedal (released), else the oldest sustained by the pedal, else the
///   oldest; and retriggers it in place: the voice's noteOn() on a
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
/// Mono Legato, portamento, the pitch bend and the hold pedal (M1.4 task 3,
/// chosen by the project owner from measured candidates,
/// docs/dsp/legato-portamento.md):
///
/// - Mono Legato: voice 0 alone, whatever Polyphony (0 included), with a
///   stack of the keys held. Last-note priority: the most recent key held
///   sounds; releasing it returns to the key held before it as a legato
///   note (the voice's legatoNoteOn()). A key pressed while none is held is
///   a new note (a retrigger in place, from the envelopes' level). A switch
///   of Mono Legato releases every sounding note; keys still down sound
///   when next played.
/// - Portamento: a time in seconds (0: none) and a mode. Normal glides only
///   while another key is held; Slide always, and after a gap the next note
///   is a new note that glides. A glide starts from the pitch reached: in
///   Mono Legato the voice's own; in poly the last note played (its voice's
///   pitch now, if that voice still plays it, else its key). A gliding
///   note's velocity ramps from the last note's.
/// - The bend: one PitchBend for every voice, set at each bend event at its
///   absolute sample (counted from prepare() or reset()) and read by every
///   sounding voice, releasing ones included, through its knobs.
/// - The hold pedal (CC 64, down at 64 and above): a key released under it
///   keeps its gate; pedal-up releases the voices whose keys are up; a key
///   played again under it takes a new voice. For stealing, a sustained note
///   comes after released ones and before held keys. In Mono Legato the
///   stack follows the keys, the pedal holds the last note's gate, and a key
///   played while it does is legato.
///
/// The arpeggiator's voices are M1.6's.
/// Doc: docs/dsp/voice-allocation.md, docs/dsp/legato-portamento.md
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
concept AllocatableVoice = std::same_as<typename V::Knobs, VoiceKnobs> &&
                           requires(V& v, const V& cv, float note, const VoiceKnobs& knobs, const NoteStart& start,
                                    const AudioBlock& block, const ProcessSpec& spec) {
                               v.prepare(spec);
                               v.reset();
                               v.noteOn(note, knobs, start);
                               v.legatoNoteOn(note, knobs, start);
                               v.noteOff();
                               v.stop();
                               v.process(block, knobs);
                               { cv.isActive() } -> std::convertible_to<bool>;
                               { cv.activeOscillatorVoices() } -> std::convertible_to<std::size_t>;
                               { cv.oscillatorVoicesPerNote() } -> std::convertible_to<std::size_t>;
                               { cv.currentPitch() } -> std::convertible_to<float>;
                               { cv.hasPlayed() } -> std::convertible_to<bool>;
                               { cv.velocity() } -> std::convertible_to<float>;
                           };

enum class VoiceEventKind : std::uint8_t
{
    noteOn,     ///< value: the key
    noteOff,    ///< value: the key
    polyphony,  ///< value: the new limit, clamped to [0, maxVoices]
    pedal,      ///< value: the hold pedal's CC 64, 0-127
    pitchBend,  ///< value: the 14-bit bend, 0-16383
    monoLegato, ///< value: Mono Legato off (0) or on
};

/// Portamento's modes (manual p. 29).
enum class PortamentoMode : std::uint8_t
{
    normal, ///< glides only while at least one other key is held
    slide,  ///< always glides to the next note, from the last
};

/// An event at a sample offset within the block it is passed with.
struct VoiceEvent
{
    std::size_t offset = 0;
    VoiceEventKind kind = VoiceEventKind::noteOn;
    int value = 0;
    int velocity = 127; ///< a note-on's MIDI velocity, 1-127
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
        bend.prepare(spec.sampleRate);
        clear();
    }

    /// Every voice reset() (as freshly prepared) and free, no key held, the
    /// pedal up, the bend at 0; the settings (Polyphony, Mono Legato,
    /// portamento, Bend Range) kept.
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

    /// Mono Legato, from now (between blocks; within a block, a monoLegato
    /// event). A change releases every sounding note.
    void setMonoLegato(bool on) noexcept
    {
        if (on == mono)
        {
            return;
        }
        mono = on;
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            auto& r = records[i];
            if (r.busy && (r.keyDown || r.sustained))
            {
                voices[i].noteOff();
            }
            r.keyDown = false;
            r.sustained = false;
        }
        stackSize = 0;
        monoNote = -1;
    }
    [[nodiscard]] bool monoLegato() const noexcept
    {
        return mono;
    }
    /// Portamento's time in seconds (0: no glide) and its mode, for the
    /// glides that start from now.
    void setPortamento(float seconds) noexcept
    {
        portamentoSeconds = std::isnan(seconds) ? 0.0f : std::max(seconds, 0.0f);
    }
    void setPortamentoMode(PortamentoMode newMode) noexcept
    {
        mode = newMode;
    }
    /// The Bend Range in semitones (0-24), from the next bend value.
    void setBendRange(float semitones) noexcept
    {
        bend.setRange(semitones);
    }

    /// Mono Legato's sounding key (-1 when none has played since the last
    /// prepare(), reset() or switch), the pedal, and a voice held by it.
    [[nodiscard]] int monoKey() const noexcept
    {
        return monoNote;
    }
    [[nodiscard]] bool isPedalDown() const noexcept
    {
        return pedalDown;
    }
    [[nodiscard]] bool isSustained(std::size_t index) const noexcept
    {
        return records[index].busy && records[index].sustained;
    }

    /// Adds the busy voices into `out`, handling `events` (sorted by offset;
    /// an offset past the block counts as its end) at their samples, with
    /// the knobs positioned at the block's first sample. The knobs' bend and
    /// time are the allocator's own.
    void process(const AudioBlock& out, std::span<const VoiceEvent> events, const Knobs& blockKnobs) noexcept
    {
        Knobs knobs = blockKnobs;
        knobs.bend = &bend;
        knobs.time = clock;
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
        clock += n;
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
    static constexpr std::size_t keyCount = 128;

    struct Record
    {
        bool busy = false;
        bool keyDown = false;
        bool sustained = false; ///< key up, held by the pedal
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
        keys = {};
        stack = {};
        stackSize = 0;
        monoNote = -1;
        lastKey = -1;
        lastVelocity = 1.0f;
        newest = MaxVoices;
        pedalDown = false;
        bend.reset();
        clock = 0;
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
        const int key = std::clamp(event.value, 0, static_cast<int>(keyCount) - 1);
        switch (event.kind)
        {
        case VoiceEventKind::noteOn:
        {
            const float velocity = static_cast<float>(std::clamp(event.velocity, 1, 127)) / 127.0f;
            keys[static_cast<std::size_t>(key)] = true;
            if (mono)
            {
                monoNoteOn(key, velocity, knobs);
            }
            else
            {
                noteOn(key, velocity, knobs);
            }
            lastKey = key;
            lastVelocity = velocity;
            break;
        }
        case VoiceEventKind::noteOff:
            keys[static_cast<std::size_t>(key)] = false;
            if (mono)
            {
                monoNoteOff(key, knobs);
            }
            else
            {
                noteOff(key);
            }
            break;
        case VoiceEventKind::polyphony:
            setPolyphony(static_cast<std::size_t>(std::max(event.value, 0)));
            break;
        case VoiceEventKind::pedal:
            pedal(event.value >= 64);
            break;
        case VoiceEventKind::pitchBend:
            bend.setValue(knobs.time, event.value);
            break;
        case VoiceEventKind::monoLegato:
            setMonoLegato(event.value != 0);
            break;
        }
    }

    /// Whether a key other than `key` is down (Normal's condition).
    [[nodiscard]] bool otherKeyHeld(int key) const noexcept
    {
        for (std::size_t k = 0; k < keyCount; ++k)
        {
            if (keys[k] && std::cmp_not_equal(k, key))
            {
                return true;
            }
        }
        return false;
    }

    // --- Poly ---

    void noteOn(int key, float velocity, const Knobs& knobs) noexcept
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
        NoteStart start{.velocity = velocity};
        const bool slides = mode == PortamentoMode::slide || otherKeyHeld(key);
        if (slides && portamentoSeconds > 0.0f && lastKey >= 0)
        {
            // From the last note played: its voice's pitch and velocity now,
            // if it still plays it, else its key and velocity.
            const bool sounding = newest < MaxVoices && records[newest].busy && records[newest].note == lastKey;
            start.glide = true;
            start.from = sounding ? voices[newest].currentPitch() : static_cast<float>(lastKey);
            start.glideSeconds = portamentoSeconds;
            start.fromVelocity = sounding ? voices[newest].velocity() : lastVelocity;
        }
        records[chosen] = {.busy = true, .keyDown = true, .sustained = false, .note = key, .age = ++counter};
        voices[chosen].noteOn(static_cast<float>(key), knobs, start);
        newest = chosen;
    }

    void noteOff(int key) noexcept
    {
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            auto& r = records[i];
            if (r.busy && r.keyDown && r.note == key)
            {
                r.keyDown = false;
                if (pedalDown)
                {
                    r.sustained = true;
                }
                else
                {
                    voices[i].noteOff();
                }
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

    /// The steal order's tier: released notes (0), then those the pedal
    /// sustains (1), then held keys (2).
    [[nodiscard]] static int tier(const Record& r) noexcept
    {
        if (r.keyDown)
        {
            return 2;
        }
        return r.sustained ? 1 : 0;
    }

    /// The oldest busy voice of the lowest tier. Only called with at least
    /// one voice busy (Polyphony >= 1 notes are).
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
            const int rt = tier(r);
            const int bt = tier(b);
            const bool better = !found || (rt != bt ? rt < bt : r.age < b.age);
            if (better)
            {
                best = i;
                found = true;
            }
        }
        return best;
    }

    // --- The pedal ---

    void pedal(bool down) noexcept
    {
        if (down == pedalDown)
        {
            return;
        }
        pedalDown = down;
        if (down)
        {
            return;
        }
        if (mono)
        {
            if (records[0].busy && records[0].sustained)
            {
                records[0].sustained = false;
                voices[0].noteOff();
            }
            return;
        }
        for (std::size_t i = 0; i < MaxVoices; ++i)
        {
            auto& r = records[i];
            if (r.busy && r.sustained && !r.keyDown)
            {
                r.sustained = false;
                voices[i].noteOff();
            }
        }
    }

    // --- Mono Legato ---

    void push(int key) noexcept
    {
        remove(key);
        stack[stackSize] = key;
        ++stackSize;
    }
    void remove(int key) noexcept
    {
        const auto held = std::span{stack}.first(stackSize);
        const auto it = std::ranges::find(held, key);
        if (it != held.end())
        {
            std::copy(it + 1, held.end(), it);
            --stackSize;
        }
    }

    /// A note on the mono voice: legato while keys are held and it sounds,
    /// else a new note (a retrigger in place, or from silence). It glides
    /// from the pitch the voice has reached: always in Slide; in Normal only
    /// as a legato note.
    void monoPlay(int key, float velocity, bool legato, const Knobs& knobs) noexcept
    {
        auto& v = voices[0];
        NoteStart start{.velocity = velocity, .fromVelocity = v.velocity()};
        if (portamentoSeconds > 0.0f && v.hasPlayed() && (legato || mode == PortamentoMode::slide))
        {
            start.glide = true;
            start.from = v.currentPitch();
            start.glideSeconds = portamentoSeconds;
        }
        if (legato && records[0].busy && v.isActive())
        {
            v.legatoNoteOn(static_cast<float>(key), knobs, start);
        }
        else
        {
            v.noteOn(static_cast<float>(key), knobs, start);
        }
        records[0] = {.busy = true, .keyDown = true, .sustained = false, .note = key, .age = ++counter};
        monoNote = key;
    }

    void monoNoteOn(int key, float velocity, const Knobs& knobs) noexcept
    {
        // A note the pedal holds counts as held: a key played under it is
        // legato.
        const bool pedalHeld = records[0].busy && records[0].sustained && voices[0].isActive();
        const bool held = stackSize > 0 || pedalHeld;
        push(key);
        if (!held)
        {
            monoPlay(key, velocity, false, knobs);
            return;
        }
        if (key != monoNote || !records[0].busy)
        {
            monoPlay(key, velocity, true, knobs);
        }
    }

    void monoNoteOff(int key, const Knobs& knobs) noexcept
    {
        remove(key);
        if (!records[0].busy)
        {
            return;
        }
        if (stackSize == 0)
        {
            if (records[0].keyDown)
            {
                records[0].keyDown = false;
                if (pedalDown)
                {
                    records[0].sustained = true;
                }
                else
                {
                    voices[0].noteOff();
                }
            }
            return;
        }
        const int next = stack[stackSize - 1];
        if (next != monoNote)
        {
            monoPlay(next, voices[0].velocity(), true, knobs);
        }
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

    bool mono = false;
    PortamentoMode mode = PortamentoMode::normal;
    float portamentoSeconds = 0.0f;
    PitchBend bend;
    std::uint64_t clock = 0; ///< samples since prepare() or reset()
    std::array<bool, keyCount> keys{};
    std::array<int, keyCount> stack{};
    std::size_t stackSize = 0;
    int monoNote = -1;
    int lastKey = -1;
    float lastVelocity = 1.0f;
    std::size_t newest = MaxVoices;
    bool pedalDown = false;
};

} // namespace anoesis::dsp
