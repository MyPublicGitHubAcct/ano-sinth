// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <string_view>

namespace anoesis::dsp
{

/// Name of this library, as used for its CMake target. Lets a smoke test prove
/// the library builds, links, and can be called.
[[nodiscard]] std::string_view libraryName() noexcept;

} // namespace anoesis::dsp
