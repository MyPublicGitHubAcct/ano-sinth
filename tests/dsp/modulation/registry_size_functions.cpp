// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

// Registry candidate (function_registry) on its own, for its code size
// (registry_size_probe.hpp).

#include "registry_size_probe.hpp"

namespace anoesis::test::modulation::size_probe
{

ModSums evaluateFunctions(const std::array<Route, routeCount>& patch,
                          std::span<const float, sourceCount> values) noexcept
{
    const auto compiled = function_registry::compile(patch);
    ModSums sums{};
    function_registry::accumulate(compiled, values, sums);
    return sums;
}

Interval boundFunctions(std::size_t source, const BoundInputs& in) noexcept
{
    return function_registry::bound(source, in);
}

} // namespace anoesis::test::modulation::size_probe
