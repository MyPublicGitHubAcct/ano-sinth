#include <anoesis/test/golden.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <vector>

namespace anoesis::test
{

namespace
{

// WAV is little-endian. Bytes are assembled explicitly, so the files are the
// same on every host.
void putU16(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void putU32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    putU16(out, value & 0xFFFFU);
    putU16(out, value >> 16U);
}

void putTag(std::vector<std::uint8_t>& out, std::string_view tag)
{
    out.insert(out.end(), tag.begin(), tag.end());
}

std::uint32_t getU32(const std::vector<std::uint8_t>& in, std::size_t at)
{
    return static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(in[at + 2]) << 16U) | (static_cast<std::uint32_t>(in[at + 3]) << 24U);
}

std::uint32_t getU16(const std::vector<std::uint8_t>& in, std::size_t at)
{
    return static_cast<std::uint32_t>(in[at]) | (static_cast<std::uint32_t>(in[at + 1]) << 8U);
}

bool tagIs(const std::vector<std::uint8_t>& in, std::size_t at, std::string_view tag)
{
    return in.size() >= at + 4 && std::string_view{reinterpret_cast<const char*>(in.data() + at), 4} == tag; // NOLINT
}

constexpr std::uint32_t formatIeeeFloat = 3;

} // namespace

bool writeWav(const std::filesystem::path& path, const Buffer& buffer, double sampleRate)
{
    const auto channels = static_cast<std::uint32_t>(buffer.numChannels());
    const auto frames = static_cast<std::uint32_t>(buffer.numSamples());
    const auto dataBytes = frames * channels * 4U;
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));

    std::vector<std::uint8_t> out;
    putTag(out, "RIFF");
    putU32(out, 4U + (8U + 18U) + (8U + 4U) + (8U + dataBytes));
    putTag(out, "WAVE");

    putTag(out, "fmt ");
    putU32(out, 18);
    putU16(out, formatIeeeFloat);
    putU16(out, channels);
    putU32(out, rate);
    putU32(out, rate * channels * 4U); // bytes per second
    putU16(out, channels * 4U);        // bytes per frame
    putU16(out, 32);                   // bits per sample
    putU16(out, 0);                    // no extension

    putTag(out, "fact"); // required for non-PCM formats
    putU32(out, 4);
    putU32(out, frames);

    putTag(out, "data");
    putU32(out, dataBytes);
    for (std::size_t n = 0; n < buffer.numSamples(); ++n)
    {
        for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch)
        {
            putU32(out, std::bit_cast<std::uint32_t>(buffer.channel(ch)[n]));
        }
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size())); // NOLINT
    return static_cast<bool>(file);
}

std::optional<WavFile> readWav(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> in{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (in.size() < 12 || !tagIs(in, 0, "RIFF") || !tagIs(in, 8, "WAVE"))
    {
        return std::nullopt;
    }

    std::uint32_t channels = 0;
    std::uint32_t rate = 0;
    bool floatFormat = false;
    for (std::size_t at = 12; at + 8 <= in.size();)
    {
        const auto size = getU32(in, at + 4);
        const auto body = at + 8;
        if (body + size > in.size())
        {
            return std::nullopt; // truncated chunk
        }
        if (tagIs(in, at, "fmt ") && size >= 16)
        {
            floatFormat = getU16(in, body) == formatIeeeFloat && getU16(in, body + 14) == 32;
            channels = getU16(in, body + 2);
            rate = getU32(in, body + 4);
        }
        else if (tagIs(in, at, "data"))
        {
            if (!floatFormat || channels == 0)
            {
                return std::nullopt;
            }
            const std::size_t frames = size / (channels * 4U);
            WavFile wav{.buffer = Buffer{channels, frames}, .sampleRate = static_cast<double>(rate)};
            for (std::size_t n = 0; n < frames; ++n)
            {
                for (std::size_t ch = 0; ch < channels; ++ch)
                {
                    wav.buffer.channel(ch)[n] = std::bit_cast<float>(getU32(in, body + (((n * channels) + ch) * 4U)));
                }
            }
            return wav;
        }
        at = body + size + (size & 1U); // chunks are padded to even sizes
    }
    return std::nullopt;
}

std::filesystem::path defaultGoldenDirectory()
{
    return ANOESIS_GOLDEN_DIR;
}

std::filesystem::path defaultOutputDirectory()
{
    return ANOESIS_TEST_OUTPUT_DIR;
}

bool updateRequestedByEnvironment()
{
#ifdef _MSC_VER
    // MSVC deprecates getenv (C4996, an error under /WX); _dupenv_s is its replacement.
    char* value = nullptr;
    std::size_t length = 0;
    const bool set =
        _dupenv_s(&value, &length, "ANOESIS_UPDATE_GOLDEN") == 0 && value != nullptr && std::string_view{value} == "1";
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
    return set;
#else
    const char* value = std::getenv("ANOESIS_UPDATE_GOLDEN"); // NOLINT(concurrency-mt-unsafe): read once, tests only
    return value != nullptr && std::string_view{value} == "1";
#endif
}

GoldenResult compareWithGolden(std::string_view name, const Buffer& actual, double sampleRate,
                               const GoldenOptions& options)
{
    const auto fileName = std::string{name} + ".wav";
    const auto goldenPath = options.goldenDirectory / fileName;
    const auto actualPath = options.outputDirectory / fileName;

    if (options.update)
    {
        const auto written = writeWav(goldenPath, actual, sampleRate);
        return {
            .passed = written,
            .message = (written ? "updated " : "could not write ") + goldenPath.string(),
            .maxAbsDifference = 0.0,
        };
    }

    const auto fail = [&](const std::string& why, double difference)
    {
        static_cast<void>(writeWav(actualPath, actual, sampleRate));
        return GoldenResult{
            .passed = false,
            .message = why + " Actual output written to " + actualPath.string(),
            .maxAbsDifference = difference,
        };
    };

    const auto golden = readWav(goldenPath);
    if (!golden)
    {
        return fail("No golden file at " + goldenPath.string() +
                        ". If the render is correct, create it by running the test with ANOESIS_UPDATE_GOLDEN=1, "
                        "listen to it, and commit it.",
                    0.0);
    }
    if (golden->buffer.numChannels() != actual.numChannels() || golden->buffer.numSamples() != actual.numSamples() ||
        std::lround(golden->sampleRate) != std::lround(sampleRate))
    {
        std::ostringstream why;
        why << "Shape differs from the golden file: expected " << golden->buffer.numChannels() << " channels x "
            << golden->buffer.numSamples() << " samples at " << golden->sampleRate << " Hz, got "
            << actual.numChannels() << " x " << actual.numSamples() << " at " << sampleRate << " Hz.";
        return fail(why.str(), 0.0);
    }

    double worst = 0.0;
    std::size_t worstChannel = 0;
    std::size_t worstSample = 0;
    for (std::size_t ch = 0; ch < actual.numChannels(); ++ch)
    {
        for (std::size_t n = 0; n < actual.numSamples(); ++n)
        {
            const auto expected = static_cast<double>(golden->buffer.channel(ch)[n]);
            const auto got = static_cast<double>(actual.channel(ch)[n]);
            const auto bothNan = std::isnan(expected) && std::isnan(got);
            const auto difference = bothNan ? 0.0 : std::abs(got - expected);
            if (std::isnan(difference) || difference > worst)
            {
                worst = std::isnan(difference) ? INFINITY : difference;
                worstChannel = ch;
                worstSample = n;
            }
        }
    }

    std::ostringstream report;
    if (worst <= options.tolerance)
    {
        report << "Matches " << goldenPath.string() << " (max |difference| " << worst << ").";
        return {.passed = true, .message = report.str(), .maxAbsDifference = worst};
    }
    report << "Differs from " << goldenPath.string() << " by up to " << worst << " (tolerance " << options.tolerance
           << "), worst at channel " << worstChannel << ", sample " << worstSample << ": expected "
           << golden->buffer.channel(worstChannel)[worstSample] << ", got " << actual.channel(worstChannel)[worstSample]
           << ".";
    return fail(report.str(), worst);
}

} // namespace anoesis::test
