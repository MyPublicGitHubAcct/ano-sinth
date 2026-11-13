# Fast approximations: exp2 and tanh

A synth evaluates a few transcendental functions millions of times per second. Every pitch modulation needs 2^x to turn semitones into a frequency ratio. Every saturating filter stage needs tanh, four times per sample per voice in a 24 dB ladder. The C library versions are accurate to an ulp, but they are function calls that handle every edge case of IEEE arithmetic. This page shows how to build cheaper versions whose accuracy is *designed*, then *measured*.

## Theory

### Range reduction

Approximating 2^x directly over the whole float range is hopeless: a polynomial cannot follow a function that spans 2^-126 to 2^127. Split x into its integer and fractional parts instead:

- x = i + f, with i = ⌊x⌋ and f ∈ [0, 1)
- 2^x = 2^i · 2^f

2^i is free: a float stores a power of two in its exponent field, so 2^i is the bit pattern `(i + 127) << 23`. Only 2^f on [0, 1) needs approximating, and that is a smooth, gently curved function that a short polynomial fits very well. Computing f = x − ⌊x⌋ is exact in float, so range reduction adds no error.

### Minimax polynomials and the Remez exchange

A Taylor series is accurate at one point and gets worse away from it. For a fixed degree, the polynomial that minimises the **maximum** error over an interval is the *minimax* polynomial. By Chebyshev's equioscillation theorem, its error curve touches ±E alternately at n + 2 points. The **Remez exchange algorithm** finds it:

1. Guess n + 2 reference points.
2. Solve the linear system that forces the error to be +E, −E, +E, … at them.
3. Move the references to the new error extrema.
4. Repeat until stable.

For relative error, weight the error by 1/f(x).

### Constraints that matter more than the last bit

Two properties matter more than the size of the error:

- **Exactness at integers** (q(0) = 0): 2^n comes out exact, so octaves are exact.
- **Continuity across integers** (q(1) = 1): approaching x = n from below gives 2^(n−1) · (1 + q(1)) = 2^n, which matches the value at n. Without it there is a tiny step at every integer. That step breaks monotonicity, which a pitch glide or envelope would hear as a click.

Both constraints are built into the polynomial's form, so they hold exactly however the coefficients come out.

### tanh without cancellation

tanh(x) = (e^{2x} − 1)/(e^{2x} + 1). The common form 1 − 2/(e^{2x} + 1) **cancels catastrophically** near zero: for x = 10⁻⁴ it subtracts two numbers that agree in their first four digits, so the result has only about 3 correct digits. A saturator's low-level behaviour, where tanh(x) ≈ x, depends exactly on those digits.

The fix is to compute m = e^{2|x|} − 1 *directly*, which an exp2 split makes possible:

- m = (2^i − 1) + 2^i · q(f), with q(f) ≈ 2^f − 1;
- when i = 0, m is simply q(f), with no subtraction at all;
- then tanh|x| = m / (m + 2).

So the polynomial approximates **2^f − 1**, not 2^f, and is designed for relative error in that form. A relative bound on q also bounds 2^f, at half the error.

## How it is applied here

[libs/dsp/include/anoesis/dsp/math/fast_math.hpp](../../libs/dsp/include/anoesis/dsp/math/fast_math.hpp):

- **The polynomial.** q(f) = f·g(f) with g(f) = 1 + (f − 1)·h(f) and h cubic, so q(0) = 0 and q(1) = 1 hold by construction and q has degree 5. [tools/design/exp2_polynomial.py](../../tools/design/exp2_polynomial.py) runs the Remez exchange in pure Python (no numpy) and prints the coefficients. Its design error is **5.6e-7 relative** on q, so ≤ 2.8e-7 on 2^f. Re-run it to reproduce or change the degree.
- **`fastExp2(x)`** clamps x to [−126, 127], so the result is always a normal float: no denormals, no inf. It takes the floor with a truncating conversion and a correction, because `std::floor` is a library call on x86-64 without SSE4.1. It builds 2^i from bits and returns 2^i + 2^i·q(f).
- **`fastTanh(x)`** works on |x| and restores the sign with `copysign`, so it is exactly odd. It uses the cancellation-free m / (m + 2). It saturates to exactly ±1 at |x| ≥ 8.6. That limit has a reason: above about 8.66, m exceeds 2²⁵. The sum m + 2 then rounds to even, alternately up and down, which would make the output wobble between 1 and 0.99999994, a monotonicity failure that the tests caught during design. Below 8.6, m + 2 is exact.
- **NaN never causes undefined behaviour.** Clamps are written as `std::max(lo, x)`, which returns `lo` for NaN. A NaN is therefore never converted to an integer, which C++ leaves undefined. NaN gives 2^-126 or ±1: finite, bounded, and visible in a test.

### Measured accuracy

These come from [tests/dsp/math/fast_math_test.cpp](../../tests/dsp/math/fast_math_test.cpp), checked against double-precision `std::exp2` and `std::tanh` on Apple Clang 21 (arm64). Debug and RelWithDebInfo give identical numbers. Run `anoesis_dsp_tests "[approximation]" -s` to print them.

| Function | Domain swept | Max error (measured) | Test bound |
|---|---|---|---|
| `fastExp2` | [−126, 127], step 1/1024 | **3.08e-7 relative** (≈ 5 float ulps, or 0.0005 cents as pitch) | 5e-7 |
| `fastExp2` | integers −126 … 127 | **exact** | 0 ulp |
| `fastTanh` | [−12, 12], step 1/8192 | **1.63e-7 absolute** (−136 dB) | 2e-7 |
| `fastTanh` | the above plus 10⁻³⁰ … 0.09 geometrically | **7.6e-7 relative** | 1e-6 |

The bounds are derived from the design error plus float rounding, with the derivation written in the tests, not fitted to the measurement. The tests also check:

- `fastExp2`: monotonic on a dense grid, continuous just below each integer, and correct clamping for ±200, ±inf, and NaN;
- `fastTanh`: bitwise odd symmetry, |y| ≤ 1, monotonic over [−12, 12], and exact ±1 saturation, including for ±inf and NaN.

The speed side of the trade is measured by the M1.0 benchmark harness (PLAN.md, M1.0).

## Trade-offs and alternatives

- **Degree.** Degree 4 would save one multiply-add for about 4e-6 relative error, still only 0.007 cents. Degree 5 was chosen so the approximation sits within a few float ulps and can stand in for `exp2f` anywhere. Phase 3 can trade it down where a profile shows it matters.
- **Lookup tables** (2^f from a 256-entry table with interpolation) are common on DSP chips. They cost memory and cache traffic, and they don't vectorise as cleanly as a polynomial does with SIMD. They are not used here.
- **Bit tricks.** Schraudolph's exp writes a scaled x straight into the exponent bits: one multiply and one add, but about 4% error. It is fine for envelopes and too coarse for pitch.
- **Padé or rational tanh** (for example x(27 + x²)/(27 + 9x²)) is cheaper but only good to about 2% and not bounded by 1 for large x. A rational *minimax* tanh could match this accuracy for a similar cost. It is a Phase 3 candidate if a profile shows tanh is hot.
- **Saturating at ±1 exactly versus following tanh to the last ulp.** The 7e-8 difference is −143 dB. Exact saturation buys monotonicity and a clean, bounded output.

## References

- J.-M. Muller, *Elementary Functions: Algorithms and Implementation* (3rd ed., 2016): range reduction (ch. 11) and minimax approximation with the Remez algorithm (ch. 3).
- N. N. Schraudolph, "A Fast, Compact Approximation of the Exponential Function", *Neural Computation* 11(4), 1999.
- W. H. Press et al., *Numerical Recipes* (3rd ed.), §5.13: Chebyshev and minimax approximation.
- D. Goldberg, "What Every Computer Scientist Should Know About Floating-Point Arithmetic", *ACM Computing Surveys* 23(1), 1991: cancellation.
