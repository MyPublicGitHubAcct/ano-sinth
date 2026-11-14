// Test-only reference: PolyBLEP / PolyBLAMP, the D6 runner-up family.
//
// Kept (not shipped) because it is the closed-form, table-free baseline the
// minBLEP oscillator is measured against: the [d6-report] test and the
// [oscillators] benchmarks print both side by side, so any later change to
// minBLEP (taps, vectorisation, the 8 waveforms) can be judged against the
// alternative D6 rejected, with the same harness. Its residuals are also an
// independent analytic check of the BLEP/BLAMP formulation.

#pragma once

#include <anoesis/dsp/core/process_spec.hpp>
#include <anoesis/dsp/oscillators/phase.hpp>
#include <anoesis/dsp/oscillators/waveform.hpp>

#include <cstdint>

/// PolyBLEP / PolyBLAMP.
///
/// A naive waveform is corrected near each jump by adding a short polynomial
/// "residual": the difference between a band-limited step and the naive one.
/// The band-limited step is the integral of a kernel h (an approximation of
/// sinc), sampled where it falls. For a bend instead of a jump (triangle), the
/// residual is integrated once more (BLAMP). No tables, no state beyond the
/// phase.
///
///   Points = 2: h is the linear B-spline (triangle) on [-1, 1]: the classic
///               PolyBLEP of Valimaki et al. Corrects one sample either side.
///   Points = 4: h is the cubic B-spline on [-2, 2]: two samples either side,
///               steeper alias rejection, more high-frequency droop.
///
/// Doc: docs/dsp/band-limited-oscillators.md
namespace anoesis::test::reference
{

using dsp::halfCycle;
using dsp::incrementFor;
using dsp::naivePulse;
using dsp::naiveSaw;
using dsp::naiveTriangle;
using dsp::ProcessSpec;
using dsp::toCycles;
using dsp::Waveform;
using dsp::widthToPhase;

namespace polyblep
{

/// |residual| of a unit step, `d` samples from the edge (d >= 0). The signed
/// residual is +stepResidual(d) before the edge and -stepResidual(d) after it.
template <int Points>
[[nodiscard]] float stepResidual(float d) noexcept
{
    static_assert(Points == 2 || Points == 4);
    if constexpr (Points == 2)
    {
        if (d >= 1.0f)
        {
            return 0.0f;
        }
        const float e = 1.0f - d;
        return 0.5f * e * e;
    }
    else
    {
        if (d >= 2.0f)
        {
            return 0.0f;
        }
        if (d >= 1.0f)
        {
            const float e = 2.0f - d;
            const float e2 = e * e;
            return e2 * e2 * (1.0f / 24.0f);
        }
        // 1/2 - 2d/3 + d^3/3 - d^4/8
        return 0.5f + (d * ((-2.0f / 3.0f) + (d * d * ((1.0f / 3.0f) - (0.125f * d)))));
    }
}

/// Residual of a unit bend (a ramp's corner), `d` samples from it; the same
/// on both sides. The integral of the step residual.
template <int Points>
[[nodiscard]] float rampResidual(float d) noexcept
{
    static_assert(Points == 2 || Points == 4);
    if constexpr (Points == 2)
    {
        if (d >= 1.0f)
        {
            return 0.0f;
        }
        const float e = 1.0f - d;
        return e * e * e * (1.0f / 6.0f);
    }
    else
    {
        if (d >= 2.0f)
        {
            return 0.0f;
        }
        if (d >= 1.0f)
        {
            const float e = 2.0f - d;
            const float e2 = e * e;
            return e2 * e2 * e * (1.0f / 120.0f);
        }
        // 7/30 - d/2 + d^2/3 - d^4/12 + d^5/40
        const float d2 = d * d;
        return (7.0f / 30.0f) + (d * (-0.5f + (d * (1.0f / 3.0f)))) + (d2 * d2 * ((-1.0f / 12.0f) + (d * 0.025f)));
    }
}

} // namespace polyblep

template <int Points>
class PolyBlepOscillator
{
public:
    static_assert(Points == 2 || Points == 4);

    void prepare(const ProcessSpec& spec) noexcept
    {
        sampleRate = spec.sampleRate;
        reset();
    }

    /// Restarts at phase 0. Sample 0 is the waveform just after its edge at
    /// t = 0, as if it had been running forever.
    void reset() noexcept
    {
        phase = 0;
        started = false;
    }

    void setWaveform(Waveform newWaveform) noexcept
    {
        waveform = newWaveform;
    }

    void setPulseWidth(float width) noexcept
    {
        widthPhase = widthToPhase(width);
    }

    void setFrequency(float hz) noexcept
    {
        setIncrement(incrementFor(hz, sampleRate));
    }

    /// Cycles per sample in units of 2^-32, at most halfCycle (Nyquist).
    void setIncrement(std::uint32_t newIncrement) noexcept
    {
        increment = newIncrement < halfCycle ? newIncrement : halfCycle;
        const float dt = toCycles(increment);
        samplesPerCycle = increment > 0 ? 1.0f / dt : 0.0f;
        const auto reach = static_cast<std::uint64_t>(increment) * (Points / 2);
        nearAhead = reach > 0xFFFFFFFFU ? 0xFFFFFFFFU : static_cast<std::uint32_t>(reach);
    }

    /// PolyBLEP is zero-phase: no delay.
    [[nodiscard]] float latencySamples() const noexcept
    {
        return 0.0f;
    }

    [[nodiscard]] float next() noexcept
    {
        float y = 0.0f;
        switch (waveform)
        {
        case Waveform::saw:
            y = naiveSaw(toCycles(phase)) + jump(0, -2.0f);
            break;
        case Waveform::pulse:
            y = naivePulse(phase, widthPhase) + jump(0, 2.0f) + jump(widthPhase, -2.0f);
            break;
        case Waveform::triangle:
        {
            const float slopeChange = 8.0f * toCycles(increment);
            y = naiveTriangle(toCycles(phase)) + bend(0, slopeChange) + bend(halfCycle, -slopeChange);
            break;
        }
        }
        phase += increment;
        arrivedSamplesPerCycle = samplesPerCycle;
        nearBehind = nearAhead;
        started = true;
        return y;
    }

private:
    // An edge at `edgePhase` matters while it is under `Points / 2` samples
    // behind (measured with the increment that brought us here) or ahead
    // (with the current one). Both tests are integer compares, so samples
    // far from every edge cost no float work. Before the first sample the
    // oscillator is treated as having run at the current pitch forever;
    // after standing still (increment 0) there is no recent edge.
    template <typename Residual>
    [[nodiscard]] float near(std::uint32_t edgePhase, float sign, Residual residual) const noexcept
    {
        float r = 0.0f;
        const std::uint32_t behind = phase - edgePhase;
        const std::uint32_t reach = started ? nearBehind : nearAhead;
        if (behind < reach)
        {
            const float spc = started ? arrivedSamplesPerCycle : samplesPerCycle;
            r += sign * residual(toCycles(behind) * spc);
        }
        const std::uint32_t ahead = edgePhase - phase;
        if (ahead != 0 && ahead < nearAhead)
        {
            r += residual(toCycles(ahead) * samplesPerCycle);
        }
        return r;
    }

    // The correction for a jump of `height` at phase `edgePhase`: the step
    // residual is odd (+ before the edge, - after).
    [[nodiscard]] float jump(std::uint32_t edgePhase, float height) const noexcept
    {
        return height * near(edgePhase, -1.0f, [](float d) { return polyblep::stepResidual<Points>(d); });
    }

    // The correction for a change of slope (per sample) at `edgePhase`: the
    // ramp residual is even.
    [[nodiscard]] float bend(std::uint32_t edgePhase, float slopeChange) const noexcept
    {
        return slopeChange * near(edgePhase, 1.0f, [](float d) { return polyblep::rampResidual<Points>(d); });
    }

    float sampleRate = 48000.0f;
    std::uint32_t phase = 0;
    std::uint32_t increment = 0;
    std::uint32_t widthPhase = halfCycle;
    float samplesPerCycle = 0.0f;
    float arrivedSamplesPerCycle = 0.0f;
    std::uint32_t nearAhead = 0;  // Points / 2 samples' worth of phase, at the current increment
    std::uint32_t nearBehind = 0; // ... at the increment that brought us here
    bool started = false;
    Waveform waveform = Waveform::saw;
};

} // namespace anoesis::test::reference
