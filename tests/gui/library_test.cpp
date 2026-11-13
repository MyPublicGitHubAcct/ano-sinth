// Smoke test for libs/gui: the JUCE module builds, links, and is callable.

#include <anoesis_gui/anoesis_gui.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("gui library identifies itself", "[gui][smoke]")
{
    CHECK(anoesis::gui::libraryName() == "anoesis_gui");
}
