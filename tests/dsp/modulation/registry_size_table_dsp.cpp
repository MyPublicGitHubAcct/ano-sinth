// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Registry candidate (a) with the DSP's fields only (no keys or labels), on
// its own, for its code size (registry_size_probe.hpp).

#include "registry_size_probe.hpp"

namespace anoesis::test::modulation::size_probe
{

ModSums evaluateTableDsp(const std::array<Route, routeCount>& patch,
                         std::span<const float, sourceCount> values) noexcept
{
    const auto compiled = table_registry::compileDsp(patch);
    ModSums sums{};
    table_registry::accumulate(compiled, values, sums);
    return sums;
}

Interval boundTableDsp(std::size_t source, const BoundInputs& in) noexcept
{
    return table_registry::boundDsp(source, in);
}

} // namespace anoesis::test::modulation::size_probe
