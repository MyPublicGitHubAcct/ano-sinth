// Smoke test for libs/interop: the JUCE module builds, links, and is callable.

#include <anoesis_interop/anoesis_interop.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("interop library identifies itself", "[interop][smoke]")
{
    CHECK(anoesis::interop::libraryName() == "anoesis_interop");
}
