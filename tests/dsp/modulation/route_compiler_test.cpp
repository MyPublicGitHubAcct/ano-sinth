// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Tests for when a voice compiles its routes (libs/dsp modulation/
// route_compiler.hpp; docs/dsp/modulation.md, as built): at a note-on from
// silence and at the first of its own grid points after the patch changes.
//
// A stand-in voice (GridVoice) runs the voice's 32-sample grid from its
// note-on, calls RouteCompiler::update() at its note-on and at each grid
// point, and outputs per sample its Mix A sum from sources that move with
// time (a per-sample destination, so a recompile mid-segment would show at
// once) and its Cutoff A sum taken at each grid point and held (a grid
// destination). An engine splits the host's blocks at its events (note-ons,
// a patch change), as M1.8's will. Two voices on different grids, a patch
// change mid-block and a retrigger: the same output at host blocks of 1, 37
// and 4096, each voice taking the change at its own next grid point, with no
// allocation. A voice whose grid restarts at each process() call (the host's
// blocks) instead of running from its note-on fails the same comparison.

#include "../support/allocation_counter.hpp"

#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/modulation/route_compiler.hpp>
#include <anoesis/dsp/modulation/routing.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mod = anoesis::dsp::modulation;

namespace
{

constexpr std::size_t grid = 32;
constexpr std::size_t length = 3000;

/// A stand-in for dsp::Voice's grid: the routes recompiled at a note-on from
/// silence and at each grid point, Mix A per sample from the compiled routes.
class GridVoice
{
public:
    /// gridFromCall: the control, a grid restarted at each process() call.
    explicit GridVoice(bool gridFromCall) noexcept : gridFromCall(gridFromCall) {}

    void noteOn(const mod::Patch& patch, std::uint64_t time, bool sounding) noexcept
    {
        if (sounding)
        {
            // A note-on on a sounding voice keeps its grid (M1.4 task 2): the
            // change is taken at the next grid point.
            return;
        }
        (void)time;
        routes.reset();
        routes.update(patch);
        gridPosition = 0;
        active = true;
    }

    /// Mix A per sample into `mix`, Cutoff A held from each grid point into `cutoff`.
    void process(const mod::Patch& patch, std::uint64_t time, std::span<float> mix, std::span<float> cutoff) noexcept
    {
        if (!active)
        {
            std::ranges::fill(mix, 0.0f);
            std::ranges::fill(cutoff, 0.0f);
            return;
        }
        if (gridFromCall)
        {
            gridPosition = 0;
        }
        for (std::size_t i = 0; i < mix.size(); ++i)
        {
            const auto values = sourcesAt(time + i);
            if (gridPosition == 0)
            {
                compiles += routes.update(patch) ? 1U : 0U;
                mod::Sums sums{};
                mod::accumulate(routes.routes(), values, sums);
                held = sums.at(mod::Target::cutoff, 0);
            }
            mod::Sums sums{};
            mod::accumulate(routes.routes(), values, sums);
            mix[i] = sums.at(mod::Target::mix, 0);
            cutoff[i] = held;
            gridPosition = (gridPosition + 1) % grid;
        }
    }

    /// The sources at absolute sample t: velocity, and a moving mod wheel.
    [[nodiscard]] static mod::SourceValues sourcesAt(std::uint64_t t) noexcept
    {
        mod::SourceValues values{};
        values[static_cast<std::size_t>(mod::Source::velocity)] = 0.8f;
        values[static_cast<std::size_t>(mod::Source::modWheel)] = static_cast<float>(t % 101) / 100.0f;
        return values;
    }

    std::size_t compiles = 0;

private:
    mod::RouteCompiler routes;
    std::size_t gridPosition = 0;
    float held = 0.0f;
    bool active = false;
    bool gridFromCall;
};

struct Event
{
    enum class Kind : std::uint8_t
    {
        noteOn,
        patch,
    };
    std::uint64_t time = 0;
    Kind kind = Kind::noteOn;
    std::size_t voice = 0;
};

mod::Slots firstSlots()
{
    mod::Slots s{};
    s[8] = {.source = mod::Source::velocity, .destination = mod::Destination::mixA, .amount = 5.0f};
    s[10] = {.source = mod::Source::modWheel, .destination = mod::Destination::cutoffA, .amount = 2.0f};
    return s;
}

mod::Slot changedSlot()
{
    return {.source = mod::Source::modWheel, .destination = mod::Destination::mixAB, .amount = -3.0f};
}

struct Render
{
    std::array<std::vector<float>, 2> voices{std::vector<float>(length), std::vector<float>(length)};
    std::array<std::vector<float>, 2> cutoffs{std::vector<float>(length), std::vector<float>(length)};
    std::size_t allocations = 0;
    std::array<std::size_t, 2> compiles{};
};

/// The engine: events at absolute samples, each host block split at them.
Render render(std::size_t blockSize, bool gridFromCall, const std::vector<Event>& events)
{
    Render r;
    mod::Patch patch;
    patch.set(firstSlots());
    std::array<GridVoice, 2> voices{GridVoice{gridFromCall}, GridVoice{gridFromCall}};
    std::array<bool, 2> sounding{};
    std::size_t next = 0;
    for (std::uint64_t start = 0; start < length; start += blockSize)
    {
        const std::uint64_t end = std::min<std::uint64_t>(start + blockSize, length);
        const anoesis::test::AllocationCounter counter;
        std::uint64_t t = start;
        while (t < end)
        {
            while (next < events.size() && events[next].time == t)
            {
                const auto& e = events[next];
                if (e.kind == Event::Kind::noteOn)
                {
                    voices[e.voice].noteOn(patch, t, sounding[e.voice]);
                    sounding[e.voice] = true;
                }
                else
                {
                    patch.set(9, changedSlot());
                }
                ++next;
            }
            const std::uint64_t stop = next < events.size() ? std::min(end, events[next].time) : end;
            for (std::size_t v = 0; v < 2; ++v)
            {
                const auto at = static_cast<std::size_t>(t);
                const auto n = static_cast<std::size_t>(stop - t);
                voices[v].process(patch, t, std::span{r.voices[v]}.subspan(at, n),
                                  std::span{r.cutoffs[v]}.subspan(at, n));
            }
            t = stop;
        }
        r.allocations += counter.count();
    }
    r.compiles = {voices[0].compiles, voices[1].compiles};
    return r;
}

bool identical(const std::vector<float>& a, const std::vector<float>& b)
{
    return std::ranges::equal(a, b, [](float x, float y)
                              { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}

/// Voice 0 starts at 0, voice 1 at 517 (another grid); the patch changes at
/// 1000, mid-segment for both; voice 0 is retriggered at 1500.
const std::vector<Event> sequence{
    {.time = 0, .kind = Event::Kind::noteOn, .voice = 0},
    {.time = 517, .kind = Event::Kind::noteOn, .voice = 1},
    {.time = 1000, .kind = Event::Kind::patch},
    {.time = 1500, .kind = Event::Kind::noteOn, .voice = 0},
};

} // namespace

TEST_CASE("Modulation routes: recompiled at grid points, the same at host blocks of 1, 37 and 4096", "[modulation]")
{
    const auto a = render(4096, false, sequence);
    const auto b = render(37, false, sequence);
    const auto c = render(1, false, sequence);
    for (std::size_t v = 0; v < 2; ++v)
    {
        CAPTURE(v);
        CHECK(identical(a.voices[v], b.voices[v]));
        CHECK(identical(a.voices[v], c.voices[v]));
        CHECK(identical(a.cutoffs[v], b.cutoffs[v]));
        CHECK(identical(a.cutoffs[v], c.cutoffs[v]));
    }
    CHECK(a.allocations == 0);
    CHECK(b.allocations == 0);
    CHECK(c.allocations == 0);
    // Each voice compiled at its note-on (not counted) and once at the change.
    CHECK(a.compiles == std::array<std::size_t, 2>{1, 1});
    CHECK(c.compiles == std::array<std::size_t, 2>{1, 1});
}

TEST_CASE("Modulation routes: each voice takes a patch change at its own next grid point", "[modulation]")
{
    const auto r = render(37, false, sequence);
    // Before the change: velocity 0.8 x 5 on Mix A, from each note-on.
    const auto before = [](std::size_t t)
    {
        (void)t;
        return 0.8f * 5.0f;
    };
    // After it: and the mod wheel x -3 (Mix AB includes A). The product in a
    // statement of its own, as accumulate() makes it: in one expression the
    // compiler may fuse the multiply and the add (-ffp-contract=on).
    const auto after = [](std::size_t t)
    {
        const float wheel = -3.0f * (static_cast<float>(t % 101) / 100.0f);
        return (0.8f * 5.0f) + wheel;
    };
    const auto same = [](float x, float y)
    {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    };
    // Voice 0's grid points are multiples of 32: the first at or after 1000 is 1024.
    for (std::size_t t = 0; t < length; ++t)
    {
        CAPTURE(t);
        const float want = t < 1024 ? before(t) : after(t);
        REQUIRE(same(r.voices[0][t], want));
    }
    // Voice 1's are 517 + 32k: the first after 1000 is 1029. Silent before its note-on.
    for (std::size_t t = 0; t < length; ++t)
    {
        CAPTURE(t);
        if (t < 517)
        {
            REQUIRE(same(r.voices[1][t], 0.0f));
            continue;
        }
        REQUIRE(same(r.voices[1][t], t < 1029 ? before(t) : after(t)));
    }
    // Cutoff A: the mod wheel x 2, taken at each voice's own grid points and held.
    for (std::size_t t = 0; t < length; ++t)
    {
        CAPTURE(t);
        const std::size_t point0 = t - (t % grid);
        REQUIRE(same(r.cutoffs[0][t], 2.0f * (static_cast<float>(point0 % 101) / 100.0f)));
        if (t >= 517)
        {
            const std::size_t point1 = t - ((t - 517) % grid);
            REQUIRE(same(r.cutoffs[1][t], 2.0f * (static_cast<float>(point1 % 101) / 100.0f)));
        }
    }
}

TEST_CASE("Modulation routes: a voice whose grid follows the host's blocks differs", "[modulation]")
{
    // The control: a grid restarted at each process() call takes the change
    // and the held sums where the host's blocks (and the engine's splits)
    // fall, so the comparison above would catch it.
    const auto a = render(4096, true, sequence);
    const auto b = render(37, true, sequence);
    CHECK_FALSE(identical(a.cutoffs[0], b.cutoffs[0]));
    CHECK_FALSE(identical(a.cutoffs[1], b.cutoffs[1]));
}

TEST_CASE("Modulation routes: update() compiles only when the patch changed", "[modulation]")
{
    mod::Patch patch;
    CHECK(patch.version() == 0);
    mod::RouteCompiler routes;
    // Nothing compiled yet: the first update compiles, even the init patch.
    CHECK(routes.update(patch));
    CHECK(routes.routes().empty());
    CHECK_FALSE(routes.update(patch));
    patch.set(8, {.source = mod::Source::velocity, .destination = mod::Destination::volumeB, .amount = 2.0f});
    CHECK(patch.version() == 1);
    CHECK(patch.slots()[8].destination == mod::Destination::volumeB);
    CHECK(routes.update(patch));
    CHECK(routes.routes().count == 1);
    CHECK_FALSE(routes.update(patch));
    // reset() forgets: the next update compiles again.
    routes.reset();
    CHECK(routes.update(patch));
    CHECK(routes.routes().count == 1);
    // A slot out of range is ignored and changes nothing.
    patch.set(16, {.source = mod::Source::velocity, .destination = mod::Destination::volumeA, .amount = 2.0f});
    CHECK(patch.version() == 1);
    CHECK_FALSE(routes.update(patch));
    // A whole preset at once.
    mod::Slots all{};
    all[0] = {.source = mod::Source::modEnv1, .destination = mod::Destination::cutoffA, .amount = 4.0f};
    all[1] = {.source = mod::Source::modEnv1, .destination = mod::Destination::cutoffB, .amount = 4.0f};
    patch.set(all);
    CHECK(patch.version() == 2);
    CHECK(routes.update(patch));
    CHECK(routes.routes().count == 2);
    // Any change of value recompiles, across the counter's wrap included.
    mod::RouteCompiler other;
    CHECK(other.update(patch));
    patch.set(0, all[0]);
    CHECK(other.update(patch));
}
