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
| Voices | 0–8 unison voices. 0 turns the oscillator off. |
| Waveform | 8 waveforms "emulating common analog wave shapes" (p. 14). **The manual does not name them.** Third-party sources do: see [Waveforms](#waveforms) below. |
| Octave / Note | Coarse tuning. Centered by default (0). |
| Fine | ±1 semitone. |
| Volume | Oscillator output level. |
| Phase | 0–360° start phase. Only has an effect when Retrigger is on. |
| Detune | Spreads the pitch of the unison voices around the center pitch, as in a supersaw. |
| Stereo | Spreads unison voices across the stereo field: 0 is mono, 10 is full width. Needs more than 1 voice. |
| Pan | Oscillator pan. |
| Retrig | Restarts every voice at Phase on each note-on. |
| Inv | Inverts the output, for PWM-style tricks. |
| C / P | Copy and paste oscillator settings, including between presets. |

Requirements: alias-free; stable from below 0.01 Hz up to Nyquist; up to 32 voices per note (4 oscillators × 8 unison voices) and 512 voices in total.

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
