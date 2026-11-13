// Phase 0 behaviour of the plugin processor: an empty instrument with a stereo
// output that renders silence.

#include "brutalis_info.hpp"
#include "plugin_processor.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <memory>

namespace
{

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 256;

} // namespace

TEST_CASE("processor reports the product identity", "[plugin][smoke]")
{
    const brutalis::PluginProcessor processor;

    CHECK(processor.getName() == brutalis::info::productName);
    CHECK(processor.acceptsMidi());
    CHECK_FALSE(processor.producesMidi());
    CHECK_FALSE(processor.isMidiEffect());
}

TEST_CASE("processor is a stereo-out instrument with no audio input", "[plugin][smoke]")
{
    const brutalis::PluginProcessor processor;

    CHECK(processor.getTotalNumInputChannels() == 0);
    CHECK(processor.getTotalNumOutputChannels() == 2);
}

TEST_CASE("processor renders silence, overwriting whatever the host left in the buffer", "[plugin][smoke]")
{
    brutalis::PluginProcessor processor;
    processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
    processor.prepareToPlay(sampleRate, blockSize);

    juce::AudioBuffer<float> buffer(2, blockSize);
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        juce::FloatVectorOperations::fill(buffer.getWritePointer(channel), 0.5f, blockSize);
    }

    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        CHECK_THAT(buffer.getMagnitude(channel, 0, blockSize), Catch::Matchers::WithinULP(0.0f, 0)); // exactly zero
    }

    processor.releaseResources();
}

TEST_CASE("processor provides an editor", "[plugin][smoke]")
{
    const juce::ScopedJuceInitialiser_GUI juceInitialiser;
    brutalis::PluginProcessor processor;

    REQUIRE(processor.hasEditor());
    const std::unique_ptr<juce::AudioProcessorEditor> editor{processor.createEditorAndMakeActive()};
    REQUIRE(editor != nullptr);
    CHECK(processor.getActiveEditor() == editor.get());
}
