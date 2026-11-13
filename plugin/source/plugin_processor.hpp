#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace brutalis
{

/// The Brutalis audio processor. In Phase 0 it is an empty instrument: it
/// accepts MIDI, has a stereo output, and renders silence.
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

protected:
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

private:
    // Private in juce::AudioProcessor since JUCE 9: callers must use
    // createEditorAndMakeActive() so the processor tracks its active editor.
    juce::AudioProcessorEditor* createEditor() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

} // namespace brutalis
