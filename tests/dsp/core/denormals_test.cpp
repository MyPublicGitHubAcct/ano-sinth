// Tests for denormal protection: the flush-to-zero guard and flushDenormal().
// Doc: docs/dsp/denormals.md

#include <anoesis/dsp/core/denormals.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <limits>

using anoesis::dsp::flushDenormal;
using anoesis::dsp::flushDenormalsSupported;
using anoesis::dsp::ScopedFlushDenormals;
using Catch::Matchers::WithinULP;

namespace
{

constexpr float smallestNormal = std::numeric_limits<float>::min(); // 1.18e-38
constexpr float denormalMin = std::numeric_limits<float>::denorm_min();

bool isDenormal(float x)
{
    return std::fpclassify(x) == FP_SUBNORMAL;
}

// Halving the smallest normal float gives a denormal in IEEE mode, and zero in
// flush-to-zero mode. `volatile` stops the compiler from folding it at compile
// time, where the FPU mode would not apply.
float halfOfSmallestNormal()
{
    const volatile float x = smallestNormal;
    const volatile float result = x * 0.5f;
    return result;
}

// A denormal *input* multiplied by 1. Flush-to-zero alone flushes denormal
// results; "denormals are zero" (DAZ on x86, FZ on ARM) also flushes inputs.
float denormalTimesOne()
{
    const volatile float x = denormalMin;
    const volatile float result = x * 1.0f;
    return result;
}

// A one-pole low-pass whose input stops: the classic denormal generator. The
// state decays geometrically from 1 towards 0 and passes through the
// denormal range on the way. Returns the number of denormal outputs.
template <typename Protect>
std::size_t denormalsInDecay(Protect protect)
{
    const volatile float coefficient = 0.9f; // volatile: keep the arithmetic at run time
    float state = 1.0f;
    std::size_t count = 0;
    for (int n = 0; n < 2000; ++n) // 0.9^2000 = 1e-92, far below the smallest denormal
    {
        state = protect(state * coefficient);
        count += isDenormal(state) ? 1U : 0U;
    }
    return count;
}

} // namespace

TEST_CASE("flushDenormal: zeroes denormals and leaves everything else alone", "[dsp][denormals]")
{
    CHECK_THAT(flushDenormal(denormalMin), WithinULP(0.0f, 0));
    CHECK_THAT(flushDenormal(-denormalMin), WithinULP(0.0f, 0));
    CHECK_THAT(flushDenormal(smallestNormal * 0.999f), WithinULP(0.0f, 0));

    CHECK_THAT(flushDenormal(smallestNormal), WithinULP(smallestNormal, 0));
    CHECK_THAT(flushDenormal(-smallestNormal), WithinULP(-smallestNormal, 0));
    CHECK_THAT(flushDenormal(0.5f), WithinULP(0.5f, 0));
    CHECK_THAT(flushDenormal(0.0f), WithinULP(0.0f, 0));
    CHECK(std::isinf(flushDenormal(std::numeric_limits<float>::infinity())));
    CHECK(std::isnan(flushDenormal(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("Without protection, a decaying feedback loop produces denormals", "[dsp][denormals]")
{
    // The problem being solved. In default IEEE mode the decay enters the
    // denormal range after about 830 samples, and then it gets stuck: once
    // the state is a few multiples of denorm_min, x * 0.9 rounds back to x,
    // so it never reaches zero. Every later sample is a denormal (on x86,
    // each one costs a slow microcode assist), until the note is killed.
    CHECK(denormalsInDecay([](float x) { return x; }) > 1000);
}

TEST_CASE("flushDenormal in the feedback path removes them portably", "[dsp][denormals]")
{
    CHECK(denormalsInDecay([](float x) { return flushDenormal(x); }) == 0);
}

TEST_CASE("ScopedFlushDenormals switches the FPU to flush-to-zero and restores it", "[dsp][denormals]")
{
    if (!flushDenormalsSupported)
    {
        SKIP("This target has no flush-to-zero control; flushDenormal() is the only protection.");
    }

    REQUIRE(isDenormal(halfOfSmallestNormal())); // IEEE mode before the guard
    {
        const ScopedFlushDenormals guard;
        CHECK_THAT(halfOfSmallestNormal(), WithinULP(0.0f, 0)); // denormal results flushed
        CHECK_THAT(denormalTimesOne(), WithinULP(0.0f, 0));     // denormal inputs treated as zero
        CHECK(denormalsInDecay([](float x) { return x; }) == 0);
    }
    CHECK(isDenormal(halfOfSmallestNormal())); // restored on scope exit
}

TEST_CASE("ScopedFlushDenormals nests and restores the mode it found", "[dsp][denormals]")
{
    if (!flushDenormalsSupported)
    {
        SKIP("This target has no flush-to-zero control.");
    }

    {
        const ScopedFlushDenormals outer;
        {
            const ScopedFlushDenormals inner; // already on: must restore "on", not "off"
            CHECK_THAT(halfOfSmallestNormal(), WithinULP(0.0f, 0));
        }
        CHECK_THAT(halfOfSmallestNormal(), WithinULP(0.0f, 0)); // still flushing after inner exits
    }
    CHECK(isDenormal(halfOfSmallestNormal()));
}
