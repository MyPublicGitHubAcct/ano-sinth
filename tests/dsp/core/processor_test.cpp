// Tests for the processor lifecycle: ProcessSpec, the Preparable/BlockProcessor
// concepts, and splitting a block into chunks no longer than maxBlockSize.

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/processor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <vector>

using anoesis::dsp::AudioBlock;
using anoesis::dsp::ProcessSpec;

namespace
{

// The smallest type that meets the full lifecycle contract.
struct ConformingProcessor
{
    void prepare(const ProcessSpec& /*spec*/) {}
    void reset() noexcept {}
    void process(const AudioBlock& /*block*/) noexcept {}
};

// process() may throw: not allowed on the audio thread.
struct ThrowingProcess
{
    void prepare(const ProcessSpec& /*spec*/) {}
    void reset() noexcept {}
    void process(const AudioBlock& /*block*/) {}
};

// No reset(): state could never be cleared without re-preparing.
struct NoReset
{
    void prepare(const ProcessSpec& /*spec*/) {}
    void process(const AudioBlock& /*block*/) noexcept {}
};

// Records the length and start position of every chunk it is given. Fixed
// capacity, so process() cannot allocate (or throw).
struct ChunkRecorder
{
    std::array<std::size_t, 8> lengthStore{};
    std::array<const float*, 8> startStore{};
    std::size_t count = 0;

    void prepare(const ProcessSpec& /*spec*/) {}
    void reset() noexcept {}
    void process(const AudioBlock& block) noexcept
    {
        if (count < lengthStore.size())
        {
            lengthStore[count] = block.numSamples();
            startStore[count] = block.channel(0).data();
        }
        ++count;
    }

    [[nodiscard]] std::vector<std::size_t> lengths() const
    {
        return {lengthStore.begin(),
                lengthStore.begin() + static_cast<std::ptrdiff_t>(std::min(count, lengthStore.size()))};
    }
    [[nodiscard]] std::vector<const float*> starts() const
    {
        return {startStore.begin(),
                startStore.begin() + static_cast<std::ptrdiff_t>(std::min(count, startStore.size()))};
    }
};

} // namespace

TEST_CASE("ProcessSpec: only complete, finite specs are valid", "[dsp][core]")
{
    CHECK(ProcessSpec{.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 2}.isValid());

    CHECK_FALSE(ProcessSpec{}.isValid());
    CHECK_FALSE(ProcessSpec{.sampleRate = 0.0f, .maxBlockSize = 512, .numChannels = 2}.isValid());
    CHECK_FALSE(ProcessSpec{.sampleRate = -44100.0f, .maxBlockSize = 512, .numChannels = 2}.isValid());
    CHECK_FALSE(ProcessSpec{.sampleRate = std::numeric_limits<float>::infinity(), .maxBlockSize = 512, .numChannels = 2}
                    .isValid());
    CHECK_FALSE(
        ProcessSpec{.sampleRate = std::numeric_limits<float>::quiet_NaN(), .maxBlockSize = 512, .numChannels = 2}
            .isValid());
    CHECK_FALSE(ProcessSpec{.sampleRate = 48000.0f, .maxBlockSize = 0, .numChannels = 2}.isValid());
    CHECK_FALSE(ProcessSpec{.sampleRate = 48000.0f, .maxBlockSize = 512, .numChannels = 0}.isValid());
}

TEST_CASE("Lifecycle concepts accept conforming types and reject the rest", "[dsp][core]")
{
    STATIC_CHECK(anoesis::dsp::Preparable<ConformingProcessor>);
    STATIC_CHECK(anoesis::dsp::BlockProcessor<ConformingProcessor>);

    STATIC_CHECK_FALSE(anoesis::dsp::BlockProcessor<ThrowingProcess>);
    STATIC_CHECK_FALSE(anoesis::dsp::Preparable<NoReset>);
    STATIC_CHECK_FALSE(anoesis::dsp::BlockProcessor<NoReset>);
}

TEST_CASE("processInChunks splits a block into consecutive chunks of at most maxChunk", "[dsp][core]")
{
    std::array<float, 1000> samples{};
    std::array<float*, 1> pointers{samples.data()};
    const AudioBlock block{pointers.data(), 1, samples.size()};

    ChunkRecorder recorder;

    SECTION("a long block is split, the last chunk takes the remainder")
    {
        anoesis::dsp::processInChunks(recorder, block, 256);
        CHECK(recorder.lengths() == std::vector<std::size_t>{256, 256, 256, 232});
        CHECK(recorder.starts() == std::vector<const float*>{samples.data(), samples.data() + 256, samples.data() + 512,
                                                             samples.data() + 768});
    }

    SECTION("a block no longer than maxChunk is passed through in one call")
    {
        anoesis::dsp::processInChunks(recorder, block, 1000);
        CHECK(recorder.lengths() == std::vector<std::size_t>{1000});
    }

    SECTION("an empty block produces no calls")
    {
        anoesis::dsp::processInChunks(recorder, block.subBlock(0, 0), 256);
        CHECK(recorder.lengths().empty());
    }

    SECTION("maxChunk == 0 means no limit, so audio is never silently dropped")
    {
        anoesis::dsp::processInChunks(recorder, block, 0);
        CHECK(recorder.lengths() == std::vector<std::size_t>{1000});
    }
}
