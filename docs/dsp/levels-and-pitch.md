# Levels and pitch: decibels, notes, and master tuning

Two conversions sit under almost every control in a synth. Levels are set in **decibels** but applied as linear gain. Pitch is played as **MIDI notes** but oscillators need **Hz**. Both are exponential maps, and both have edge cases (silence, a negative gain, a re-tuned A4) that are easy to get wrong.

## Theory

### Decibels

Hearing responds to *ratios* of sound pressure, roughly logarithmically. The decibel expresses an amplitude ratio on a log scale:

- level in dB = 20 · log₁₀(gain)
- gain = 10^(dB / 20)

The factor is 20, not 10, because power is proportional to amplitude squared and the bel is defined on power. Useful anchors: +6.02 dB doubles the amplitude, −6.02 dB halves it, ±20 dB is ×10 or ÷10, and 0 dB is unity.

Zero gain is −∞ dB, which no fader can show and no float arithmetic should produce. Every implementation chooses a **silence floor**. Below it, the level is treated as −∞ and the gain as exactly zero.

### Note numbers and equal temperament

MIDI numbers semitones from C−1 (note 0) up to G9 (note 127). A4, the A above middle C, is note 69. In 12-tone equal temperament (12-TET) every semitone is the same ratio, 2^(1/12) ≈ 1.0595, so:

- f = f_ref · 2^((n − 69) / 12)
- n = 69 + 12 · log₂(f / f_ref)

`n` may be fractional. Pitch bend, glide, detune, and pitch modulation all add fractions of a semitone. A **cent** is 1/100 of a semitone, so a ratio of 2^(c/1200). Listeners can just distinguish about 5 cents for pure tones in the midrange, which is the scale that numeric accuracy is judged against here.

### Master tuning

`f_ref` is the frequency of A4. ISO 16 sets concert pitch to 440 Hz. Orchestras commonly use 442–444 Hz, baroque ensembles 415 Hz, and some musicians prefer 432 Hz. Sylenth1 added a master tuning control in v3.050 (inventory §15). Whether its control is in Hz or in cents is still unspecified, so both forms are supported, and they reduce to the same thing: a reference frequency for A4.

## How it is applied here

[libs/dsp/include/anoesis/dsp/math/decibels.hpp](../../libs/dsp/include/anoesis/dsp/math/decibels.hpp):

- `decibelsToGain(dB, silenceDb = -100)` returns `10^(dB/20)`. At or below the floor, or for NaN, it returns exactly 0, so a fader at its bottom really is silent.
- `gainToDecibels(gain, silenceDb = -100)` returns `20·log₁₀(gain)`, never below the floor. Zero and negative gains give the floor, so the result is always finite and safe to display.

−100 dB (an amplitude of 10⁻⁵) is below the noise floor of any real playback chain. Callers that need a different floor, such as a meter that bottoms out at −60 dB, pass one.

[libs/dsp/include/anoesis/dsp/math/pitch.hpp](../../libs/dsp/include/anoesis/dsp/math/pitch.hpp):

- `noteToFrequency(note, referenceHz = 440)` and `frequencyToNote(hz, referenceHz = 440)` implement the 12-TET formulas. `referenceHz` *is* the master tune.
- `centsToRatio(cents)` and `referenceFromCents(cents)` convert a cents-based master tune (for example −31.77 cents for A4 = 432 Hz) to a reference frequency.

The division by 12 is a true division rather than multiplication by a rounded 1/12. That keeps octaves exact: note 81 is exactly 880 Hz, not 880.00006.

These functions call `std::pow`, `std::exp2`, and `std::log2`. They suit per-note and per-block use. For per-sample pitch modulation, `fastExp2` ([fast approximations](fast-math.md)) is the cheaper alternative.

## Trade-offs and alternatives

- **Floor at −100 dB versus returning −∞.** −∞ is mathematically honest, but it poisons any later arithmetic (−∞ · 0 = NaN) and can't be drawn. JUCE's `Decibels` makes the same choice, also at −100 dB by default.
- **Reference in Hz versus a cents offset.** Taking Hz as the primitive keeps the formula to one multiply. A cents control is one `exp2` away and is computed once when the setting changes, not per note.
- **Other temperaments** (just intonation, Scala tuning files, MTS-ESP) would replace the 12-TET formula with a lookup per note. They are out of scope for Phase 1. `noteToFrequency` is the single place they would plug in.

## Tests

- [tests/dsp/math/decibels_test.cpp](../../tests/dsp/math/decibels_test.cpp): exact unity at 0 dB; ×10 at +20 dB; half amplitude at −6.0206 dB; the floor behaviour on both sides; a sweep from −99 dB to +40 dB against `10^(dB/20)` computed in double; round trips. The 1e-6 relative tolerance is derived in the test: the rounding of dB/20 in float, amplified by ln 10, plus one ulp from `pow`.
- [tests/dsp/math/pitch_test.cpp](../../tests/dsp/math/pitch_test.cpp):
  - A4 = 440 Hz, and octaves (220, 880, 13.75 Hz) are bit-exact;
  - every quarter tone from note 0 to 127 matches the double-precision formula to 1e-6 relative, which is 0.0017 cents (the derivation is in the test);
  - the master tune (432, 415, and 444 Hz) moves A4 and scales every note by the same ratio;
  - the cents form gives 432 Hz at −31.766 cents;
  - `frequencyToNote` round-trips to within 0.01 cents.

## References

- ISO 16:1975, *Acoustics: Standard tuning frequency*.
- MIDI Association, *MIDI 1.0 Detailed Specification*: note numbering.
- Brian C. J. Moore, *An Introduction to the Psychology of Hearing* (6th ed., 2012), ch. 6: frequency discrimination.
