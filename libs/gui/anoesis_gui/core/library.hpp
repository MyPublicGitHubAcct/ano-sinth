#pragma once

#include <string_view>

namespace anoesis::gui
{

/// Name of this library, as used for its JUCE module. Lets a smoke test prove
/// the module builds, links, and can be called.
[[nodiscard]] std::string_view libraryName() noexcept;

} // namespace anoesis::gui
