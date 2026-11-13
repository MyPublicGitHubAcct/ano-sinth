// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio

#pragma once

#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/process_spec.hpp>

#include <algorithm>
#include <concepts>
#include <cstddef>

/// The lifecycle every anoesis::dsp processor shares.
///
///   prepare(spec)   Off the audio thread. Configures the processor for a
///                   sample rate, a maximum block size, and a channel count.
///                   It is the only place a processor may allocate. It leaves
///                   the processor in its reset state. It may be called again
///                   at any time (never concurrently with process()), for
///                   example when the host changes the sample rate.
///   reset()         Clears all running state (delay lines, filter memories,
///                   envelope stages, smoother ramps) without touching the
///                   configuration. noexcept, never allocates, audio-thread safe.
///   process(block)  Processes the block in place. noexcept, never allocates,
///                   never blocks. block.numSamples() <= spec.maxBlockSize and
///                   block.numChannels() <= spec.numChannels.
///
/// The contract tests check (anoesis::test::checkLifecycleContract): after
/// reset(), and after a second prepare(), a processor produces bit-identical
/// output to a freshly prepared one.
///
/// Full write-up: docs/dsp/processor-lifecycle.md
namespace anoesis::dsp
{

/// A type with the configuration half of the lifecycle: prepare() and a
/// noexcept reset(). Smoothers, generators, and other building blocks that are
/// not themselves block processors satisfy this.
template <typename T>
concept Preparable = requires(T& p, const ProcessSpec& spec) {
    { p.prepare(spec) } -> std::same_as<void>;
    { p.reset() } noexcept -> std::same_as<void>;
};

/// A Preparable that also processes an AudioBlock in place, without throwing.
template <typename T>
concept BlockProcessor = Preparable<T> && requires(T& p, const AudioBlock& block) {
    { p.process(block) } noexcept -> std::same_as<void>;
};

/// Processes `block` as consecutive sub-blocks of at most `maxChunk` samples,
/// in order. Use it when a caller's block may be longer than the
/// maxBlockSize a processor was prepared with (some hosts exceed what they
/// announced), or to split a block at event times. `maxChunk == 0` means no
/// limit, so audio is never silently dropped.
template <BlockProcessor P>
void processInChunks(P& processor, const AudioBlock& block, std::size_t maxChunk) noexcept
{
    if (maxChunk == 0)
    {
        maxChunk = block.numSamples();
    }

    for (std::size_t start = 0; start < block.numSamples(); start += maxChunk)
    {
        processor.process(block.subBlock(start, std::min(maxChunk, block.numSamples() - start)));
    }
}

} // namespace anoesis::dsp
