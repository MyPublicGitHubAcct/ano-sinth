# Oscillator range: from 0.01 Hz to beyond Nyquist

Sylenth1's oscillators are specified as "stable from below 0.01 Hz up to Nyquist" (inventory [§2](../reference/sylenth1-features.md#2-oscillators-pp-5-1415)). This page covers what "stable" has to mean at each end of that range, and what an oscillator should do when its pitch goes *past* Nyquist. This happens easily: a high note, a positive octave, and the widest unison detune push the top voices above fs/2.

Read [band-limited-oscillators.md](band-limited-oscillators.md), [oscillator-waveforms.md](oscillator-waveforms.md), and [oscillator-unison.md](oscillator-unison.md) first.

*Status: the behaviour above Nyquist has been measured and decided (this page). Building it, and the tests across the whole range, is the next commit.*

## Above Nyquist

### Theory: what a band-limited waveform is up there

A periodic waveform with fundamental f₀ is a sum of harmonics at k·f₀ (k = 1, 2, …) plus its mean, the DC term. Band-limiting it to fs/2 keeps the harmonics below fs/2 and drops the rest. Once f₀ itself is above fs/2, every harmonic is dropped, and **only the mean is left**. The ideal is silence for the waveforms with no DC (saw, square, triangle, sine, TriSaw), and a constant for those with one: the 25% HPulse at −0.5 and the 12.5% QPulse at −0.75.

An oscillator can't simply run on above fs/2, because a phase step of more than half a cycle per sample is indistinguishable from a step *backwards* of less than half a cycle. A fundamental at 0.6 fs would come out as one at 0.4 fs. That is aliasing of the fundamental itself. Something has to be done instead.

### What the clamp did

Until now, `incrementFor` and `MinBlepOscillator::setIncrement` clamped any pitch above fs/2 to an increment of exactly half a cycle. At that increment the phase alternates between two values, so the output is periodic in 2 samples: exactly **dc + a·(−1)ⁿ**, a stationary tone at Nyquist. Its level a depends on which two phases the oscillator lands on, which depends on where it was when the pitch crossed fs/2.

Measured by the `[range-report]` ([oscillator_range_report.cpp](../../tests/dsp/oscillators/oscillator_range_report.cpp)) for every waveform at a Nyquist increment. The voice was started at 256 phases, and separately arrived at by a per-sample sweep from 0.45 fs to 0.55 fs from 256 random phases. Measured after 128 samples, over 4096. In cycles per sample, every rate is the same.

| Waveform | Band-limited ideal | DC | Nyquist tone: min / median / max over start phases | Max, arriving by a sweep | Peak |
|---|---|---:|---:|---:|---:|
| Saw | silence | 0 | −52.7 / −12.9 / −9.9 dBFS | −9.9 dBFS | 0.68 |
| Square | silence | 0 | −46.7 / −6.9 / **−3.9** | −3.9 | 1.37 |
| HPulse | −0.5 | −0.500 | −49.7 / −9.9 / −6.9 | −6.9 | 1.48 |
| QPulse | −0.75 | −0.750 | −55.1 / −15.2 / −12.3 | −12.3 | 1.28 |
| Triangle | silence | 0 | −50.6 / −10.8 / −7.8 | −7.8 | 0.87 |
| Sine | silence | 0 | −47.3 / −3.0 / **0.0** | 0.0 | 2.00 |
| TriSaw | silence | 0 | −59.6 / −14.8 / −11.8 | −11.8 | 0.55 |

So a clamped voice had the right DC, but on top of it a tone at fs/2 at up to full scale, where the ideal has nothing. Its level was decided by chance. The sine is the worst case, because the minBLEP kernel doesn't filter it: it is evaluated directly, at a delayed phase.

### Level as the fundamental approaches Nyquist

Below fs/2, the minBLEP output is the waveform filtered by the kernel, whose transition band is centred on Nyquist. So every straight-segment waveform already rolls off over the last few percent before it. RMS about the mean, relative to the ideal fundamental (above fs/4 the band-limited waveform is its fundamental alone), 48,000 samples:

| Waveform | 0.30 fs | 0.40 | 0.45 | 0.47 | 0.48 | 0.49 | 0.495 | 0.499 fs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Saw, square, HPulse, QPulse, triangle, TriSaw | 0.0 dB | 0.0 | 0.0 | −0.2 | −0.8 | −2.6 | −4.1 | −5.6 |
| Sine | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 |

The kernel is flat to 0.45 fs. The sine never rolls off, so its full level ran straight into the clamp.

### The options

Each option was prototyped as a gain on one minBLEP voice's output, as a function of its target pitch, and measured two ways:

- **Above Nyquist:** the RMS about the waveform's mean once the target has been above fs/2 for 10 ms. The ideal is 0.
- **10 Hz–18 kHz:** the power in that band about the mean, relative to a full-scale sine. The tone itself never falls in this band, so it is what the option's transitions put into the audible range.

Each was run on two pitch paths at 48 kHz, worst of 16 start phases, for the saw, the HPulse (DC −0.5), and the sine:

1. a sweep, linear in pitch, from 0.40 fs to 0.60 fs and back, about 1 s each way;
2. a 13 Hz vibrato of ±1 semitone around 0.485 fs (0.458 to 0.514 fs), crossing Nyquist 26 times a second.

| Option | Sweep: above Nyquist (saw / HPulse / sine) | Sweep: 10 Hz–18 kHz | Vibrato: 10 Hz–18 kHz |
|---|---|---|---|
| Clamp | −10.0 / −7.0 / −0.2 dB | −95.4 / −92.4 / −118.3 dB | −66.8 / −91.7 / −110.4 dB |
| Mute above Nyquist, at once | 0 / −6.0 / 0 (linear) | −75.8 / **−43.9** / −66.1 | −46.1 / **−9.6** / −36.4 |
| Mute above Nyquist, 10 ms ramp | 0 / −6.0 / 0 | −95.4 / **−45.2** / −115.6 | −67.8 / **−10.6** / −88.2 |
| Fade the output, 0.45 fs → 0.5 fs | 0 / −6.0 / 0 | −96.7 / **−62.0** / −119.4 | −73.7 / **−11.5** / −125.3 |
| **Fade about the mean, 0.45 fs → 0.5 fs** | **0 / 0 / 0** | **−96.7 / −93.7 / −119.4** | **−73.7 / −95.8 / −125.3** |

"0" in the first column means silence about the mean, exactly the ideal. The HPulse's −6.0 dB under the mutes and the plain fade is its DC of −0.5 being removed, which the ideal keeps. The fade is a raised cosine of the pitch, gain = ½(1 + cos(π·(c − 0.45)/0.05)) for c = f/fs in the band.

- **Clamp.** Click-free, because nothing switches. But it leaves the stationary Nyquist tone shown above.
- **Mute at once.** Clicks on every crossing.
- **Mute with a ramp, and fading the whole output.** Clean for the DC-free waveforms. But both take the pulses' DC with the AC. Each crossing then steps the DC (−45 dB), and a vibrato across the band wobbles it at the vibrato rate (−10 to −12 dB in band, clearly audible).
- **Fade about the mean.** Output = mean + g·(y − mean). The DC never moves, so the pulses are as clean as the rest, and above Nyquist the output *is* the band-limited ideal. Because it is a pure function of the pitch, it holds no state to depend on the block size.

The saw's −67 to −74 dB under the vibrato is the oscillator's own aliasing under deep per-sample modulation near fs/2, not the option's. It is the same for every option, give or take the fade's attenuation of the voice.

### Downstream: filters and distortion

The filter (M1.3) and the effects (M1.7) don't exist yet, so this is measured with a stand-in and reasoned about from their planned designs.

**A driven saturator at 4× oversampling.** A 220 Hz saw at full scale plus one voice of the same waveform targeted at 1.1·fs/2, summed and driven through tanh(drive·x) inside the library's `Oversampler` at 4×, 48 kHz. The figure is the power below 20 kHz that is not a harmonic of 220 Hz, relative to the 220 Hz fundamental: intermodulation between the note and the upper voice.

| Upper voice | Drive 1 | Drive 4 | Drive 16 |
|---|---:|---:|---:|
| Muted (the note alone: every mute or fade option) | −81.7 dB | −84.1 dB | −48.2 dB |
| Saw, clamped (a loud start phase) | −49.3 | −39.1 | −33.2 |
| Saw, clamped (a quiet start phase) | −65.4 | −61.3 | −48.2 |
| HPulse, clamped | −47.3 | −37.8 | −32.0 |
| Sine, clamped | −41.5 | −31.3 | −25.4 |

A Nyquist tone on its own only folds back onto DC and Nyquist through a static nonlinearity. Mixed with a note, though, it produces sum and difference tones at fs/2 ± k·f, which land across the audible band, here up to 53 dB above what the note alone produces.

**Filters.** A TPT (zero-delay-feedback) lowpass discretised by the bilinear transform has a zero at exactly z = −1, so it nulls an exact fs/2 tone. The planned BP, HP, and Bypass modes pass it, though. The filter's drive stages sit before and inside the ladder, so they intermodulate it exactly like the saturator above. Each clamped voice also uses up to 1.0 of headroom, times up to 8 unison voices.

Under any of the mute or fade options, the filter and the effects see only what an ideal band-limited oscillator would give them.

### The decision

**Fade about the mean, from 0.45 fs to Nyquist, with a raised cosine; above Nyquist a voice plays its waveform's mean alone.** Chosen by the project owner on 2026-11-14 from these measurements (PLAN.md, Decisions log).

- The fade band starts at 0.45 fs: 19.8 kHz at 44.1 kHz, 21.6 kHz at 48 kHz. That is where the kernel's own roll-off begins, so the fade only completes a roll-off the straight-segment waveforms already have. For the sine, it adds that roll-off.
- At 96 and 192 kHz the band is far above hearing.

## References

- J. O. Smith III, *Spectral Audio Signal Processing*, online book, W3K Publishing, 2011: sampling, aliasing, and the Fourier series of periodic waveforms.
- V. Zavalishin, *The Art of VA Filter Design*, rev. 2.1.2, Native Instruments, 2020, ch. 3: the bilinear transform and its zero at Nyquist for TPT lowpasses.
- E. Brandt, "Hard Sync Without Aliasing", ICMC 2001: minBLEP.
