#include "plugin_processor.hpp"

#include "brutalis_info.hpp"

#include <anoesis/dsp/library.hpp>
#include <anoesis_gui/anoesis_gui.hpp>
#include <anoesis_interop/anoesis_interop.hpp>

namespace brutalis
{

PluginProcessor::PluginProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

void PluginProcessor::prepareToPlay(double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) {}

void PluginProcessor::releaseResources() {}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.inputBuses.isEmpty() && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/)
{
    const juce::ScopedNoDenormals noDenormals;

    // Hosts may pass a buffer holding garbage, so an instrument must write
    // every output sample. No synthesis yet, so that means silence.
    buffer.clear();
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    // JUCE's API transfers ownership of a raw pointer to the caller.
    return new juce::GenericAudioProcessorEditor(*this); // NOLINT(cppcoreguidelines-owning-memory)
}

bool PluginProcessor::hasEditor() const
{
    return true;
}

const juce::String PluginProcessor::getName() const
{
    return info::productName;
}

bool PluginProcessor::acceptsMidi() const
{
    return true;
}

bool PluginProcessor::producesMidi() const
{
    return false;
}

bool PluginProcessor::isMidiEffect() const
{
    return false;
}

double PluginProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int PluginProcessor::getNumPrograms()
{
    return 1; // Hosts misbehave if this is 0. Programs arrive with presets (M1.10).
}

int PluginProcessor::getCurrentProgram()
{
    return 0;
}

void PluginProcessor::setCurrentProgram(int /*index*/) {}

const juce::String PluginProcessor::getProgramName(int /*index*/)
{
    return {};
}

void PluginProcessor::changeProgramName(int /*index*/, const juce::String& /*newName*/) {}

void PluginProcessor::getStateInformation(juce::MemoryBlock& /*destData*/) {}

void PluginProcessor::setStateInformation(const void* /*data*/, int /*sizeInBytes*/) {}

} // namespace brutalis

// JUCE's plugin wrappers call this to create the processor.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    // JUCE's API transfers ownership of a raw pointer to the caller.
    return new brutalis::PluginProcessor(); // NOLINT(cppcoreguidelines-owning-memory)
}
