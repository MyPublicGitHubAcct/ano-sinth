// Tests for WAV I/O and golden-file comparison.

#include <anoesis/test/buffer.hpp>
#include <anoesis/test/golden.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

using namespace anoesis::test;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace
{

Buffer makeTestBuffer()
{
    Buffer buffer{2, 100};
    for (std::size_t i = 0; i < 100; ++i)
    {
        buffer.channel(0)[i] = static_cast<float>(i) * 0.01f;
        buffer.channel(1)[i] = -1.0f / static_cast<float>(i + 1);
    }
    return buffer;
}

// A fresh, empty scratch directory under the build's test output directory.
fs::path scratchDirectory(const std::string& name)
{
    const auto dir = defaultOutputDirectory() / "support-tests" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE("WAV files round-trip float samples bit for bit", "[support][golden]")
{
    const auto path = scratchDirectory("wav") / "roundtrip.wav";
    const auto original = makeTestBuffer();
    REQUIRE(writeWav(path, original, 44100.0));

    // value_or: a missing file reads as an empty 0 Hz WavFile, which fails below.
    const auto wav = readWav(path).value_or(WavFile{});
    CHECK_THAT(wav.sampleRate, Catch::Matchers::WithinAbs(44100.0, 0.0));
    CHECK(bitIdentical(wav.buffer, original));
}

TEST_CASE("readWav rejects missing and malformed files", "[support][golden]")
{
    const auto dir = scratchDirectory("bad-wav");
    CHECK_FALSE(readWav(dir / "missing.wav").has_value());
    {
        std::ofstream junk{dir / "junk.wav", std::ios::binary};
        junk << "RIFF0000WAVEnope";
    }
    CHECK_FALSE(readWav(dir / "junk.wav").has_value());
}

TEST_CASE("Golden comparison: update, match, tolerance, mismatch, missing", "[support][golden]")
{
    GoldenOptions options;
    options.goldenDirectory = scratchDirectory("golden");
    options.outputDirectory = scratchDirectory("golden-actual");
    const auto reference = makeTestBuffer();

    SECTION("a missing golden file fails and says how to create it")
    {
        options.update = false;
        const auto result = compareWithGolden("absent", reference, 48000.0, options);
        CHECK_FALSE(result.passed);
        CHECK_THAT(result.message, ContainsSubstring("ANOESIS_UPDATE_GOLDEN"));
        CHECK(fs::exists(options.outputDirectory / "absent.wav")); // the actual output, for inspection
    }

    SECTION("update mode writes the file; a later identical render matches")
    {
        options.update = true;
        REQUIRE(compareWithGolden("render", reference, 48000.0, options).passed);
        REQUIRE(fs::exists(options.goldenDirectory / "render.wav"));

        options.update = false;
        const auto again = compareWithGolden("render", reference, 48000.0, options);
        CHECK(again.passed);
        CHECK_THAT(again.maxAbsDifference, Catch::Matchers::WithinAbs(0.0, 0.0));

        SECTION("differences within the tolerance pass")
        {
            auto nudged = reference;
            nudged.channel(1)[50] += 1e-6f;
            options.tolerance = 1e-5;
            CHECK(compareWithGolden("render", nudged, 48000.0, options).passed);
        }

        SECTION("differences beyond it fail, report where, and save the actual output")
        {
            auto broken = reference;
            broken.channel(1)[50] += 0.01f;
            options.tolerance = 1e-5;
            const auto result = compareWithGolden("render", broken, 48000.0, options);
            CHECK_FALSE(result.passed);
            CHECK_THAT(result.message, ContainsSubstring("channel 1, sample 50"));
            CHECK(result.maxAbsDifference > 0.009);
            CHECK(fs::exists(options.outputDirectory / "render.wav"));
        }

        SECTION("a different shape or sample rate fails")
        {
            CHECK_FALSE(compareWithGolden("render", Buffer{1, 100}, 48000.0, options).passed);
            CHECK_FALSE(compareWithGolden("render", reference, 44100.0, options).passed);
        }
    }
}
