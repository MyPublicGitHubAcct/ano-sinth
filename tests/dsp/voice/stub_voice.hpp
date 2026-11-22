// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// A stub voice for VoiceAllocator's tests (voice_allocator_test.cpp,
// legato_test.cpp): it renders nothing and records what the allocator tells
// it. Active from a note-on until `release` samples after its note-off (0:
// at once), counted in the blocks it is given; one oscillator voice while
// active. Its pitch is the last note's, unless a test sets `pitchNow`.

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/voice/voice_allocator.hpp>
#include <anoesis/dsp/voice/voice_knobs.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace anoesis::test
{

/// The kind of the last call a stub voice was given.
enum class StubCall : std::uint8_t
{
    none,
    noteOn,
    legato,
    noteOff,
    stop,
};

class StubVoice
{
public:
    using Knobs = dsp::VoiceKnobs;

    explicit StubVoice(std::uint64_t voiceSeed) noexcept : seed(voiceSeed) {}

    void prepare(const dsp::ProcessSpec& /*spec*/) noexcept {}
    void reset() noexcept
    {
        active = false;
        gate = false;
        played = false;
    }
    void noteOn(float key, const Knobs& knobs, const dsp::NoteStart& start) noexcept
    {
        retriggers += active ? 1 : 0;
        ++noteOns;
        note = key;
        gate = true;
        active = true;
        played = true;
        lastOffset = knobs.offset;
        lastStart = start;
        lastCall = StubCall::noteOn;
        pitchNow.reset();
    }
    void legatoNoteOn(float key, const Knobs& knobs, const dsp::NoteStart& start) noexcept
    {
        ++legatoNotes;
        note = key;
        gate = true;
        active = true;
        played = true;
        lastOffset = knobs.offset;
        lastStart = start;
        lastCall = StubCall::legato;
        pitchNow.reset();
    }
    void noteOff() noexcept
    {
        ++noteOffs;
        gate = false;
        remaining = release;
        active = release > 0;
        lastCall = StubCall::noteOff;
    }
    void stop() noexcept
    {
        ++stops;
        active = false;
        gate = false;
        lastCall = StubCall::stop;
    }
    void process(const dsp::AudioBlock& out, const Knobs& /*knobs*/) noexcept
    {
        rendered += out.numSamples();
        if (active && !gate)
        {
            remaining -= std::min(remaining, out.numSamples());
            active = remaining > 0;
        }
    }
    [[nodiscard]] bool isActive() const noexcept
    {
        return active;
    }
    [[nodiscard]] std::size_t activeOscillatorVoices() const noexcept
    {
        return active ? 1 : 0;
    }
    [[nodiscard]] static std::size_t oscillatorVoicesPerNote() noexcept
    {
        return 1;
    }
    [[nodiscard]] float currentPitch() const noexcept
    {
        return pitchNow.value_or(note);
    }
    [[nodiscard]] bool hasPlayed() const noexcept
    {
        return played;
    }
    [[nodiscard]] float velocity() const noexcept
    {
        return lastStart.velocity;
    }

    std::uint64_t seed;
    std::size_t release = 0;
    std::size_t remaining = 0;
    bool active = false;
    bool gate = false;
    bool played = false;
    float note = 0.0f;
    std::optional<float> pitchNow; ///< set by a test: the pitch a glide has reached
    std::size_t noteOns = 0;
    std::size_t legatoNotes = 0;
    std::size_t noteOffs = 0;
    std::size_t retriggers = 0; ///< note-ons on an active voice
    std::size_t stops = 0;
    std::size_t rendered = 0;
    std::size_t lastOffset = 0;
    dsp::NoteStart lastStart{};
    StubCall lastCall = StubCall::none;
};

static_assert(dsp::AllocatableVoice<StubVoice>);

} // namespace anoesis::test
