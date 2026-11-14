# Sylenth1 feature inventory (Part 1 target)

Source: *Sylenth1 User Manual*, version 2.5 (© 2015 LennarDigital). Page numbers are the manual's printed page numbers. Everything here is written in our own words and describes **behavior only**.

Where the manual leaves out a detail (exact ranges, curve shapes, and list contents), it is marked **Unspecified**. Settle those by listening to and measuring the real plugin, or by watching the [overview video](https://www.youtube.com/watch?v=y3wMVOdIZ1c). Record each decision in the matching component doc.

Features added after the manual (v3.x) are listed in [§15](#15-additions-after-manual-v25-from-the-v3x-changelog). They come from the official changelog.

---

## 1. Architecture and signal flow (pp. 8–9)

- Two **parts**, A and B. Each part has 2 oscillators, 1 filter, and 1 amplitude envelope.
- The two oscillators in a part are summed and sent to a filter. Using **input select**, either filter can take part A, part B, both, or nothing.
- Filter → amp envelope → **mixer** (Mix A, Mix B) → **master effects** → main volume → output.
- The **Filter Control** panel controls filters A and B together. It is not a third filter.
- Everything up to the mixer is per voice (polyphonic). The master effects are global.
- Parts that are not in use are switched off automatically to save CPU (p. 6).
- **Part select** chooses which part the UI shows. **Solo** plays only the selected part.

## 2. Oscillators (pp. 5, 14–15)

Four oscillators: A1, A2, B1, B2.

| Control | Behavior |
|---|---|
| Voices | 0–8 unison voices. 0 turns the oscillator off. See [Unison](#unison). |
| Waveform | 8 waveforms "emulating common analog wave shapes" (p. 14). **The manual does not name them.** Third-party sources do: see [Waveforms](#waveforms) below. |
| Octave / Note | Coarse tuning. Centered by default (0). Octave −3 to +3, Note −7 to +7 semitones, in whole steps (see [Tuning, volume, pan, and invert](#tuning-volume-pan-and-invert)). |
| Fine | ±1 semitone, continuous; default 0. |
| Volume | Oscillator output level. Knob 0–10; taper unspecified. |
| Phase | 0–360° start phase. Only has an effect when Retrigger is on. See [Phase and Retrig](#phase-and-retrig). |
| Detune | Spreads the pitch of the unison voices around the center pitch, as in a supersaw. Law: see [Unison](#unison). |
| Stereo | Spreads unison voices across the stereo field: 0 is mono, 10 is full width. Needs more than 1 voice. Law: see [Unison](#unison). |
| Pan | Oscillator pan, 0 (left) to 10 (right), centred by default (pp. 10, 15). Law unspecified. |
| Retrig | Restarts every voice at Phase on each note-on. Off: free-running (see [Phase and Retrig](#phase-and-retrig)). |
| Inv | Inverts the output, for PWM-style tricks. |
| C / P | Copy and paste oscillator settings, including between presets. |

Requirements: alias-free; stable from below 0.01 Hz up to Nyquist; up to 32 voices per note (4 oscillators × 8 unison voices) and 512 voices in total.

**Range** ([oscillator-range.md](../dsp/oscillator-range.md)). The oscillator takes any pitch as a fractional note, so 0.01 Hz (note −115.79 at A4 = 440 Hz) and below arrive through modulation, bend, and master tune. What happens above Nyquist is our decision, not a measured behaviour: no source says what Sylenth1 plays there.

| # | Question | Working hypothesis | Measurement that settles it |
|---|---|---|---|
| O17 | A voice pushed to or above Nyquist (a high note, Octave +3, the widest Detune): what does it play? Does its level fall as its pitch approaches Nyquist, and does a pulse keep its DC? | It fades about the waveform's mean from 0.45 fs, and above Nyquist plays the mean alone: silence for the DC-free waveforms, −0.5 and −0.75 for the HPulse and QPulse (decided 2026-11-14, from measured options) | At 44.1 kHz, a Sine with Octave +3, swept with the pitch wheel from 18 kHz to past Nyquist and back: the level against pitch, and anything left at 22.05 kHz (a fixed tone there, at a level that depends on the phase, means a clamp). Repeat with the HPulse and a DC-coupled recording for its DC. Also listen for clicks as the pitch crosses Nyquist. |

### Tuning, volume, pan, and invert

The manual gives only outlines (pp. 10, 14–15). The rest comes from third-party sources, listed here with what each one says. Values marked *hypothesis* are what M1.1 builds ([oscillator-controls.md](../dsp/oscillator-controls.md)). In the DSP, every control takes physical units (semitones, a linear gain, a pan position in [−1, 1]). The knob ranges and laws below belong to the parameter table (M1.9), so a measurement that disagrees changes a mapping, not DSP code.

| Control | Value or law | Source | Confidence |
|---|---|---|---|
| Octave | Whole octaves, −3 to +3. Default 0. | [MusicRadar, parts and oscillators](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-parts-and-oscillators-630710): Octave "jumps the tuning up or down by 1, 2 or 3 octaves". The page body does not load in a fetcher, so this is read from search-engine excerpts of it. Manual p. 14: defaults to 0. [Electronomist tutorial](https://electronomist.com/tutorials/sylenth1-tutorial-pt1/): drag "until it displays +1", a signed whole number. | Medium |
| Note | Whole semitones, −7 to +7. Default 0. | MusicRadar (as above): "up to 7 semitones of shift in either direction, in semitone steps". A [Lynda course](https://www.lynda.com/Sylenth-tutorials/Understanding-oscillator-controls/585214/625369-4.html) summary: "minus seven to plus seven". The manual's default CC table (pp. 31–32) gives Volume, Detune, and Pan a CC but not Octave, Note, or Fine, which fits stepped controls (weak). | Medium |
| Fine | Continuous, ±1 semitone. *Hypothesis:* linear in cents, reaching exactly ±100 cents; the LCD shows semitones (0.09 = 9 cents). | Manual p. 14: "between two half notes", and double-click resets it to 0. MusicRadar: "smoothly slides up or down by up to a semitone". Tutorials quote values such as 0.09, −0.09, and 0.15. | High for the range; medium that it reaches ±1.00 exactly; low–medium for linearity (Detune is measured as x², below, so a curved Fine is not ruled out). |
| Volume | Knob 0–10 (the LCD shows other knobs as 0.00–10.00, e.g. "LFO 2 Gain 10.00" in the manual's p. 14 screenshot). *Hypothesis:* linear gain = knob / 10, so 10 is unity. Default: no source. | No source describes the taper. The only Sylenth1-specific hint is the v1.20 changelog line "Knob behavior changed to linear" ([KVR, 2007](https://www.kvraudio.com/news/lennardigital_releases_sylenth1_v1_20_6809)), which does not say whether it means mouse dragging or the gain curve. | Low |
| Pan | Knob 0 (left) to 10 (right), centred by default. *Hypothesis:* equal-power sin/cos, −3 dB per side at the centre, unity at the hard sides, the far side fully muted; position = knob / 5 − 1. | Manual pp. 10 and 15 for the range and the centre default. No source gives the law. | Low |
| Inv | Exact negation of the output. | Manual p. 15: two saws, one inverted, sum to a pulse whose width is set by the phase of one. The developer (KVR thread 301405, 2010): the sum has no DC. Added in v1.20 as "Oscillator waveform inversion option for PWM effects" (KVR, 2007). | High |

The unison laws (Detune, Stereo, how Pan combines with Stereo, and the level) have their own section: [Unison](#unison).

**Open questions, settled by measuring the demo (M1.13).** Common setup as for W1–W9, with a Sine at A4 unless noted.

| # | Question | Working hypothesis | Measurement that settles it |
|---|---|---|---|
| O1 | Octave range and step. | −3 to +3, whole octaves | Read the LCD at both ends. Render A4 at ±3: 55 Hz and 3520 Hz. |
| O2 | Note range and step. | −7 to +7, whole semitones | Read the LCD at both ends; render A4 at ±7 and measure the frequency. |
| O3 | Fine's law and end points. | Linear: display × 100 = cents, reaching ±1.00 | Render a long A4 at displayed values −1.00, −0.50, −0.09, 0, 0.09, 0.50, 1.00 and measure each frequency (zero-crossing fit or a long FFT with peak interpolation). Fit cents against the display: a slope of 100 and no curvature. Also note the display's step size. |
| O4 | Volume taper. | Linear gain | RMS of a sine, in dB, at knob 0, 1, 2.5, 5, 7.5, 10. At knob 5: linear gives −6.02 dB, quadratic −12.04 dB, and a dB taper gives a straight line in dB. |
| O5 | Volume's default and absolute level. | Unknown; 10 = unity in our DSP | Read the Clear (init) patch's oscillator volumes. Measure the peak of a low saw at Volume 10 with Mix and Main Volume at their init values. The absolute level chain is settled with the mixer (M1.4). |
| O6 | Pan law. | sin/cos, −3 dB at the centre | Left and right RMS of a sine at Pan 0, 2.5, 5, 7.5, 10 (1 voice, Stereo 0). The centre against the near side at hard pan gives 0, −3, −4.5, or −6 dB. Pan 2.5 separates laws that share −3 dB at the centre: sin/cos gives 0.924 / 0.383, the square-root law 0.866 / 0.500, linear 0.75 / 0.25. Check whether hard pan mutes the far side completely. |
| O7 | Inv. | Exact negation | With Retrig on and Phase 0, a saw with Inv on against Inv off, sample by sample. Then two saws, one inverted, at Phase 90°: a 25% pulse with zero mean. |
| O8 | How Pan combines with Stereo (unison). | An **offset**, clipped at the sides (decided 2026-11-14; see [Unison](#unison)) | 2 voices, Stereo 10, Detune small enough to separate the two voices in a long FFT, a sine. Read each voice's left and right level at Pan 0, 2.5, 5, 7.5, 10. At Pan 7.5 (+0.5), the lower voice reads 0.924 / 0.383 for the offset, 0.5 / 0 for a balance on the pair, and 0.707 / 0.707 for a squeeze into the remaining width. |
| O9 | Master tune (v3): units, range, and whether a keytracked filter follows it. | A4 reference in Hz (decided 2026-11-13); the filter follows it | Read the control's LCD at both ends. Self-oscillate the filter at Keytrack 10 and check that it stays in tune with the oscillator as master tune changes. |

### Phase and Retrig

The manual gives only the outline: Phase is a 0–360° start phase that only has an effect when Retrig is on, and Retrig restarts every voice at Phase on each note-on (pp. 14–15). M1.1 builds the rest as below ([oscillator-phase.md](../dsp/oscillator-phase.md)).

| Behaviour | As built | Source | Confidence |
|---|---|---|---|
| Phase 0 | The same point for every waveform: the start of its cycle as defined in W6 (saw at −1 rising, pulse high, triangle at −1, sine at 0 rising). Phase is an offset from it, in the direction of travel (90° = a quarter cycle in). | W6 hypothesis | Low until measured |
| Retrig on, from silence | The voice starts at Phase. | Manual | High |
| Retrig on, while sounding (a stolen voice, a mono retrigger) | A band-limited jump to Phase at the note-on's sample, keeping what is in flight, rather than a cut to silence. | No source says how Sylenth1 handles it; a click-free jump is our choice. | Unknown (O11) |
| Retrig off | A sounding oscillator runs on untouched; Phase is ignored. *Hypothesis:* a voice starting from silence starts at a random phase (seeded, so renders are reproducible). | Manual (Phase only matters with Retrig on). A user on the [Sonic Academy forum](https://forums.sonicacademy.com/t/phase-settings-on-oscilators/26166): with Retrig off "it starts at random start point"; MusicRadar's tutorial (search-engine excerpt): otherwise the waveform cycles "freely regardless of note input". | Low–medium (O10) |
| Retrig with unison | Every unison voice restarts at Phase (as built, 2026-11-14). With Retrig off, each unison voice from silence draws its own seeded random phase, in voice order. | Manual ("every voice"). Two users (KVR 452081, 2015): with Retrig on and Detune 0, more voices only make it louder, which fits identical, in-phase voices. | Medium: "voice" may mean the polyphonic voice. O15's in-phase test (+6.02 dB per doubling) confirms it. |

**Open questions, settled by measuring the demo (M1.13).** Common setup as for W1–W9.

| # | Question | Working hypothesis | Measurement that settles it |
|---|---|---|---|
| O10 | Retrig off: where does a note from silence start? Random per note, or continuing a phase the voice kept running? And are unison voices' start phases independent? | A seeded random phase per start, independent per voice | Play the same note 8 times with gaps, Retrig off, a low saw (A1). Read each note's start phase from its first cycle. Uniformly scattered: random; advancing by gap × frequency: a running phase. Repeat with 2 unison voices, Detune 0, Stereo 0: equal phases sum to 2× the level, independent ones to about √2×. With 8 voices, histogram the voices' start phases (separate them with a small Detune): BlitBit (KVR 518035, 2019) guesses Sylenth1 constrains the randomness, which would show as phases more evenly spread than independent uniform draws. |
| O11 | Retrig on, a note-on while the voice sounds (mono, or a stolen voice): does the phase jump, and is the jump band-limited (no click, no aliasing) or a hard reset? | A band-limited jump | Mono mode, Retrig on, a high saw, a second note-on at the same pitch mid-note. Look at the waveform around the note-on, and at the spectrum of a fast repeated retrigger for aliases between the harmonics. |
| O12 | Phase direction and units: is 90° a quarter cycle *into* the waveform? | Yes | With W6's setup, Phase 90 and 180 on a saw: the first sample after onset is −0.5 or 0 for "into"; +0.5 or 0 for "before". |

### Unison

The manual gives only the controls (pp. 14–15): Voices 0–8 (0 turns the oscillator off), Detune spreads the voices' pitch, Stereo spreads them across the stereo field and "needs more than 1 voice", and Retrig restarts "every voice". The rest comes from third-party sources. Values marked *hypothesis* are what M1.1 builds ([oscillator-unison.md](../dsp/oscillator-unison.md)), decided 2026-11-14. The DSP takes the outermost voice's offset in semitones and a width in [0, 1]; the knob laws belong to the parameter table (M1.9).

| Topic | Value or law | Source | Confidence |
|---|---|---|---|
| Detune law | Linear in semitones; *hypothesis:* evenly spaced, voice k of N at s·(2k/(N − 1) − 1), the centre voice of an odd count in tune. | BlitBit, [KVR thread 518035](https://www.kvraudio.com/forum/viewtopic.php?t=518035&start=15) (10 Jan 2019): "linear with regards to half tones", with a 5-voice example at −2, −1, 0, +1, +2. A contrary opinion (chk071, same thread) offers no measurement. | Low–medium (one poster; graphs not viewable) |
| Detune knob | s = 3.3219 · x² semitones for knob x ∈ [0, 1] (about 0.8 at half the knob); *hypothesis:* times a voice-count factor g(N), 1 at 2 voices to 0.66 at 8, linear in N between. At Detune 10: 3.32, 3.14, 2.95, 2.76, 2.57, 2.39, 2.20 semitones for 2 to 8 voices. | Same post for the curve. BlitBit (12 Jan 2019): "the maximum detune is dependent on the number of voices"; with 8 voices at Detune 10, the highest voice is at about 125 Hz from 110 Hz (2.2 semitones), and with 2 voices "far away" from it. g(8) = 2.2/3.32. | Low (read from graphs; which count the 3.32 belongs to is not stated) |
| Stereo order | At Stereo 10: C, LR, LCR, LRLR, LRCLR, LRRLLR, LRLCRLR, LRRLRLLR for 1 to 8 voices. *Hypothesis:* each order lists the voices from the lowest pitch to the highest, so each pair at ±d is split left and right (every order is antisymmetric, and the first voice is always L). | BlitBit, 12 Jan 2019 | Low–medium for the orders; low for the reading by pitch |
| Stereo width | *Hypothesis:* position = Pan + width × side, width = knob / 10. | No source. | Low |
| Pan law per voice | *Hypothesis:* equal power (sin/cos), as for Pan (O6). | No source. | Low |
| Pan with Stereo | *Hypothesis:* an offset, clipped at the sides (O8). | No source. | Low |
| Level | Every voice at the same amplitude; *hypothesis:* no normalisation for the count. | BlitBit, 12 Jan 2019 ("the same amplitude"). secretfound and phreaque, [KVR thread 452081](https://www.kvraudio.com/forum/viewtopic.php?p=6319341) (Dec 2015): with Retrig on and Detune 0, "weakest with 1 voice, loudest with 8", which rules out 1/N but not 1/√N. | Medium for equal amplitude; low for no normalisation |
| Noise | *Hypothesis:* one noise source per oscillator, mixed to the power and left/right correlation of N noise voices at the voices' gains. | No source. | Low |

**Open questions, settled by measuring the demo (M1.13).** Common setup as for W1–W9.

| # | Question | Working hypothesis | Measurement that settles it |
|---|---|---|---|
| O13 | Detune: the spacing of the inner voices, the knob curve, and the maximum for each voice count. | Even spacing; 3.3219 · x² · g(N) semitones, g(2) = 1 to g(8) = 0.66 | Sines at A2 (110 Hz), Stereo 0, Retrig off. For N = 2…8 at Detune 10, a long FFT (2¹⁸ points, interpolated peaks): each voice's offset in semitones. Even spacing gives equal gaps; the outermost offset per N gives g(N). Then N = 2 at Detune 1, 2.5, 5, 7.5, 10 for the curve (x²: 0.033, 0.21, 0.83, 1.87, 3.32 st). |
| O14 | Stereo: which voice goes where, how positions scale with the knob, and the pan law per voice. | The reported orders, by pitch; position = width × side; equal power | As O13, with a small Detune (voices 10–20 Hz apart) and Stereo 0, 2.5, 5, 10: each voice's left and right level. The sides at Stereo 10 give the order and whether it follows pitch; Stereo 5 gives 0.924 / 0.383 for equal power and a linear width, 0.75 / 0.25 for a linear law. |
| O15 | Level against the voice count: none, 1/√N, or 1/N? | None | Retrig on, Phase 0, Detune 0, Stereo 0, a sine: RMS at N = 1, 2, 4, 8. Each doubling adds +6.02 dB with no normalisation, +3.01 dB with 1/√N, and 0 dB with 1/N. Repeat with Retrig off and a small Detune (power adds: +3.01 dB per doubling with none). |
| O16 | Noise with unison: do Voices and Stereo change the noise's level and width? | Yes: N voices' power, and their L/R correlation | The Noise waveform at N = 1, 2, 4, 8 and Stereo 0, 5, 10: each channel's RMS (none: +3.01 dB per doubling) and the left/right correlation (1 at Stereo 0, 0 at 10 with the voices split). Folded into W7. |

### Waveforms

**Names (high confidence).** Three independent third-party sources list the same eight: Saw, Pulse, HPulse, QPulse, Triangle, Sine, Noise, TriSaw ([Vintage Synth](https://www.vintagesynth.com/lennar-digital/sylenth1) spec sheet; [ADSR beginner tutorial](https://www.adsrsounds.com/sylenth-tutorials/first-30-minutes-sylenth-beginner-tutorial/); [Splice](https://splice.com/blog/taking-closer-look-lennar-digital-sylenth1-plugin/), which names HPulse, QPulse, and TriSaw as the "modified waveforms"). KVR forum posts use the same names. The [MusicRadar oscillators article](https://www.musicradar.com/tuition/tech/how-to-master-sylenth1s-parts-and-oscillators-630710) could not be read (its body loads client-side). A search-engine excerpt of it expands the names to "Half Pulse" and "Quarter Pulse", but that excerpt is not a primary reading. The two lists disagree on the selector order (Sine before or after Triangle), so the order is open (W9).

**Shapes.** No source describes the shapes of HPulse, QPulse, or TriSaw. The shapes below are the waveform over one cycle, with phase t ∈ [0, 1) and value y. A *jump* J is a step in y. A *bend* K is a change of slope, in units per cycle. Everything else is a straight segment. "Confidence" refers to the shape. The names are settled.

| Waveform | Shape over one cycle | Source | Confidence |
|---|---|---|---|
| Saw | Rising ramp y = 2t − 1. One jump J = −2 at t = 0. | Name: as above. Rising: the manual's Inv diagram (p. 15) draws the plain saw rising and the inverted one falling. | High for the shape; medium for "rising", because the diagram is an illustration. |
| Pulse | y = +1 on [0, ½), −1 on [½, 1). Jumps J = −2 at ½ and J = +2 at 0. | KVR forum, [thread 301405](https://www.kvraudio.com/forum/viewtopic.php?t=301405): users identify "Pulse" as the square wave. The [ADSR pulse-lead tutorial](https://www.adsrsounds.com/sylenth-tutorials/learn-how-to-make-a-huge-pulse-lead-in-sylenth/) says there are "three variations of the Pulse waveform" (Pulse, HPulse, QPulse). | Medium: 50% is inferred from "it's the square", not stated. |
| HPulse | A narrower fixed pulse: y = +1 on [0, w), −1 after, with **hypothesis w = ¼** ("half" the square's width). Jumps at 0 and w. | KVR [thread 272881](https://www.kvraudio.com/forum/viewtopic.php?t=272881) treats it as narrower than Pulse and QPulse as narrower again. Fixed widths: KVR thread 301405 ("fixed width pulse waves, you just can't modulate the pulse width"). | Low for w. Levels and DC are open (W4). |
| QPulse | As HPulse, with **hypothesis w = ⅛** ("quarter" of the square's width). | As HPulse. | Low for w. |
| Triangle | −1 → +1 over [0, ½), +1 → −1 over [½, 1). Bends K = −8 at ½ and K = +8 at 0. | Name only. | High for the shape; start phase open (W6). |
| Sine | sin 2πt. | Name only. | High for the shape; start phase open (W6). |
| TriSaw | **Unknown** triangle/saw hybrid. Three candidates, all piecewise linear: (a) a skewed triangle, rising over [0, p) and falling over [p, 1) with p ≠ ½: bends only, harmonics falling as 1/k²; (b) a triangle plus a saw, rescaled: one jump and two bends, harmonics falling as 1/k; (c) a triangle-shaped first half and a saw-shaped second half: one jump and bends. | Name only (Splice, Vintage Synth, ADSR). | None. Must be measured (W3). |
| Noise | Not periodic. **Hypothesis: white**, uniformly distributed, the same for every note. | Name only. One tutorial (ADSR pulse lead) speaks of band-passing "a white noise" into "a bright noise", which suggests white but is not a specification. | Low for spectrum and distribution (W7). |

**Invert and PWM (high confidence, official).** Manual p. 15: Inv inverts the oscillator's output. Two saws, one inverted, sum to a pulse, and changing the phase of one (both oscillators retriggered) changes the pulse width. The developer (posting as "Lennard" in the LennarDigital support forum, KVR thread 301405, December 2010) adds that the two-saw sum has no DC, unlike a ±1 pulse whose width varies, and that a DC offset "can cause numerous problems". That remark is why W4 asks whether HPulse and QPulse are DC-free.

**Implemented as provisional (2026-11-13).** M1.1 builds the working hypotheses below, with TriSaw as candidate (b), (triangle + saw)/2, and the narrow pulses at ±1. Each is one row of data, so a measurement that disagrees changes that row and its expected test values. See [oscillator-waveforms.md](../dsp/oscillator-waveforms.md).

**Open questions, settled by measuring the demo (M1.13).** Black-box only. Common setup: one oscillator, 1 voice, Retrig on, Phase 0, Inv off, filters bypassed, amp envelope at A = 0 and S = max, effects off, rendered offline to 48 kHz WAV. Keep renders shorter than the demo's reminder sound interval (about 60 s).

| # | Question | Working hypothesis | Measurement that settles it |
|---|---|---|---|
| W1 | Pulse width of Pulse. | 50% | Render a low note (A0, 27.5 Hz: 1745 samples per cycle at 48 kHz) and measure the high and low times. Check: a 50% pulse has no even harmonics. |
| W2 | Pulse widths of HPulse and QPulse. | 25% and 12.5% | As W1. In the spectrum, a width w has a null at every harmonic k where k·w is a whole number: every 4th harmonic missing for 25%, every 8th for 12.5%. |
| W3 | The shape of TriSaw. | (b), (triangle + saw)/2: chosen as the provisional shape because it differs clearly from the triangle and has both a jump and bends | First, the Wave display's icon in the demo (to read the rough shape, not to copy the graphic). Then one cycle at A0 on a scope: count the jumps and bends and fit straight segments. The harmonic roll-off decides between "bends only" (1/k², candidate a) and "has a jump" (1/k, candidates b and c). |
| W4 | Levels and DC of the narrow pulses: ±1 with a DC of 2w − 1, or shifted to be DC-free (+1.5/−0.5 for 25%, for example), or rescaled? | ±1, not DC-free | The mean and the peaks of a render of several seconds. The output path must be DC-coupled. It is digital, so it should be, but check the saw's mean ≈ 0 as a control. |
| W5 | Relative levels across waveforms at the same Volume: equal peak, equal RMS, or something else? | Equal peak (±1) | Peak and RMS of each waveform at A0 with the same Volume. Use a low note so the band-limiting overshoot does not distort the peak reading. |
| W6 | Polarity and start phase for each waveform: what Phase 0 starts on (the saw at −1, sine at 0 rising, triangle at −1 or at 0, pulse high first?) | Saw at −1 rising, pulse high first, triangle at −1, sine at 0 rising | The first cycle after note-on with Retrig on and Phase 0, then Phase 90 and 180 to confirm the direction. |
| W7 | Noise: spectrum (white or coloured), amplitude distribution, whether it depends on the note or the sample rate, and whether unison voices are decorrelated. | White, uniform, independent of the note, decorrelated | Power spectral density of a long render (flat = white, −3 dB/octave = pink); a histogram (flat = uniform, bell-shaped = Gaussian); compare notes far apart and 44.1 against 48 kHz; with 2 voices and Stereo 10, the correlation between left and right. |
| W8 | Overshoot: do Sylenth1's oscillators overshoot like a minimum-phase BLEP (about 37% on a saw)? | Unknown | Peak of a high saw (MIDI 96 and up), against W5's low-note peak. Already an open item in [band-limited-oscillators.md](../dsp/band-limited-oscillators.md). |
| W9 | The order of the waveforms in the selector, and the default. | Saw first; default Saw (the manual's p. 14 screenshot shows a saw icon) | Step through the selector in the demo from an initialised patch. Matters only for our own UI order and parameter-value mapping. |

## 3. Filters (pp. 5, 16–18)

**Per-part filter (A and B)**

- **Input select:** A, B, AB, or `--` (no input).
- **Type:** Bypass, Lowpass, Bandpass, Highpass. Bypass stops processing altogether.
- **Slope:** 12 dB/oct or 24 dB/oct.
- **Cutoff, Resonance:** resonance goes well past self-oscillation.
- **Drive:** drives the filter's **4 stages, each with nonlinear saturation**.

**Filter Control (shared)**

- **Cutoff and Resonance:** act on both filters at once. How they combine with each filter's own knobs is **unspecified**.
- **Keytrack:** 0–10. At 10, cutoff follows note pitch exactly, so a self-oscillating filter can be played in tune.
- **Warm Drive:** on/off. Switches the saturation to a higher-quality mode with more harmonics, at extra CPU cost.

## 4. Amplitude envelopes (p. 17)

One ADSR per part (Amp Env A and B). An attack or release of 0 is allowed and clicks on purpose.

## 5. Mixer (p. 18)

Mix A, Mix B, Main Volume (after the effects), and a stereo VU meter. The output is **not clipped**.

## 6. Modulation (pp. 19–20)

There is **no general mod matrix.** Each source has its own slots:

| Source block | Count | Destinations each | Controls |
|---|---|---|---|
| Mod Envelope | 2 | 2 | ADSR |
| LFO | 2 | 2 | Wave (10 waveforms, **not named**), Rate, Gain, Offset (±), Free |
| Misc panel | 2 panels × 2 selectable sources = 4 | 2 | Source selector |

- Each destination has a **bipolar amount** knob (centered means none; double-click resets it).
- **Misc sources:** velocity, aftertouch, keytrack, mod wheel, amp envelopes, mod envelopes, LFOs, and arpeggiator **Step Velocity**. The full list is **unspecified**.
- **Destinations:** chosen from a grouped popup. Examples in the manual include Volume A/B/AB, Pitch A, Cutoff AB, Reso, Pan AB, Mix AB, LFO gain, and Phaser center frequency. The **full list is unspecified**, so recover it from the plugin.
- **LFO Free:** the LFO is not retriggered on note-on and does **not** follow tempo sync.
- Phase and pitch modulation are evaluated **per voice**, which makes them expensive (p. 30).

## 7. Voicing and keyboard (pp. 11–12, 29)

- **Polyphony:** 0–16 notes. The Voices readout shows active voices and the maximum, where the maximum is the sum of the oscillator voice counts × polyphony.
- **Pitch bend,** with **Bend Range** in semitones.
- **Mod wheel** is available as a mod source.
- **Mono Legato:** forces mono and does not retrigger envelopes, so pitch and velocity slide between notes.
- **Portamento:** time knob plus a mode switch. In **Normal**, it slides only when notes overlap. In **Slide**, it always slides.
- **Hold pedal:** CC 64.

## 8. Tempo sync (p. 12)

A global **Sync** switch shows LFO rates, delay times, and similar values as note lengths (for example 1/4 and 1/8, plus **T** for triplet and **D** for dotted) locked to host tempo. When it is off, those values are shown in seconds or Hz.

## 9. Arpeggiator (pp. 6, 21–22)

- **Modes:** "10 melodic modes". The ones named are Up, Down, Up/Down (several variants), Random, Ordered (the order notes were played), Step Seq (one note at a time), and Step Chord. **The exact set of 10 is unspecified.**
- **Time** sets the step rate. **Gate** sets note length as a fraction of a step. **Wrap** restarts the sequence after N notes (at least 16, judging by the screenshot). **Octave** sets the range in octaves.
- **Step sequencer:** each step has a **Transpose** value (semitones, used in the Step modes only), a **Velocity** value (`-` means rest), and a **Hold** flag that ties to the next step for slides. Steps are arranged in pages, and pages can be copied and pasted.
- **Velocity mode:** Key, Hold (the last key's velocity), Step, Step+Key, or Step+Hold.
- Outputs **Step Velocity** as a modulation source, which is useful for gating.

## 10. Master effects (pp. 6, 9, 20–28)

The effects are in series, **in this order:** Distortion → Phaser → Chorus → EQ → **Reverb → Delay** → Compressor. Each has an on/off switch (off frees its CPU) and copy/paste.

| Effect | Parameters | Notes |
|---|---|---|
| Distortion | Type (Overdrive, Foldback, Clip, Decimate, Bitcrush), Amount, Dry/Wet | 4× oversampling, stereo |
| Phaser | CenterFreq, Spread, LR Offset, Width, LFO Rate, LFO Gain (0 turns the LFO off), Feedback, Dry/Wet | 6 stages, stereo, 2× oversampling |
| Chorus/Flanger | Delay (ms), Rate, Depth, Feedback, Dual Mode, Width, Dry/Wet | Each stage has 2 delay lines and 2 LFOs. Dual Mode gives 4 lines in quadrature. 2× oversampling. Flanging at 1–5 ms delay with feedback. |
| EQ | Bass, BassFreq, Treble, TrebleFreq | **Normalizes output volume automatically** when boosting |
| Reverb | PreDelay (screenshot suggests 0–200 ms), Damp, Size, Width, Dry/Wet | |
| Delay | Delay L, Delay R, Feedback, PingPong (on/off), Spread (ping-pong only: 0 gives L-C-R, 10 gives full ping-pong), Width, Low Cut, High Cut, Smear, Dry/Wet | Smear spreads each echo's spectrum over time |
| Compressor | Ratio (1:1 to 100:1), Threshold (0 to −30 dB), Attack, Release | Stereo |

## 11. Presets and banks (pp. 11–13)

- A bank holds 512 presets in 4 sub-banks of 128. MIDI program change reaches only the first 128.
- Programs can be changed from the host, with the prev/next arrows, from a popup list, or by MIDI program change.
- Load and save single presets and whole banks (Sylenth1 uses `.fxp` and `.fxb`; ours will use our own format).
- Menu actions: Copy, Paste, Insert, Delete, **Clear** (initialize), **Randomize**, **Reset** (undo all edits to the preset), and rename.

## 12. MIDI control (pp. 10–11, 31–32)

- Every main knob has a **default CC** assignment (the table is on pp. 31–32, CC 1–127, including CC 123 All Notes Off).
- **MIDI Learn and Forget** are available from each knob's right-click menu. Learned assignments **persist across sessions** (global, not stored per preset).

## 13. UI interaction (p. 10)

These are not DSP features, but they are expected UI behavior:

- Drag vertically to change a value. Shift-drag gives fine control. Alt switches to rotary drag.
- Double-click or Ctrl-click resets a knob to its center or default value.
- Clicking a knob shows its value on the LCD without changing it.

## 14. Performance notes from the manual (p. 30)

- The voice path (oscillators, filters, envelopes) is duplicated for every note, so its cost grows with polyphony.
- One oscillator with 4 voices is cheaper than 4 oscillators with 1 voice each, which suggests unison is vectorized inside each oscillator.
- Bypassed filters and disabled effects cost nothing.
- Sylenth1 uses SSE. These notes are inputs to Part 3.

## 15. Additions after manual v2.5 (from the v3.x changelog)

Source: [official changelog](https://lennardigital.com/sylenth1/whatsnew.php). The latest entry is v3.075 (April 2026). Only changes relevant to the engine or to features are listed; skins, licensing, and bug fixes are left out.

**Sound engine**
- v3.050: oscillators have less aliasing and more high-frequency content; filter response is improved.
- v3.050: **oversampling options**, with separate settings for real-time playback and offline render.
- v3.050: **master tuning** control and improved pitch accuracy.
- v3.072: **oscillator drift** emulation (a settings-file option).
- v3.029: negative volume modulation was removed. A legacy option keeps the v2 engine behavior.

**Arpeggiator**
- v3.050: **Swing** knob.

**Parameters, parts, and presets**
- v3.053: **Link AmpEnv A and B** option.
- v3.031 and v3.053: **parameter and part locking**, so locked values survive preset changes.
- v3.063 and v3.066: **Copy to Part A/B** and further "Copy To" menu options.
- v3.024: alphanumeric sorting in the bank menus.
- v3.029 and later: drag-and-drop preset loading. Option to load presets by file name.

**Host and MIDI**
- v3.008: sample-accurate automation.
- v3.008, v3.050, v3.063, v3.072: MIDI learn by Ctrl-click, a MIDI-learn auto-switch option, MIDI learn for bank and program select, and MIDI learn for pitch bend.
- v3.050: better host automation of buttons and option controls.

**UI**
- v3.000: resizable GUI, high-DPI and Retina support, and customizable skins.
- v3.040: option to display cutoff in Hz.
- v3.040: improved Shift-drag sensitivity.
