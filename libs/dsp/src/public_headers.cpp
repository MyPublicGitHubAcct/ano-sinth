// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio
//
// Includes every public header, so that:
//   - each header is proven self-contained (it compiles with nothing before it);
//   - header-only code is compiled with the library's own flags, including
//     -Wdouble-promotion (cmake/AnoesisCompiler.cmake), which the tests skip.
// Add every new public header here.

#include <anoesis/dsp/control/adsr.hpp>
#include <anoesis/dsp/control/smoother.hpp>
#include <anoesis/dsp/control/stereo_gain.hpp>
#include <anoesis/dsp/core/audio_block.hpp>
#include <anoesis/dsp/core/denormals.hpp>
#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/core/processor.hpp>
#include <anoesis/dsp/generators/white_noise.hpp>
#include <anoesis/dsp/library.hpp>
#include <anoesis/dsp/math/decibels.hpp>
#include <anoesis/dsp/math/fast_math.hpp>
#include <anoesis/dsp/math/pan.hpp>
#include <anoesis/dsp/math/pitch.hpp>
#include <anoesis/dsp/math/random.hpp>
#include <anoesis/dsp/oscillators/min_blep.hpp>
#include <anoesis/dsp/oscillators/oscillator_slot.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/pitch_drift.hpp>
#include <anoesis/dsp/oscillators/unison.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>
#include <anoesis/dsp/resampling/halfband.hpp>
#include <anoesis/dsp/resampling/oversampler.hpp>
