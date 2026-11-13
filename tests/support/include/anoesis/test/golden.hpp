// Test support: WAV I/O and golden-file comparison for offline renders.
// How to use and update golden files: docs/dsp/measuring-dsp.md#golden-files

#pragma once

#include <anoesis/test/buffer.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace anoesis::test
{

struct WavFile
{
    Buffer buffer{0, 0};
    double sampleRate = 0.0;
};

/// Writes a 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT), little-endian on every
/// platform. Creates parent directories. Returns false on I/O failure.
[[nodiscard]] bool writeWav(const std::filesystem::path& path, const Buffer& buffer, double sampleRate);

/// Reads a 32-bit float WAV written by writeWav (or any tool writing that
/// format). Returns nothing if the file is missing or not 32-bit float.
[[nodiscard]] std::optional<WavFile> readWav(const std::filesystem::path& path);

/// tests/golden/ in the source tree, where golden files are committed.
[[nodiscard]] std::filesystem::path defaultGoldenDirectory();

/// <build>/test-output/, where failed renders are written for inspection.
[[nodiscard]] std::filesystem::path defaultOutputDirectory();

/// True if the environment variable ANOESIS_UPDATE_GOLDEN is set to 1.
[[nodiscard]] bool updateRequestedByEnvironment();

struct GoldenOptions
{
    /// Largest allowed |actual - golden| per sample. 0 demands bit-identical
    /// output, which is right for integer-exact processes (noise). Float DSP
    /// may differ in the last bits across compilers (FMA contraction), so use
    /// a small tolerance such as 1e-6 (-120 dB) there.
    double tolerance = 0.0;
    std::filesystem::path goldenDirectory = defaultGoldenDirectory();
    std::filesystem::path outputDirectory = defaultOutputDirectory();
    bool update = updateRequestedByEnvironment();
};

struct GoldenResult
{
    bool passed = false;
    std::string message;
    double maxAbsDifference = 0.0;
};

/// Compares `actual` with <goldenDirectory>/<name>.wav. In update mode, writes
/// it instead and passes. On any failure, writes `actual` to
/// <outputDirectory>/<name>.wav so the two can be compared by ear or in an
/// audio editor, and says why in `message`.
[[nodiscard]] GoldenResult compareWithGolden(std::string_view name, const Buffer& actual, double sampleRate,
                                             const GoldenOptions& options = {});

} // namespace anoesis::test
