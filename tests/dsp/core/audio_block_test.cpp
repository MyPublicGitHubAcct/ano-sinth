// Tests for AudioBlock: the non-owning multichannel view every processor works on.

#include <anoesis/dsp/core/audio_block.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstddef>

using anoesis::dsp::AudioBlock;
using Catch::Matchers::WithinULP;

namespace
{

// Two channels of 8 samples, each sample holding a distinct, recognisable value.
struct TwoChannelData
{
    std::array<float, 8> left{0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f};
    std::array<float, 8> right{10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f};
    std::array<float*, 2> pointers{left.data(), right.data()};

    [[nodiscard]] AudioBlock block() noexcept
    {
        return {pointers.data(), pointers.size(), left.size()};
    }
};

} // namespace

TEST_CASE("AudioBlock: a default block is empty", "[dsp][core]")
{
    const AudioBlock block;
    CHECK(block.numChannels() == 0);
    CHECK(block.numSamples() == 0);
    CHECK(block.empty());
}

TEST_CASE("AudioBlock: channels view the caller's memory", "[dsp][core]")
{
    TwoChannelData data;
    const auto block = data.block();

    REQUIRE(block.numChannels() == 2);
    REQUIRE(block.numSamples() == 8);
    CHECK_FALSE(block.empty());

    const auto right = block.channel(1);
    REQUIRE(right.size() == 8);
    CHECK(right.data() == data.right.data());

    // Writing through the view writes the caller's buffer: it is a view, not a copy.
    block.channel(0)[3] = -1.0f;
    CHECK_THAT(data.left[3], WithinULP(-1.0f, 0));
}

TEST_CASE("AudioBlock: a sub-block is a window into the same samples", "[dsp][core]")
{
    TwoChannelData data;
    const auto sub = data.block().subBlock(2, 4);

    REQUIRE(sub.numChannels() == 2);
    REQUIRE(sub.numSamples() == 4);
    CHECK(sub.channel(0).data() == data.left.data() + 2);
    CHECK(sub.channel(1).data() == data.right.data() + 2);

    SECTION("sub-blocks of sub-blocks accumulate their offsets")
    {
        const auto subSub = sub.subBlock(1, 2);
        REQUIRE(subSub.numSamples() == 2);
        CHECK_THAT(subSub.channel(1)[0], WithinULP(13.0f, 0));
    }
}

TEST_CASE("AudioBlock: out-of-range sub-blocks are clamped, never overrun", "[dsp][core]")
{
    TwoChannelData data;
    const auto block = data.block();

    CHECK(block.subBlock(6, 100).numSamples() == 2); // length clamped to what is left
    CHECK(block.subBlock(8, 1).numSamples() == 0);   // offset at the end: empty
    CHECK(block.subBlock(50, 1).numSamples() == 0);  // offset past the end: empty
    CHECK(block.subBlock(50, 1).numChannels() == 2); // ...but keeps its channel count
}

TEST_CASE("AudioBlock: clear() zeroes only the samples it views", "[dsp][core]")
{
    TwoChannelData data;
    data.block().subBlock(2, 3).clear();

    for (std::size_t i = 0; i < data.left.size(); ++i)
    {
        const bool inside = i >= 2 && i < 5;
        CAPTURE(i);
        CHECK_THAT(data.left[i], WithinULP(inside ? 0.0f : static_cast<float>(i), 0));
        CHECK_THAT(data.right[i], WithinULP(inside ? 0.0f : static_cast<float>(i) + 10.0f, 0));
    }
}
