// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// The registry candidates, each compiled in a translation unit of its own
// (registry_size_{table,table_dsp,functions,types}.cpp), so that their code size can
// be measured on its own (docs/dsp/modulation.md, deliverable 1):
//
//   clang++ -std=c++23 -Os -fno-exceptions -fno-rtti -Ilibs/dsp/include \
//       -c tests/dsp/modulation/registry_size_table.cpp -o /tmp/a.o
//   size -m /tmp/a.o
//
// Each probe does the two jobs the voice does: compile a patch's 16 routes
// and accumulate them, and a source's bound.

#pragma once

#include "modulation_model.hpp"
#include "registry_candidates.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace anoesis::test::modulation::size_probe
{

ModSums evaluateTable(const std::array<Route, routeCount>& patch, std::span<const float, sourceCount> values) noexcept;
ModSums evaluateFunctions(const std::array<Route, routeCount>& patch,
                          std::span<const float, sourceCount> values) noexcept;
ModSums evaluateTableDsp(const std::array<Route, routeCount>& patch,
                         std::span<const float, sourceCount> values) noexcept;
ModSums evaluateTypes(const std::array<Route, routeCount>& patch, std::span<const float, sourceCount> values) noexcept;

Interval boundTable(std::size_t source, const BoundInputs& in) noexcept;
Interval boundFunctions(std::size_t source, const BoundInputs& in) noexcept;
Interval boundTableDsp(std::size_t source, const BoundInputs& in) noexcept;
Interval boundTypes(std::size_t source, const BoundInputs& in) noexcept;

} // namespace anoesis::test::modulation::size_probe
