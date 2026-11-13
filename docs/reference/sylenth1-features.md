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
| Waveform | 8 analog-style waveforms. **The manual does not name them.** |
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
