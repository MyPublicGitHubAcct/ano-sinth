# The processor lifecycle: prepare, reset, process

Every DSP component in `libs/dsp` goes through the same three steps. This page explains why the lifecycle exists, how it is expressed in C++, and how the tests check that a processor honours it.

## Theory

An audio plugin runs in two worlds:

- **The audio thread** calls the plugin every few milliseconds with a block of samples. It must return before the audio device needs the next block, every time. Anything with unbounded or unpredictable duration is forbidden there:
  - heap allocation (the allocator may take a lock or ask the OS for pages);
  - locks (the thread holding the lock may be descheduled);
  - file or network I/O, logging;
  - exceptions (unwinding may allocate and is slow).

  A single miss is an audible click. This is *real-time* in the strict sense: worst-case time matters, not average time.
- **Everything else** (the host's message thread, the UI, loading presets) may allocate and block.

The lifecycle exists to separate the two worlds. Work that may allocate or take time happens once, up front, in **prepare**. The audio thread then only ever calls **process** and **reset**, which are guaranteed not to allocate, block, or throw.

The three operations, and why each exists:

| Operation | Thread | What it does | Why it is separate |
|---|---|---|---|
| `prepare(spec)` | not audio | Learns the sample rate, maximum block size, and channel count. Sizes buffers and computes rate-dependent coefficients. Leaves the processor reset. | The only step that may allocate. Hosts call it before playback and again whenever the sample rate or block size changes. |
| `reset()` | any | Clears running state (filter memories, delay lines, envelope stages, smoother ramps). Keeps the configuration. | Hosts need to silence tails without reconfiguring (transport jumps, bypass, voice reuse). It must be cheap and real-time safe. |
| `process(block)` | audio | Processes a block of samples in place. | The hot path. Its contract is what makes the plugin glitch-free. |

The same split appears in every serious audio framework: JUCE's `prepareToPlay`/`processBlock`/`releaseResources`, VST3's `setupProcessing`/`setActive`/`process`, and the AU render lifecycle. Having one shape for our own processors means the plugin can forward host calls mechanically. It also keeps every processor testable without a host.

The same rules happen to be what a microcontroller needs. A Cortex-M has no OS to fall back on, and allocating once at startup and never again is standard practice there. So the lifecycle also serves the STM32 goal.

## How it is applied here

### `ProcessSpec`

[libs/dsp/include/anoesis/dsp/core/process_spec.hpp](../../libs/dsp/include/anoesis/dsp/core/process_spec.hpp) holds the sample rate, the maximum block size, and the channel count. `isValid()` rejects zero, negative, infinite, and NaN rates, and zero sizes.

The sample rate is a `float`, not a `double`:

- every standard rate (44.1 kHz to 768 kHz) is an integer below 2²⁴, so float stores it exactly;
- `libs/dsp` must run on a Cortex-M4F, whose FPU is single precision only.

Code that needs more precision locally (for example, a phase accumulator for a very slow LFO) can still widen it.

### `AudioBlock`

[libs/dsp/include/anoesis/dsp/core/audio_block.hpp](../../libs/dsp/include/anoesis/dsp/core/audio_block.hpp) is a non-owning view: an array of channel pointers, a channel count, a sample count, and an offset. It is the one buffer type every processor takes:

- in the plugin, it wraps `juce::AudioBuffer`'s channel pointers, with no copy;
- in tests, `anoesis::test::Buffer` hands one out;
- on an embedded target, it wraps static arrays.

`subBlock(offset, length)` returns a window into the same memory. Out-of-range requests are **clamped** rather than asserted. A sub-block therefore can never reach past its parent, even in a release build where asserts are compiled out.

### The concepts

[libs/dsp/include/anoesis/dsp/core/processor.hpp](../../libs/dsp/include/anoesis/dsp/core/processor.hpp) defines two C++20 concepts:

- **`Preparable`**: `prepare(const ProcessSpec&)` and a `noexcept` `reset()`. Building blocks that are not themselves block processors satisfy this, such as parameter smoothers and noise generators.
- **`BlockProcessor`**: `Preparable`, plus a `noexcept` `process(const AudioBlock&)`.

`noexcept` is part of the requirement. A `process()` that might throw is rejected at compile time. `prepare()` is deliberately *not* required to be `noexcept`, because allocation may fail and a caller off the audio thread can handle that.

### `processInChunks`

Some hosts call `processBlock` with more samples than they announced in `prepareToPlay`. Sample-accurate automation (M1.8) will also split blocks at event times. `processInChunks(processor, block, maxChunk)` feeds a processor consecutive sub-blocks of at most `maxChunk` samples, in order, so a processor never sees more than it was prepared for. `maxChunk == 0` means no limit: audio is never silently dropped.

## Trade-offs and alternatives

- **Concepts versus a virtual base class.** Concepts cost nothing at run time and let the compiler inline small processors into the voice loop. Phase 2 needs run-time choice of filter types and effects through registries. That will be a thin virtual interface *wrapping* concept-conforming processors (M1.3 and M1.7), so the cost of virtual dispatch is paid once per block, not per sample.
- **`float` versus `double` sample rate.** Double would match JUCE's API exactly. It would also put a software-emulated double on the hot path of single-precision targets, for no gain at standard rates.
- **Clamping versus asserting in `subBlock`.** An assert documents a precondition, but it disappears in release builds. Clamping costs two `min`s and turns a would-be buffer overrun into a short block.
- **Chunk splitting inside every processor, versus one helper.** Writing the loop once means each processor can assume `numSamples <= maxBlockSize` and size its scratch buffers to match.

## Tests

- [tests/dsp/core/audio_block_test.cpp](../../tests/dsp/core/audio_block_test.cpp) checks that the view aliases the caller's memory, that sub-block offsets compose, that out-of-range sub-blocks clamp, and that `clear()` touches only its window.
- [tests/dsp/core/processor_test.cpp](../../tests/dsp/core/processor_test.cpp) checks:
  - `ProcessSpec` validation, including NaN and infinity;
  - that the concepts accept a conforming type and reject one whose `process()` may throw and one with no `reset()` (as compile-time `STATIC_CHECK`s);
  - that `processInChunks` produces the expected chunk lengths and start pointers, handles an empty block, and treats `maxChunk == 0` as no limit.
- **The contract check.** [tests/support/include/anoesis/test/lifecycle.hpp](../../tests/support/include/anoesis/test/lifecycle.hpp) has `checkLifecycleContract(make, spec, numSamples)`. It renders a probe signal through:
  1. a freshly prepared processor;
  2. one that was used, then `reset()`;
  3. one that was used, then prepared again.

  Outputs 2 and 3 must match output 1 *bit for bit*: reset is about state, so no tolerance is acceptable. Every stateful processor in later milestones runs through it. The checker has its own test ([tests/support/test/lifecycle_test.cpp](../../tests/support/test/lifecycle_test.cpp)), which uses an integrator: it passes when `reset()` clears the sum and fails when it doesn't.
- **The allocation check.** [tests/dsp/support/allocation_counter.hpp](../../tests/dsp/support/allocation_counter.hpp) has `AllocationCounter`. The DSP test executable replaces the global `operator new` (and its array and nothrow forms), counting every allocation on a thread while a counter is alive. A test wraps the real-time calls in a counter's scope and checks `count() == 0` afterwards, outside the scope, because Catch2's assertions allocate. The replacement is compiled into the executable directly rather than taken from the support library, so the linker always uses it. Its own test checks that it counts a `make_unique` and a `vector` and nothing else. First used by unison ([oscillator-unison.md](oscillator-unison.md#tests)): changing the voice count mid-note, and every control and `process()` call, allocate nothing.

## References

- Ross Bencina, ["Real-time audio programming 101: time waits for nothing"](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing) (2011). The canonical list of what not to do on the audio thread.
- Timur Doumler, "C++ in the Audio Industry" (CppCon 2015) and "Using locks in real-time audio processing, safely" (ADC 2020).
- JUCE `AudioProcessor` documentation: `prepareToPlay`, `processBlock`, `releaseResources`.
- Steinberg VST 3 SDK documentation: "Audio Processor Call Sequence".
