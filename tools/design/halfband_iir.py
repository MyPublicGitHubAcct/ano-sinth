#!/usr/bin/env python3
"""Designs the polyphase IIR halfband filters behind anoesis::dsp::Oversampler.

    python3 tools/design/halfband_iir.py

A halfband filter H(z) = 1/2 [A0(z^2) + z^-1 A1(z^2)] built from two chains of
first-order allpass sections, A(z^2) = prod (a_k + z^-2) / (1 + a_k z^-2). The
coefficients come from the elliptic-filter design of Valenzuela and
Constantinides (1983): the halfband elliptic filter's poles all lie on the
imaginary axis, so each pole pair is one allpass coefficient. They are
computed here with Jacobi theta-function series. The coefficients alternate
between the two chains (sorted ascending: even indices -> A0, odd -> A1).

The script then *checks* each design by evaluating the exact polyphase
structure on a dense frequency grid: passband ripple up to the passband edge
and attenuation from the stopband edge to Nyquist. Pure Python (no numpy).

Prints C++ float literals for libs/dsp/include/anoesis/dsp/resampling/halfband_coefficients.hpp.
"""
import cmath
import math


def transition_params(transition):
    """k (selectivity) and q (nome) for a normalised transition width in (0, 1/2).

    The passband ends at (1/4 - transition/2) of the (oversampled) rate and the
    stopband starts at (1/4 + transition/2).
    """
    k = math.tan((1.0 - 2.0 * transition) * math.pi / 4.0) ** 2
    kk = (1.0 - k * k) ** 0.25
    e = 0.5 * (1.0 - kk) / (1.0 + kk)
    e4 = e ** 4
    q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)))
    return k, q


def coefficient(index, k, q, order):
    c = index + 1
    num, i, sign = 0.0, 0, 1.0
    while True:
        term = q ** (i * (i + 1)) * math.sin((2 * i + 1) * c * math.pi / order) * sign
        num += term
        if abs(term) < 1e-100:
            break
        sign, i = -sign, i + 1
    den, i, sign = 0.0, 1, -1.0
    while True:
        term = q ** (i * i) * math.cos(2 * i * c * math.pi / order) * sign
        den += term
        if abs(term) < 1e-100:
            break
        sign, i = -sign, i + 1
    ww = (num * q ** 0.25) / (den + 0.5)
    wwsq = ww * ww
    x = math.sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq)
    return (1.0 - x) / (1.0 + x)


def design(count, transition):
    k, q = transition_params(transition)
    order = 2 * count + 1
    return sorted(coefficient(i, k, q, order) for i in range(count))


def response(coefs, f):
    """H at normalised frequency f (cycles per sample of the oversampled rate)."""
    z1 = cmath.exp(-2j * math.pi * f)
    z2 = z1 * z1
    a0 = a1 = 1.0 + 0j
    for i, a in enumerate(coefs):
        section = (a + z2) / (1.0 + a * z2)
        if i % 2 == 0:
            a0 *= section
        else:
            a1 *= section
    return 0.5 * (a0 + z1 * a1)


def evaluate(coefs, pass_edge, stop_edge, points=20000):
    passband = [abs(response(coefs, pass_edge * i / points)) for i in range(points + 1)]
    stop = [abs(response(coefs, stop_edge + (0.5 - stop_edge) * i / points)) for i in range(points + 1)]
    ripple_db = 20 * math.log10(max(passband)) - 20 * math.log10(min(passband))
    rejection_db = -20 * math.log10(max(stop))
    return ripple_db, rejection_db


def dc_group_delay(coefs):
    """Group delay at DC, in samples of the oversampled rate, of the halfband filter."""
    tau = lambda a: 2.0 * (1.0 - a) / (1.0 + a)  # one section in z^2
    t0 = sum(tau(a) for a in coefs[0::2])
    t1 = sum(tau(a) for a in coefs[1::2]) + 1.0
    return 0.5 * (t0 + t1)


def report(name, coefs, pass_edge, stop_edge):
    ripple, rejection = evaluate(coefs, pass_edge, stop_edge)
    print(f"{name}: {len(coefs)} coefficients")
    print(f"  passband 0..{pass_edge:.5f}  ripple {ripple:.3e} dB")
    print(f"  stopband {stop_edge:.5f}..0.5  rejection {rejection:.1f} dB")
    print(f"  DC group delay {dc_group_delay(coefs):.4f} samples (oversampled rate)")
    for c in coefs:
        print(f"    {c:.9e}f,")


def to_float(x):
    """Round to the nearest IEEE single, as the C++ float literals will be."""
    import struct
    return struct.unpack("f", struct.pack("f", x))[0]


# The chosen designs: the fewest coefficients reaching 100 dB of rejection.
TARGET_REJECTION_DB = 100.0


def main():
    # Stage 1 (base rate -> 2x): pass 0..20 kHz at a 44.1 kHz base rate, so the
    # images of 0..20 kHz (24.1..44.1 kHz at the 2x rate) are in the stopband.
    # At higher base rates the band edges scale with the rate.
    pass1 = 20000.0 / 88200.0
    # Stage 2 (2x -> 4x): pass 0..20 kHz at 88.2 kHz; images start at 68.2 kHz.
    pass2 = 20000.0 / 176400.0

    for label, pass_edge in (("stage 1 (1x <-> 2x)", pass1), ("stage 2 (2x <-> 4x)", pass2)):
        stop_edge = 0.5 - pass_edge  # a halfband filter is symmetric about 1/4
        transition = 2.0 * (stop_edge - 0.25)
        for count in range(1, 17):
            coefs = design(count, transition)
            if evaluate(coefs, pass_edge, stop_edge, 4000)[1] >= TARGET_REJECTION_DB:
                break
        report(label, coefs, pass_edge, stop_edge)
        rounded = [to_float(c) for c in coefs]
        ripple, rejection = evaluate(rounded, pass_edge, stop_edge)
        print(f"  with float coefficients: ripple {ripple:.3e} dB, rejection {rejection:.1f} dB")
        print()


if __name__ == "__main__":
    main()
