// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

/// When a voice compiles its routes (M1.5 task 2, docs/dsp/modulation.md,
/// recommendation 3): at a note-on, and at the first of the voice's own
/// 32-sample grid points after the patch changes.
///
/// The engine owns one Patch: the 16 slots and a version that every change
/// bumps. Each voice owns a RouteCompiler and calls update() at its note-ons
/// and at each of its grid points (counted from its note-on, as everything
/// else on the voice's grid is). update() compiles only if the version moved
/// since the last compile, so a patch change mid-segment takes effect at the
/// voice's next grid point, wherever the host's blocks fall, and a patch that
/// doesn't change costs one comparison per grid point.
///
/// No allocation, real-time safe. Not thread safe: the engine changes the
/// patch on the audio thread, between the voices' process() calls.
/// Doc: docs/dsp/modulation.md
#include <anoesis/dsp/modulation/routing.hpp>

#include <cstddef>
#include <cstdint>

namespace anoesis::dsp::modulation
{

/// The engine's modulation patch: the 16 slots and a change counter.
class Patch
{
public:
    /// Sets slot `index` (0-15; others are ignored) and bumps the version.
    constexpr void set(std::size_t index, const Slot& slot) noexcept
    {
        if (index >= slotCount)
        {
            return;
        }
        slotValues[index] = slot;
        ++changeCount;
    }

    /// Sets every slot (a preset) and bumps the version.
    constexpr void set(const Slots& all) noexcept
    {
        slotValues = all;
        ++changeCount;
    }

    [[nodiscard]] constexpr const Slots& slots() const noexcept
    {
        return slotValues;
    }

    /// Bumped by every change. It wraps; a voice recompiles on any change of
    /// value, so only exactly 2^32 changes between two of its grid points
    /// could go unseen.
    [[nodiscard]] constexpr std::uint32_t version() const noexcept
    {
        return changeCount;
    }

private:
    Slots slotValues{};
    std::uint32_t changeCount = 0;
};

/// A voice's compiled routes, recompiled when the patch has changed.
class RouteCompiler
{
public:
    /// Forget the compiled routes: the next update() compiles. For a voice
    /// starting from silence.
    constexpr void reset() noexcept
    {
        hasCompiled = false;
    }

    /// At a note-on, or at one of the voice's grid points: compiles `patch`
    /// if it changed since the last compile (or nothing has been compiled
    /// since reset()). Returns true if it compiled.
    constexpr bool update(const Patch& patch) noexcept
    {
        if (hasCompiled && patch.version() == compiledVersion)
        {
            return false;
        }
        compiled = compile(patch.slots());
        compiledVersion = patch.version();
        hasCompiled = true;
        return true;
    }

    [[nodiscard]] constexpr const CompiledRoutes& routes() const noexcept
    {
        return compiled;
    }

private:
    CompiledRoutes compiled{};
    std::uint32_t compiledVersion = 0;
    bool hasCompiled = false;
};

} // namespace anoesis::dsp::modulation
