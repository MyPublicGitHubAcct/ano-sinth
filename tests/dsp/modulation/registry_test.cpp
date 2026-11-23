// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Tests for the modulation registries as built (libs/dsp modulation/
// registry.hpp; docs/dsp/modulation.md, as built): the library's rows are
// the stage-1 prototype's DSP fields, field for field and bit for bit
// (registry_candidates.hpp, dspDestinations; modulation_model.hpp,
// sourceRows); the enums are the prototype's, in the demo's menu order;
// every row's checks run at compile time, and a broken row fails them; the
// ranges are the laws' own.

#include "registry_candidates.hpp"

#include <anoesis/dsp/filters/filter_control.hpp>
#include <anoesis/dsp/modulation/registry.hpp>
#include <anoesis/dsp/voice/mix_law.hpp>

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

namespace mod = anoesis::dsp::modulation;
namespace proto = anoesis::test::modulation;

namespace
{

bool sameBits(float a, float b)
{
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// --- Compile-time checks --------------------------------------------------------------------

// The counts: "None" and 27 destinations, "None" and 11 sources, 13 targets.
static_assert(mod::destinationCount == 28);
static_assert(mod::sourceCount == 12);
static_assert(mod::targetCount == 14);
static_assert(mod::destinationCount == proto::destinationCount);
static_assert(mod::sourceCount == proto::sourceCount);
static_assert(mod::targetCount == proto::targetCount);

// The rows are placed by their enum value, not by their position in the list.
static_assert(mod::destinationRow(mod::Destination::cutoffAB).target == mod::Target::controlCutoff);
static_assert(mod::destinationRow(mod::Destination::resonanceAB).target == mod::Target::controlResonance);
static_assert(mod::destinationRow(mod::Destination::volumeAB).parts == mod::Parts::ab);
static_assert(mod::destinationRow(mod::Destination::lfo2Gain).target == mod::Target::lfoGain);
static_assert(mod::destinationRow(mod::Destination::lfo2Gain).parts == mod::Parts::b);
static_assert(mod::sourceRow(mod::Source::lfo2).bound == mod::BoundKind::lfo);
static_assert(mod::sourceRow(mod::Source::stepVelocity).scope == mod::Scope::global);

// Out of range is "None".
static_assert(mod::destinationRow(static_cast<mod::Destination>(200)).target == mod::Target::none);
static_assert(mod::sourceRow(static_cast<mod::Source>(200)).bound == mod::BoundKind::none);

// The ranges are the laws' own (F24's clamp).
static_assert(mod::destinationRow(mod::Destination::mixA).range.max >= anoesis::dsp::mixLaw.knobMax &&
              mod::destinationRow(mod::Destination::mixA).range.max <= anoesis::dsp::mixLaw.knobMax);
static_assert(mod::destinationRow(mod::Destination::cutoffA).range.max >= anoesis::dsp::filterControlLaw.knobMax &&
              mod::destinationRow(mod::Destination::cutoffA).range.max <= anoesis::dsp::filterControlLaw.knobMax);
static_assert(mod::destinationRow(mod::Destination::pitchAB).range.isUnbounded());
static_assert(mod::destinationRow(mod::Destination::phaseB).range.wraps);

// A list with a value twice, or one missing, fails the placement check.
inline constexpr std::array twice{
    mod::SourceEntry{.id = mod::Source::none},
    mod::SourceEntry{.id = mod::Source::none},
};
static_assert(!mod::detail::eachIdOnce<mod::Source>(twice, 2));
inline constexpr std::array reordered{
    mod::SourceEntry{.id = mod::Source::velocity},
    mod::SourceEntry{.id = mod::Source::none},
};
static_assert(mod::detail::eachIdOnce<mod::Source>(reordered, 2));
static_assert(!mod::detail::eachIdOnce<mod::Source>(std::array{mod::SourceEntry{.id = mod::Source::none}}, 2));

// Broken rows fail the row checks.
constexpr mod::DestinationRow goodRow = mod::destinationRow(mod::Destination::volumeA);
static_assert(mod::detail::destinationRowValid(mod::Destination::volumeA, goodRow));
static_assert(!mod::detail::destinationRowValid(mod::Destination::volumeA,
                                                []
                                                {
                                                    auto r = goodRow;
                                                    r.parts = mod::Parts::none; // a voice destination on no part
                                                    return r;
                                                }()));
static_assert(!mod::detail::destinationRowValid(mod::Destination::volumeA,
                                                []
                                                {
                                                    auto r = goodRow;
                                                    r.scope = mod::Scope::global; // global, per sample, on part A
                                                    return r;
                                                }()));
static_assert(!mod::detail::destinationRowValid(mod::Destination::volumeA,
                                                []
                                                {
                                                    auto r = goodRow;
                                                    r.unitsPerAmount = 0.0f;
                                                    return r;
                                                }()));
static_assert(!mod::detail::destinationRowValid(mod::Destination::volumeA,
                                                []
                                                {
                                                    auto r = goodRow;
                                                    r.range = {.min = 10.0f, .max = 0.0f};
                                                    return r;
                                                }()));
static_assert(!mod::detail::destinationRowValid(
    mod::Destination::cutoffAB,
    []
    {
        auto r = mod::destinationRow(mod::Destination::cutoffAB);
        r.target = mod::Target::cutoff; // AB on the filters' own Cutoff: not the Filter Control's term
        return r;
    }()));
static_assert(!mod::detail::destinationRowValid(mod::Destination::none, goodRow));
static_assert(!mod::detail::destinationRowValid(mod::Destination::volumeA, mod::DestinationRow{}));
static_assert(mod::detail::sourceRowValid(mod::Source::velocity, mod::sourceRow(mod::Source::velocity)));
static_assert(!mod::detail::sourceRowValid(
    mod::Source::velocity,
    {.scope = mod::Scope::voice, .min = 1.0f, .max = 0.0f, .bound = mod::BoundKind::linearEnds}));
static_assert(!mod::detail::sourceRowValid(mod::Source::none, {.min = 0.0f, .max = 1.0f}));
static_assert(!mod::detail::sourceRowValid(mod::Source::velocity, {.min = 0.0f, .max = 1.0f}));

} // namespace

TEST_CASE("Modulation registry: every destination row is the prototype's DSP fields, bit for bit", "[modulation]")
{
    for (std::size_t i = 0; i < mod::destinationCount; ++i)
    {
        CAPTURE(i);
        const auto& lib = mod::destinationRow(static_cast<mod::Destination>(i));
        const auto& ref = proto::table_registry::dspDestinations[i];
        CHECK(static_cast<std::size_t>(lib.target) == static_cast<std::size_t>(ref.target));
        CHECK(static_cast<unsigned>(lib.parts) == static_cast<unsigned>(ref.parts));
        CHECK(static_cast<unsigned>(lib.scope) == static_cast<unsigned>(ref.scope));
        CHECK(static_cast<unsigned>(lib.rate) == static_cast<unsigned>(ref.rate));
        CHECK(sameBits(lib.unitsPerAmount, ref.unitsPerAmount));
        CHECK(sameBits(lib.range.min, ref.range.min));
        CHECK(sameBits(lib.range.max, ref.range.max));
        CHECK(lib.range.wraps == ref.range.wraps);
    }
}

TEST_CASE("Modulation registry: the enums are the prototype's, in the demo's menu order", "[modulation]")
{
    using D = mod::Destination;
    using PD = proto::table_registry::Destination;
    // Named, so a reordering on either side shows here.
    CHECK(static_cast<int>(D::volumeA) == static_cast<int>(PD::volumeA));
    CHECK(static_cast<int>(D::pitchAB) == static_cast<int>(PD::pitchAB));
    CHECK(static_cast<int>(D::cutoffAB) == static_cast<int>(PD::cutoffAB));
    CHECK(static_cast<int>(D::phaserFrequency) == static_cast<int>(PD::phaserFrequency));
    CHECK(static_cast<int>(D::mixAB) == static_cast<int>(PD::mixAB));
    CHECK(static_cast<int>(D::lfo2Gain) == static_cast<int>(PD::lfo2Gain));
    CHECK(static_cast<int>(D::distortionAmount) == static_cast<int>(PD::distortionAmount));
    for (std::size_t i = 1; i < mod::destinationCount; ++i)
    {
        // The menu's order: the prototype's rows, whose keys are the menu's entries.
        CAPTURE(i, proto::destinationRows[i].key);
        CHECK(static_cast<std::size_t>(mod::destinations[i].target) ==
              static_cast<std::size_t>(proto::destinationRows[i].target));
    }
    using S = mod::Source;
    using PS = proto::table_registry::Source;
    CHECK(static_cast<int>(S::velocity) == static_cast<int>(PS::velocity));
    CHECK(static_cast<int>(S::keytrack) == static_cast<int>(PS::keytrack));
    CHECK(static_cast<int>(S::modEnv2) == static_cast<int>(PS::modEnv2));
    CHECK(static_cast<int>(S::lfo1) == static_cast<int>(PS::lfo1));
    CHECK(static_cast<int>(S::stepVelocity) == static_cast<int>(PS::stepVelocity));
}

TEST_CASE("Modulation registry: every source row is the prototype's, bit for bit", "[modulation]")
{
    for (std::size_t i = 0; i < mod::sourceCount; ++i)
    {
        CAPTURE(i, proto::sourceRows[i].key);
        const auto& lib = mod::sourceRow(static_cast<mod::Source>(i));
        const auto& ref = proto::sourceRows[i];
        CHECK(static_cast<unsigned>(lib.scope) == static_cast<unsigned>(ref.scope));
        CHECK(sameBits(lib.min, ref.min));
        CHECK(sameBits(lib.max, ref.max));
        CHECK(static_cast<unsigned>(lib.bound) == static_cast<unsigned>(ref.bound));
    }
    // The LFO's range as a source: half of gain x (wave + offset), a slew
    // 0.25 past +-1 (M6).
    CHECK(sameBits(mod::lfoSourceLimit, 1.125f));
    CHECK(sameBits(mod::lfoSourceLimit, proto::lfoSourceLimit));
    CHECK(sameBits(mod::lfoSourceScale, 0.5f));
}

TEST_CASE("Modulation registry: rows that share a target agree, and AB is both parts but for the filters",
          "[modulation]")
{
    // Volume, Pitch, Phase, Pan and Mix: A, B and AB on one target.
    for (const auto t :
         {mod::Target::volume, mod::Target::pitch, mod::Target::phase, mod::Target::pan, mod::Target::mix})
    {
        CAPTURE(static_cast<int>(t));
        std::size_t rows = 0;
        unsigned parts = 0;
        for (const auto& d : mod::destinations)
        {
            if (d.target == t)
            {
                ++rows;
                parts |= 1U << static_cast<unsigned>(d.parts);
            }
        }
        CHECK(rows == 3);
        CHECK(parts == ((1U << 1U) | (1U << 2U) | (1U << 3U)));
    }
    // Cutoff and Reso: A and B on the filters' own, AB the Filter Control's.
    CHECK(mod::destinationRow(mod::Destination::cutoffA).target == mod::Target::cutoff);
    CHECK(mod::destinationRow(mod::Destination::cutoffB).target == mod::Target::cutoff);
    CHECK(mod::destinationRow(mod::Destination::resonanceA).target == mod::Target::resonance);
    CHECK(mod::destinationRow(mod::Destination::resonanceB).target == mod::Target::resonance);
    CHECK(mod::destinationRow(mod::Destination::cutoffAB).parts == mod::Parts::ab);
    CHECK(mod::destinationRow(mod::Destination::resonanceAB).parts == mod::Parts::ab);
    // The global destinations: on part 0, never per sample.
    for (const auto d : {mod::Destination::phaserFrequency, mod::Destination::distortionAmount})
    {
        CHECK(mod::destinationRow(d).scope == mod::Scope::global);
        CHECK(mod::destinationRow(d).parts == mod::Parts::a);
        CHECK(mod::destinationRow(d).rate != mod::EvalRate::perSample);
    }
}

TEST_CASE("Modulation registry: includes() reads the parts' bits", "[modulation]")
{
    CHECK(mod::includes(mod::Parts::a, 0));
    CHECK_FALSE(mod::includes(mod::Parts::a, 1));
    CHECK_FALSE(mod::includes(mod::Parts::b, 0));
    CHECK(mod::includes(mod::Parts::b, 1));
    CHECK(mod::includes(mod::Parts::ab, 0));
    CHECK(mod::includes(mod::Parts::ab, 1));
    CHECK_FALSE(mod::includes(mod::Parts::ab, 2));
    CHECK_FALSE(mod::includes(mod::Parts::none, 0));
}
