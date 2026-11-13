// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Anoesis Audio
//
// Platform code for ScopedFlushDenormals. Each branch reads and writes the
// register that controls flush-to-zero. It is the only CPU-specific code in
// libs/dsp, and every target without a branch falls back to a no-op.
//
// The ARM branches need inline assembly: there is no portable C++ (or common
// intrinsic) for FPCR/FPSCR. Each use is marked NOLINT(portability-no-assembler).

#include <anoesis/dsp/core/denormals.hpp>

#include <cstdint>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#endif

namespace anoesis::dsp
{

namespace
{

using detail::FpuControl;

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)

static_assert(detail::fpuControl == FpuControl::x86Mxcsr);

// FTZ (bit 15): flush denormal results to zero. DAZ (bit 6): read denormal
// inputs as zero. Every x86-64 CPU supports both.
constexpr std::uint64_t flushBits = 0x8040U;

std::uint64_t readControl() noexcept
{
    return _mm_getcsr();
}

void writeControl(std::uint64_t value) noexcept
{
    _mm_setcsr(static_cast<unsigned int>(value));
}

#elif defined(__aarch64__) && defined(__GNUC__)

static_assert(detail::fpuControl == FpuControl::armFpcr64);

// FPCR.FZ (bit 24) flushes both denormal inputs and outputs to zero.
constexpr std::uint64_t flushBits = std::uint64_t{1} << 24U;

std::uint64_t readControl() noexcept
{
    std::uint64_t value = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value)); // NOLINT(portability-no-assembler)
    return value;
}

void writeControl(std::uint64_t value) noexcept
{
    __asm__ __volatile__("msr fpcr, %0" : : "r"(value) : "memory"); // NOLINT(portability-no-assembler)
}

#elif defined(__arm__) && defined(__ARM_FP) && defined(__GNUC__)

static_assert(detail::fpuControl == FpuControl::armFpscr32);

// FPSCR.FZ (bit 24), on ARMv7 VFP and Cortex-M4F/M7 FPUs.
constexpr std::uint64_t flushBits = std::uint64_t{1} << 24U;

std::uint64_t readControl() noexcept
{
    std::uint32_t value = 0;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(value)); // NOLINT(portability-no-assembler)
    return value;
}

void writeControl(std::uint64_t value) noexcept
{
    const auto bits = static_cast<std::uint32_t>(value);
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(bits) : "memory"); // NOLINT(portability-no-assembler)
}

#else

static_assert(detail::fpuControl == FpuControl::none);

constexpr std::uint64_t flushBits = 0;

std::uint64_t readControl() noexcept
{
    return 0;
}

void writeControl(std::uint64_t /*value*/) noexcept {}

#endif

} // namespace

ScopedFlushDenormals::ScopedFlushDenormals() noexcept : savedState(readControl())
{
    writeControl(savedState | flushBits);
}

ScopedFlushDenormals::~ScopedFlushDenormals() noexcept
{
    writeControl(savedState);
}

} // namespace anoesis::dsp
