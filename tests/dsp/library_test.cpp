// Smoke test for libs/dsp: the library builds, links, and is callable.

#include <anoesis/dsp/library.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("dsp library identifies itself", "[dsp][smoke]")
{
    CHECK(anoesis::dsp::libraryName() == "anoesis_dsp");
}
