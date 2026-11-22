// keytrack.hpp after math/pitch.hpp, on purpose (M1.4 task 5): until stage 2,
// keytrackOctaves()'s parameter `referenceNote` shadowed dsp::referenceNote
// (-Wshadow, an error with -Werror) in any file that included pitch.hpp
// before it. This file compiling is the check; keep the two includes in this
// order, in their own blocks so that clang-format leaves them there.

#include <anoesis/dsp/math/pitch.hpp>

#include <anoesis/dsp/filters/keytrack.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinULP;

TEST_CASE("Keytrack: keytrack.hpp builds after math/pitch.hpp, and takes pitch.hpp's reference", "[keytrack]")
{
    namespace dsp = anoesis::dsp;
    // An octave above A4, tracked 1:1 from A4: one octave.
    CHECK_THAT(dsp::keytrackOctaves(dsp::referenceNote + 12.0f, 1.0f, dsp::referenceNote), WithinULP(1.0f, 0));
    CHECK_THAT(dsp::keytrackOctaves(dsp::keytrackReferenceNote, 1.0f), WithinULP(0.0f, 0));
}
