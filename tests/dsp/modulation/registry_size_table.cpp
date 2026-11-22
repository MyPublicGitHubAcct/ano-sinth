// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Registry candidate (table_registry) on its own, for its code size
// (registry_size_probe.hpp).

#include "registry_size_probe.hpp"

namespace anoesis::test::modulation::size_probe
{

ModSums evaluateTable(const std::array<Route, routeCount>& patch, std::span<const float, sourceCount> values) noexcept
{
    const auto compiled = table_registry::compile(patch);
    ModSums sums{};
    table_registry::accumulate(compiled, values, sums);
    return sums;
}

Interval boundTable(std::size_t source, const BoundInputs& in) noexcept
{
    return table_registry::bound(source, in);
}

} // namespace anoesis::test::modulation::size_probe
