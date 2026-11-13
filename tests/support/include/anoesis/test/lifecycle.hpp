// Test support: checks the processor lifecycle contract
// (libs/dsp/include/anoesis/dsp/core/processor.hpp).

#pragma once

#include <anoesis/test/buffer.hpp>

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/core/processor.hpp>

#include <cstddef>
#include <type_traits>

namespace anoesis::test
{

/// The outcome of checkLifecycleContract(), one flag per rule.
struct LifecycleResult
{
    bool resetMatchesFreshPrepare = false;     ///< use, reset(), then the probe == fresh + probe
    bool rePrepareMatchesFreshPrepare = false; ///< use, prepare() again, then the probe == fresh + probe

    [[nodiscard]] bool passed() const noexcept
    {
        return resetMatchesFreshPrepare && rePrepareMatchesFreshPrepare;
    }
};

/// A deterministic test signal: a different mix of tones on each channel,
/// with a full-scale impulse at the start so any state is excited.
[[nodiscard]] Buffer makeProbeSignal(std::size_t numChannels, std::size_t numSamples);

/// A different signal (a loud, slow ramp) used to dirty a processor's state.
[[nodiscard]] Buffer makeDisturbanceSignal(std::size_t numChannels, std::size_t numSamples);

/// Checks that reset() and a repeated prepare() both return a processor to
/// exactly the state of a freshly prepared one. `make` returns a new,
/// unprepared processor configured identically each time. Output is compared
/// bit for bit: reset is about state, so there is no tolerance.
template <typename Factory>
    requires dsp::BlockProcessor<std::invoke_result_t<Factory&>>
[[nodiscard]] LifecycleResult checkLifecycleContract(Factory make, const dsp::ProcessSpec& spec, std::size_t numSamples)
{
    const auto render = [&](auto& processor, Buffer buffer)
    {
        dsp::processInChunks(processor, buffer.block(), spec.maxBlockSize);
        return buffer;
    };
    const auto probe = makeProbeSignal(spec.numChannels, numSamples);
    const auto disturbance = makeDisturbanceSignal(spec.numChannels, numSamples);

    auto fresh = make();
    fresh.prepare(spec);
    const auto expected = render(fresh, probe);

    auto resetOne = make();
    resetOne.prepare(spec);
    static_cast<void>(render(resetOne, disturbance));
    resetOne.reset();
    const auto afterReset = render(resetOne, probe);

    auto rePrepared = make();
    rePrepared.prepare(spec);
    static_cast<void>(render(rePrepared, disturbance));
    rePrepared.prepare(spec);
    const auto afterRePrepare = render(rePrepared, probe);

    return {bitIdentical(expected, afterReset), bitIdentical(expected, afterRePrepare)};
}

} // namespace anoesis::test
