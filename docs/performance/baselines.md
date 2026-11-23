# CPU baselines

Benchmark results recorded over time, newest first. Phase 3 measures every optimisation against these. How to run, read, and record them: [benchmarking.md](benchmarking.md).

Compare only rows from the same machine and build. Percentages are of one core, for one channel, in real time at 48 kHz (20,833 ns per sample).

---

## 2026-11-23: M1.5 the destinations, as built

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice-destinations]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread up to 8.6%; one prototype run of the crossfading row, 769.9 ns, was a host stall and is outside the median).
- **What is measured** ([modulation-destinations.md](../dsp/modulation-destinations.md#cpu-stage-2)): stage 1's held voice (the init patch, A1 one saw, A3, past the attacks) and routes, `dsp::Voice` with the destinations (libs/dsp voice/voice.hpp) and, in the same runs, the stage-1 prototype in its decided configuration (`decidedAt()`).

| Benchmark: one voice (ns / sample) | `dsp::Voice` | prototype (decided) |
|---|---:|---:|
| no routes | 131.7 | 130.0 |
| Volume A, 4 routes | 145.6 | 144.9 |
| Pan A, 4 routes | 252.2 | 252.8 |
| Phase A, 4 routes | 140.8 | 142.5 |
| 16 routes, every destination | 259.7 | 263.1 |
| 16 routes, a change at every grid point (always crossfading) | 286.7 | 283.2 |

### Observations

- **No routes costs nothing against the voice before this stage**: the commit before it (c35c5f1), built in a separate worktree and run interleaved with this one 14 times, 130.3 against 130.1 ns (medians), paired differences −9.4 to +8.1, median +1.3. Without the fast path for a part with no Volume, Pan or Phase modulation it was +2.5 (six of seven pairs above).
- **The library runs level with the prototype**, and both with stage 1's numbers.

---

## 2026-11-23: M1.5 the destinations, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[destinations]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 1.0–5.3%).
- **What is measured** ([modulation-destinations.md](../dsp/modulation-destinations.md#cpu)): one voice held at the init patch (A1 one saw, A3, past the attacks), `dsp::Voice` and the stage-1 `DestVoice` (tests/dsp/modulation/destination_voice.hpp), the knobs from the slot's own ramps unless stated. 4 routes into a destination: Mod Env 1, LFO 1, Velocity, ModWheel. 16: every slot, every source. *Every destination*: two routes each to Volume, Pan and Phase, and Pitch, Mix, Cutoff, Reso and the LFOs. *A change at every grid point*: slot 0's amount set anew every 32 samples. 16 voices: question 4's pluck patch (B1 at Volume 0 lifted by Mod Env 1, decay 50 ms, sustain 0), 16 notes held past the plucks.

| Benchmark: one voice (ns / sample) | value |
|---|---:|
| `dsp::Voice`, no routes | 128.9 |
| `DestVoice`, no routes | 131.4 |
| Volume A: 4 / 16 routes | 143.3 / 153.8 |
| Phase A: 4 / 16 routes | 138.1 / 146.1 |
| Pan A: 4 / 16 routes | 250.9 / 254.8 |
| Volume A, 4 routes, knobs (a) targets / (b) the slot's ramps / (c) global ramps | 143.5 / 144.7 / 145.6 |
| 16 routes, every destination | 259.8 |
| 16 routes, a change at every grid point: step / crossfade 10 ms | 268.5 / 282.8 |
| 16 routes, no change, crossfade 10 ms | 259.2 |
| Noise: no routes / Volume A, 4 routes, ignored / mixed | 272.1 / 272.2 / 283.5 |
| 4 × 8 unison: no routes / Volume AB, 4 routes / Pan AB, 4 routes | 655.8 / 741.0 / 720.4 |
| 16 voices, pluck patch: skip never / from the routes' bound | 3,235.1 / 2,083.6 |

### Observations

- **Volume and Phase are cheap** (+12–22 and +7–15 ns per voice); **Pan costs leaving the mono path** (about 120 ns), as in task 2.
- **A 10 ms crossfade of the routes costs nothing while nothing changes**, and +14.3 ns per voice (+5.3%) while one always runs.
- **The Volume skip test from the routes' bound saves 36%** on 16 voices of the pluck patch.
- **The prototype with no routes is +2.5 ns over `dsp::Voice`** (its knob and presence ramps advanced on the unmodulated path). Stage 2 advances them lazily.

---

## 2026-11-23: M1.5 the sources, as built

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice-sources]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.3–4.4%).
- **What is measured** ([modulation-sources.md](../dsp/modulation-sources.md#cpu-stage-2)): one voice held at the init patch (A1 one saw, A3, past the attacks), `dsp::Voice` with its sources (libs/dsp voice/voice.hpp) and, in the same runs, the stage-1 prototype in its decided configuration (folded, sources run only while used). Stage 1's routes: 4 (Mod Env 1 → Pitch A, LFO 1 → Mix A, Velocity → Mix A, ModWheel → Pitch A) and 16 (every slot, every source, into Pitch, Mix, Cutoff and LFO 2 Gain). *Moving*: the wheel and pressure get a new value every block (a 5 ms ramp).

| Benchmark: one voice (ns / sample) | `dsp::Voice` | prototype |
|---|---:|---:|
| no context (`VoiceKnobs::sources` null) | 123.9 | |
| 0 routes (an empty patch) | 123.6 | 123.7 |
| 4 routes | 134.6 | 136.3 |
| 16 routes | 154.8 | 159.5 |
| 4 routes, controllers moving | 136.1 | 137.7 |
| 16 routes, controllers moving | 158.0 | 162.6 |

**Against today's voice.** Stage 1's `[sources]` benchmarks, run by the binary built at 8fed2a5 (`dsp::Voice` before this stage) and by this one, interleaved, seven runs each; `dsp::Voice` at the init patch:

| `sources: dsp::Voice, init` (ns / sample) | median | spread |
|---|---:|---:|
| 8fed2a5 (before the sources) | 127.2 | 4.5% |
| this build, no context | 129.2 | 5.2% |

The runs, in pairs (before, after): 123.5/130.1, 123.2/129.9, 122.8/123.3, 128.5/129.3, 127.9/126.3, 127.2/129.2, 127.6/123.8. The paired differences run from −3.8 to +6.7 ns, median +0.8 (+0.6%), with the machine drifting by about 4 ns between the first three pairs and the rest.

### Observations

- **No routes is today's voice**, within the noise: a paired median of +0.8 ns. The voice adds one comparison per grid point and a check per chunk.
- **Routes cost what stage 1 measured, or slightly less**: +11.0 ns per voice for 4 routes (stage 1: +11.9), +31.2 for 16 (+33.5); moving controllers +12.5 and +34.4. The library runs 1–3% under the prototype in the same runs.

---

## 2026-11-22: M1.5 the sources, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[sources]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.8–6.6%; one run of "16 routes, buffered, moving" in an earlier set was 672 ns from a host stall and isn't used).
- **What is measured** ([modulation-sources.md](../dsp/modulation-sources.md#what-they-measured)): one voice held at the init patch (A1 one saw, A3, past the attacks), `dsp::Voice` and the stage-1 `SourceVoice` (tests/dsp/modulation/source_candidates.hpp). 4 routes: Mod Env 1 → Pitch A, LFO 1 → Mix A, Velocity → Mix A, ModWheel → Pitch A. 16 routes: every slot, every source, into Pitch, Mix, Cutoff and LFO 2 Gain. *Moving*: the wheel and pressure get a new value every block (a 5 ms ramp).

| Benchmark: one voice (ns / sample) | value |
|---|---:|
| `dsp::Voice`, init | 128.7 |
| `dsp::Voice`, init, filter B on `--` | 128.2 |
| `SourceVoice`, no routes, sources run only while used | 128.7 |
| `SourceVoice`, no routes, every source always (sustains, sine and triangle LFOs) | 131.0 |
| `SourceVoice`, no routes, envelopes in a 10 s attack, Lorenz LFOs: used only / always | 128.2 / 132.2 |
| 4 routes: buffered / inline / folded | 145.3 / 158.7 / 140.6 |
| 16 routes: buffered / inline / folded | 177.3 / 197.8 / 162.2 |
| 4 routes, controllers moving: buffered / inline / folded | 144.7 / 157.2 / 141.7 |
| 16 routes, controllers moving: buffered / inline / folded | 178.9 / 197.8 / 167.4 |
| 4 routes, folded, every source always | 140.1 |
| filter B on `--`, its amp envelope always run / run only as a source | 125.9 / 128.0 |

### Observations

- **No routes costs nothing**: 128.7 against `dsp::Voice`'s 128.7. Running both mod envelopes and both LFOs anyway costs 2.3–4.0 ns per voice (1.8–3.1%).
- **Folded is the cheapest delivery**: +11.9 ns per voice for 4 routes and +33.5 for 16, against buffered's +16.6 / +48.6 and inline's +30.0 / +69.1.
- **An unheard part's amp envelope** costs nothing measurable to keep running.

---

## 2026-11-22: M1.5 the modulation model, as built

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[modulation]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.6–6%, except accumulate with 4 routes, 18% from one fast run).
- **What is measured** ([modulation.md](../dsp/modulation.md#cpu-stage-2)): the library's `anoesis::dsp::modulation` (libs/dsp modulation/), per call, on stage 1's mixed patch (16 live routes): `accumulate()` with 0, 4 (slots 0, 4, 8 and 10: an envelope, an LFO, velocity, the wheel) and 16 routes; `compile()` of the 16 slots; 16 source bounds in a runtime order; Mix A's bound from the 16 routes; `RouteCompiler::update()` unchanged and after a change. Then the prototype's registry (a) on the same routes in the same binary, and stage 1's own registry benchmarks in the same runs.

| Per call (ns) | library | prototype (a), same routes | stage 1's registry (a) benchmark, these runs |
|---|---:|---:|---:|
| accumulate, 0 routes | 1.72 | | |
| accumulate, 4 routes | 6.50 | | |
| accumulate, 16 routes | 13.70 | 13.56 | 13.91 |
| compile, 16 routes | 22.30 | 17.56 | 17.45 |
| 16 source bounds (a runtime order) | 20.83 | | |
| Mix A's bound from 16 routes | 8.09 | | |
| `update()`, unchanged | 0.62 | | |
| `update()`, after a change | 27.13 | | |

### Observations

- **`accumulate()` is the prototype's cost**: 13.70 ns for 16 routes against 13.56 on the same routes, 13.91 for stage 1's benchmark in these runs, and stage 1's recorded 13.97. With no routes, the 1.72 ns is the benchmark zeroing its 28 sums; the loop itself is empty.
- **`compile()` is 4.7 ns slower than the prototype's** (22.30 against 17.56) because it also clamps the amount and records `routesTo()` and `uses()`. It runs once per voice per patch change, at a grid point. With the patch unchanged a grid point costs 0.62 ns.
- **These are per call, not per sample.** A voice calls `update()` once per 32-sample segment (0.02 ns per sample), and `accumulate()` per segment or per sample as each destination's rate requires (tasks 3 and 4).

---

## 2026-11-22: M1.5 the modulation model, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[modulation]"`, 100 samples per benchmark, three runs; the tables give each row's median (spread 0.4–6.6%).
- **What is measured** ([modulation.md](../dsp/modulation.md#cpu)): one voice held at the init patch (A1 one saw, A3, past the attacks), `dsp::Voice` and the stage-1 `ModVoice` (tests/dsp/modulation/modulation_candidates.hpp). For each destination and rate, 4 routes into it (an envelope, an LFO, velocity, the mod wheel) or all 16 (every slot's source). For LFO 1 Rate and Gain, LFO 1 also sits on Pitch A, and that route alone is their 0. Then a mixed patch of 16 routes at the recommended rates, one voice and 16 (keys 40–55); and the three registries on their own.

| Benchmark: one voice (ns / sample) | 0 routes | 4 routes | 16 routes |
|---|---:|---:|---:|
| `dsp::Voice` | 130.23 | | |
| `ModVoice` | 128.73 | | |
| Pitch A: per sample / held / ramped | | 139.71 / 132.56 / 136.60 | 144.82 / 138.78 / 140.31 |
| Phase A: per sample / held / ramped | | 140.78 / 134.44 / 136.07 | 146.89 / 138.23 / 141.46 |
| Volume A: per sample / held / ramped | | 140.05 / 135.93 / 134.25 | 151.10 / 145.08 / 145.05 |
| Pan A: per sample / held / ramped | | 261.63 / 248.36 / 248.48 | 247.92 / 231.26 / 234.60 |
| LFO 1 Rate: per sample / held | 158.60 / 135.41 | 170.92 / 140.90 | 235.64 / 141.03 |
| LFO 1 Gain: per sample / held / ramped | 159.30 / 136.24 / 138.66 | 164.67 / 137.06 / 136.50 | 207.71 / 139.39 / 141.69 |
| 16 routes, mixed, at the recommended rates | | | 273.46 |

| Benchmark | ns / sample |
|---|---:|
| 16 voices, no routes | 2,032.93 |
| 16 voices, 16 routes, mixed | 4,114.57 |

| Registry: per call | (a) table | (b) functions | (c) types |
|---|---:|---:|---:|
| accumulate 16 routes | 13.97 | 39.48 | 42.55 |
| 16 bounds (sources in a runtime order) | 18.93 | 33.72 | 29.40 |
| compile 16 routes | 18.71 | | |

### Observations

- **No routes costs nothing measurable** (128.7 against 130.2 ns per voice).
- **Each destination's own cost is a few ns per voice**: 4 routes into it add 4–12 ns, 16 routes 10–22 ns, at any rate. Per sample costs 3–6 ns more than ramped for Pitch, Phase and Volume.
- **A Pan route costs 100–135 ns per voice, at every rate.** That is the mono path ([part-solo-and-skipping.md](../dsp/part-solo-and-skipping.md)): a part whose Pan moves isn't centred, so its filters run in stereo. The init patch costs about 240 ns per voice in stereo (the task 5 entry), and the pan law per sample adds about 13 ns on top of that.
- **LFOs per sample** (sample order) cost 23 ns per voice more than on the grid with one route, and 68–95 ns more with 16 routes into the gain or the rate. On the grid (segment order) they cost nothing measurable.
- **The mixed patch doubles 16 voices** (2,033 → 4,115 ns per sample), mostly through its Pan routes.
- **Registry (a) is the fastest at every job.** Its sums are a loop over plain data, while (b) calls through a pointer and (c) walks a fold of comparisons. A benchmark that asked for each source's bound in a compile-time order inlined (b) and (c) down to 2 ns. A voice asks in its routes' order, so the table gives the runtime order.

---

## 2026-11-22: M1.4 AmpEnv A/B link, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[amp-link]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.1–1.7%, one row 5.0% from one slow run).
- **What is measured** ([amp-envelope-link.md](../dsp/amp-envelope-link.md#cpu)): 16 voices held (keys 40–55) through `VoiceAllocator`, linked from before the notes with part B's own knobs different: `dsp::Voice` unlinked and under a parameter link (the engine mirroring the knobs: the same code), the stage-1 `LinkVoice` in tests/dsp/voice/link_candidates.hpp with no link, with one generator shared by both parts (b), and with a signal link (c). Four cases: the init patch, two parts (A1 and B1 saws), a pad (unison into filter A on AB), and two parts with both envelopes in a 10 s attack (a level computed every sample).

| Benchmark: 16 voices (ns / sample) | `dsp::Voice`, no link | (a) `dsp::Voice` linked | prototype, no link | (b) shared | (c) signal |
|---|---:|---:|---:|---:|---:|
| init | 2,017.25 | 2,011.43 | 2,011.56 | 2,023.77 | 2,015.02 |
| two parts | 3,925.12 | 3,932.70 | 3,937.01 | 3,954.34 | 3,944.04 |
| pad | 8,680.39 | 8,639.36 | 8,624.26 | 8,662.91 | 8,665.02 |
| two parts, 10 s attacks | 3,955.08 | 3,960.49 | 3,960.72 | 3,950.27 | 3,958.87 |

### Observations

- **No candidate costs or saves anything measurable.** One envelope in a moving stage costs 0.52 ns per sample (the M1.2 entry), so sharing one generator saves at most about 8 ns per sample for 16 voices, 0.2% of the two-part case and below the spread.
- **The init patch reads 2,017 against the task 5 entry's 1,958 for the same code**: 3% of session-to-session spread on this machine. Compare rows within one entry.

---

## 2026-11-22: M1.4 part solo and skipping, as built (`dsp::Voice`'s plan, the mono path, `PartSolo`)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[solo]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.4–5.2%; one prototype row 26.9%, from one slow run).
- **What is measured** ([part-solo-and-skipping.md](../dsp/part-solo-and-skipping.md#results-as-built)): the same 16 held voices as the stage-1 entry below, now through `dsp::Voice` as built (the per-segment plan, the frozen skip, the mono path; Solo's gains from `PartSolo`), beside the stage-1 prototype's rows re-run in the same session.

| Benchmark: 16 voices (ns / sample) | as built | before stage 2 (stage-1 entry) |
|---|---:|---:|
| init | 1,958.07 | 5,762.34 |
| filter B `--` | 1,948.36 | 3,807.23 |
| two parts, Mix B 0 | 1,960.66 | 7,574.75 |
| two parts, B soloed out (`PartSolo`) | 1,973.01 | 7,516.02 (gain, nothing skipped) |
| two parts sounding | 3,831.56 | 7,569.00 |
| every oscillator at Voices 0 | 0.11 | 0.11 |
| worst, centred | 6,139.00 | 10,259.06 |
| worst, spread (nothing to skip) | 9,906.74 | 10,139.92 |

The prototype's rows this session: init nothing skipped 5,684.86, atRest 3,740.68, mono only 2,946.60, atRest + slots + mono 1,989.88; filter B `--` atRest + slots + mono 1,946.73; Mix B 0 nothing skipped 7,670.68, atRest + slots 7,379.26, freeze + slots 3,765.68, freeze + slots + mono 2,001.97; B soloed out, gain nothing skipped 7,375.04, gain freeze + slots 3,820.39, routing 3,768.61; two parts sounding freeze + slots + mono 3,882.68; worst centred mono only 6,063.48, freeze + slots + mono 6,021.35; worst spread nothing skipped 10,037.01, freeze + slots + mono 10,085.61.

### Observations

- **As built costs what the decided prototype does**, within noise in every row.
- **The init patch is a third of what it was** (360 → 122 ns per voice), and a part at Mix 0 or soloed out costs nothing beyond it: 16 voices of two parts with B silenced cost 1,961–1,973 against 7,375–7,575.
- **Where nothing can be skipped, the plan costs nothing measurable** (worst, spread: 9,906.7 as built, 10,037.0 for the prototype with nothing skipped).
- **The exact skip of a filter at rest is only visible here:** with it removed (a deliberate break), the init patch read 3,036.72 (+55%), while every output test still passed, as it must.

---

## 2026-11-22: M1.4 part solo and skipping, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[solo]"`, 100 samples per benchmark, three runs; the table gives each row's median (spread 0.3–4.8%, one row 8.5%).
- **What is measured** ([part-solo-and-skipping.md](../dsp/part-solo-and-skipping.md#cpu)): 16 voices held (keys 40–55) through `VoiceAllocator`, as built (`dsp::Voice`) and as the stage-1 `SkipVoice` in tests/dsp/voice/solo_candidates.hpp with each skipping candidate: *atRest* (a filter at rest on exact zeros not solved), *freeze* (also a part whose output gain is 0, its filter's state kept), *slots* (oscillators no running filter takes, or at Volume 0, skipped with their phases advanced), *mono* (the mono path). *Two parts*: A1 and B1 saws on their own filters, centred.

| Benchmark: 16 voices (ns / sample) | this commit |
|---|---:|
| init: `dsp::Voice` / prototype, nothing skipped | 5,762.34 / 5,796.45 |
| init: atRest / mono only / atRest + slots + mono | 3,831.60 / 3,025.23 / 2,012.64 |
| filter B `--`: `dsp::Voice` / atRest + slots + mono | 3,807.23 / 1,988.14 |
| two parts, Mix B 0: `dsp::Voice` / nothing skipped / atRest + slots | 7,574.75 / 7,532.97 / 7,527.19 |
| two parts, Mix B 0: freeze + slots / freeze + slots + mono | 3,839.98 / 2,007.46 |
| two parts, B soloed out: gain, nothing skipped / gain, freeze + slots / routing | 7,516.02 / 3,827.40 / 3,793.54 |
| two parts sounding: `dsp::Voice` / freeze + slots + mono | 7,569.00 / 3,941.86 |
| every oscillator at Voices 0: `dsp::Voice` / atRest | 0.11 / 0.11 |
| worst, centred: `dsp::Voice` / mono only / freeze + slots + mono | 10,259.06 / 6,088.75 / 6,065.74 |
| worst, spread (nothing to skip): `dsp::Voice` / nothing skipped / freeze + slots + mono | 10,139.92 / 10,134.20 / 10,105.16 |

### Observations

- **A filter at rest on zeros, skipped exactly, takes a third off the init patch** (360 → 240 ns per voice); with the mono path, 126.
- **Mix B at 0 or Solo saves nothing unless the fed filter is skipped too** (freeze: 473 → 240 ns per voice); Solo as a gain plus freeze costs what routing does.
- **The mono path saves 47.5% (init) and 40.6% (worst, centred)**, as M1.4 task 1 measured.
- **The test costs nothing measurable** where nothing can be skipped (worst, spread: −0.3%); the prototype with nothing switched on is within 0.6% of `dsp::Voice`.
- **16 notes with every oscillator at Voices 0 cost nothing**: the allocator frees each voice at its first check (`isActive()` false from the note-on).

---

## 2026-11-22: M1.4 mixer, as built (Mix in `dsp::Voice`, `MainVolume`, `PeakMeter`, `OutputGuard`)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). The "Mixer as built: CPU" benchmarks (`[mixer]`), 100 samples per benchmark, three runs; the table gives each row's median.
- **What is measured** ([mixer.md](../dsp/mixer.md#results-as-built)): `dsp::Voice` with its Mix ramps (`VoiceKnobs::mix`) still and retargeted every block; 16 voices through `VoiceAllocator` without Mix ramps and with Mix, `MainVolume`, `StereoPeakMeter` and `OutputGuard` after them; each output stage on a stereo block.

| Benchmark (ns / sample) | this commit |
|---|---:|
| `dsp::Voice`, init, Mix still / ramping | 359.88 / 360.82 |
| 16 voices, init: `VoiceAllocator<dsp::Voice>` without Mix ramps / with Mix and the output chain | 5,803.77 / 5,822.50 |
| `MainVolume`, ramping / settled (each including the block copy below) | 1.73 / 0.21 |
| copying the stereo block | 0.10 |
| `StereoPeakMeter` | 1.66 |
| `OutputGuard` (stereo) | 0.43 |

### Observations

- **Mix costs nothing measurable:** ramping 0.3% over still per voice; 16 voices with the whole output chain 0.3% over the voices alone.
- **The output stages together are 2.3 ns per sample while Main Volume ramps, 0.6 settled**, under 0.7% of one voice.
- **Noise:** `dsp::Voice` reads 359.9 here against 349.5 in the stage-1 entry (another session); the spread between sessions for the same code is 349.5 to 367.6.

---

## 2026-11-22: M1.4 mixer, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[mixer]"`, 100 samples per benchmark, three runs; the table gives each row's median.
- **What is measured** ([mixer.md](../dsp/mixer.md#cpu)): the stage-1 `MixVoice` (`dsp::Voice` with a Mix gain per part) and `MixEngine` (the real `VoiceAllocator`, the Mix ramps, part buses, Main Volume, meters), in tests/dsp/voice/mixer_candidates.hpp. *Ramping*: the Mix knob retargeted every block, moving 480 of 512 samples; *LFO*: a free 40 Hz LFO of ±5 on Mix, its `sin` computed per sample in the prototype.

| Benchmark (ns / sample) | this commit |
|---|---:|
| `dsp::Voice`, init | 349.54 |
| `MixVoice`, init, grid held: still / ramping / LFO | 345.03 / 344.72 / 350.25 |
| `MixVoice`, init, grid ramped: still / ramping / LFO | 346.16 / 346.62 / 362.88 |
| `MixVoice`, init, per sample: still / ramping / LFO | 347.51 / 348.36 / 362.66 |
| `MixVoice`, init, part bus (the voice only) | 363.65 |
| 16 voices, init: `VoiceAllocator<dsp::Voice>` / engine per sample / engine part bus | 5,584.49 / 5,621.00 / 5,587.95 |
| 16 voices, worst case: the same three | 10,709.08 / 10,653.30 / 10,621.17 |
| Main Volume (stereo), ramping / settled | 0.94 / 0.10 |
| meters (stereo): sample peak / block peak / RMS window / RMS exponential / VU / PPM | 1.61 / 2.45 / 8.50 / 4.57 / 8.36 / 5.34 |
| output guard (stereo) | 0.28 |
| `MixVoice`, A1 and B1: Mix B 10 / Mix B 0 / B1 at Voices 0 | 453.02 / 460.53 / 346.88 |

### Observations

- **A per-voice Mix costs nothing measurable** while the knob is still or ramping (347.5–348.4 against 349.5); sixteen voices through the engine are within noise of the library, per sample or on part buses.
- **With a 40 Hz LFO, per sample and grid ramped both cost about 4% more**, mostly the prototype's `sin` per sample, which M1.5's LFOs compute anyway.
- **Mix B at 0 saves nothing** (460.5 against 453.0): skipping a silent part is task 5's to claim, about 110 ns per voice here.
- **Noise:** `dsp::Voice` reads 349.5 here against 367.6 in the previous entry; the part-bus voice row ranged 345–383 over the runs.

---

## 2026-11-21: M1.4 legato and portamento, as built

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[legato]"`, 100 samples per benchmark, three runs; each row's median.
- **What changed** ([legato-portamento.md](../dsp/legato-portamento.md#as-built)): `dsp::Voice` has a pitch input (a note per sample to the slots while the pitch moves, once otherwise), and `VoiceAllocator` Mono Legato, portamento, the bend and the pedal. The `[legato]` benchmarks add the built rows; the prototype's stay. One voice per row.

| Benchmark (ns / sample) | this commit |
|---|---:|
| `dsp::Voice`, init: constant pitch / gliding / the wheel every 96 samples | 369.23 / 383.48 / 367.21 |
| `dsp::Voice`, worst case: constant / gliding | 743.65 / 843.63 |
| `VoiceAllocator` Mono Legato, init: no events / 64 legato notes / 64 with a glide / 64 pedal toggles per block | 361.85 / 388.86 / 389.49 / 381.00 |
| `VoiceAllocator`, 64 poly note events with glides per block, no oscillators | 156.98 |
| the prototype, the same rows (per sample) | 368.32 / 387.34 / 375.50; 849.80; 359.84 / 389.99 / 386.79 / 377.82; 161.50 (`Engine`) |

### Observations

- **A constant pitch costs nothing** (369.23 against 368.32): the voice uses its old path when the pitch doesn't move.
- **A moving pitch costs 4% (init) to 13% (worst case)**, the slots' per-sample note conversion, as stage 1 measured.
- **Per event:** a legato note about 216 ns, a pedal toggle about 153, a poly note-on from silence 1.26 µs, within noise of the prototype.
- **Noise:** one run read 1,252 ns for `dsp::Voice`'s worst case against 743 in the other two.

---

## 2026-11-21: M1.4 legato and portamento, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[legato]"`, 100 samples per benchmark, three runs; the table gives each row's median.
- **What is measured** ([legato-portamento.md](../dsp/legato-portamento.md#cpu)): the stage-1 `PitchedVoice` (`dsp::Voice` with a pitch input) and `Engine` (the decided allocator with a mono mode, the pedal and the glide's bookkeeping), in tests/dsp/voice/legato_candidates.hpp. One voice per row. *Gliding* is three octaves a second, so the pitch changes every sample; *the wheel* is a bend message every 96 samples with a 240-sample ramp.

| Benchmark (ns / sample) | this commit |
|---|---:|
| `dsp::Voice`, init / worst case | 367.64 / 746.87 |
| `PitchedVoice`, init, constant pitch: grid held / grid ramped / per sample | 366.19 / 371.16 / 374.55 |
| `PitchedVoice`, init, gliding: grid held / grid ramped / per sample | 381.96 / 383.20 / 386.11 |
| `PitchedVoice`, init, the wheel: grid held / grid ramped / per sample | 367.07 / 373.29 / 375.50 |
| `PitchedVoice`, worst case, gliding: grid held / grid ramped / per sample | 796.98 / 843.08 / 843.83 |
| Mono Legato, init: no events / 64 legato notes / 64 with a glide / 64 pedal toggles per block | 363.52 / 386.77 / 385.08 / 378.95 |
| poly, 64 note events per block on voices with no oscillators: `Engine` / `VoiceAllocator<dsp::Voice>` (one run) | 158.33 / 153.60 |

### Observations

- **The pitch input costs nothing at a constant pitch** on the held path (366.19 against 367.64); the per-sample path costs 1.9% more there, so the voice should switch to it only while the pitch moves (the two are identical to the bit at a constant pitch).
- **A moving pitch per sample costs 5% (init) to 13% (worst case)**, against 4% and 7% held per grid point; ramped per grid point costs what per sample does. The cost is the slot's note conversion per sample and the 32 unison voices' frequency sets.
- **Per event:** a legato note about 180 ns, a pedal toggle about 120 ns (mostly splitting the render); a poly note-on from silence 1.27 µs against the library's 1.23, so about 38 ns of glide and pedal bookkeeping.
- **Noise:** one row read +130% and another +40% in one run of five, neither reproduced; hence medians.

---

## 2026-11-21: M1.4 voice allocation, as built (`VoiceAllocator`)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice-allocation]"` then `tools/bench.sh "[voice]"`, 100 samples per benchmark, one run.
- **What changed** ([voice-allocation.md](../dsp/voice-allocation.md#as-built-the-allocator)): new `dsp::VoiceAllocator` (voice_allocator.hpp), the decided allocator: a pool of 16, the oldest released note stolen first, a retrigger in place, freeing at events and each voice's 32-sample points. `VoiceFilterKnobs` gained an offset; `dsp::Voice` gained the readout's counts. The `[voice-allocation]` benchmarks add five library rows; the prototype's rows stay for the alternatives. The per-sample figures are for all 16 voices.

| Benchmark (ns / sample) | this commit |
|---|---:|
| allocator (library): 16 voices held, init patch | 5,754.02 (27.6% of a core; 359.6 per voice) |
| allocator (library): 16 voices held, worst case | 11,213.07 (53.8%; 700.8 per voice) |
| allocator (library): a steal per block, init patch | 6,192.34 |
| allocator (library): 64 note events (32 steals) per block on null voices / no events | 5.22 / 0.60 |
| allocation (prototype): 16 held init / worst / steal (retrigger) / events / no events | 5,776.86 / 11,235.25 / 6,199.47 / 21.27 / 2.09 |
| voice: built, init patch / filter B at `--` / worst case (one voice) | 356.25 / 233.99 / 683.42 |

### Observations

- **About 37 ns per note event** ((2,671 − 308) ns / 64 events per block), against the prototype's 157: a 16-voice pool and no policy switches. Without events, 0.3 µs per block of bookkeeping.
- **The voices are the cost**: 16 through the allocator cost 16 × one voice within noise (359.6 and 700.8 per voice against 356.25 and 683.42 alone, +1% and +2.5%). The library and the prototype allocator are within 0.4% of each other on every voice row.

---

## 2026-11-21: M1.4 voice: a note-on on a sounding voice, stop(), isActive()

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice-allocation]"` then `tools/bench.sh "[voice]"`, 100 samples per benchmark, one run.
- **What changed** ([voice-allocation.md](../dsp/voice-allocation.md#as-built-the-voices-part)): `dsp::Voice::noteOn` keeps the filters and grid on a sounding voice; new `stop()`; `isActive()` counts only what can be heard (4 slot reads per filter). The allocator calls `isActive()` at every event and every 32 samples per voice.

| Benchmark (ns / sample) | this commit | stage 1 (below) |
|---|---:|---:|
| voice: built, init patch / filter B at `--` / worst case (one voice) | 359.52 / 235.37 / 685.31 | 360.90 / 240.29 / 700.12 |
| allocation: 16 voices held, init patch / worst case | 5,808.52 / 11,245.88 | 5,775.35 / 11,216.15 |
| allocation: 64 note events per block on silent voices / no events | 21.29 / 2.06 | 21.57 / 1.95 |
| allocation: a steal per block, init: cut / retrigger | 6,252.36 / 6,215.78 | 6,185.41 / 6,202.34 |
| allocation: a steal per block, init: 5 ms fade on a spare / then the note | 6,346.05 / 6,188.32 | 6,365.02 / 6,168.16 |

### Observations

- **No measurable change**: every row is within 1.1% of stage 1's (the single voice −0.4% to −2.1%, the allocator +0.3% to +1.1%), which is the run-to-run noise here.

---

## 2026-11-21: M1.4 voice allocation, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice-allocation]"`, 100 samples per benchmark, one run, followed by `tools/bench.sh "[voice]"` for the built voice's rows.
- **What is measured** ([voice-allocation.md](../dsp/voice-allocation.md#cpu)): the stage-1 `Allocator` (tests/dsp/voice/allocation_candidates.hpp) driving `dsp::Voice`, 512 samples per run at 48 kHz, stereo. The per-sample figures are for **all 16 voices** (the share-of-a-core column is the whole instrument's voices), except where a row says otherwise. *Init*: A1 one saw, both filters LP24 on their own parts. *Worst*: 4 oscillators × 8 unison saws, both filters on AB, drive ×4. The bookkeeping rows use a stub voice that renders nothing.

| Benchmark (ns / sample) | this commit |
|---|---:|
| allocation: 16 voices held, init patch | 5,775.35 (361 per voice; 27.7% of a core) |
| allocation: 16 voices held, worst case | 11,216.15 (701 per voice; 53.8%) |
| allocation: 64 note events (32 steals) per block on silent voices | 21.57 (11.0 µs per block) |
| allocation: no events on silent voices | 1.95 (1.0 µs per block) |
| allocation: a steal per block, init, cut / retrigger | 6,185.41 / 6,202.34 |
| allocation: a steal per block, init, 5 ms fade on a spare / 5 ms fade then the note | 6,365.02 / 6,168.16 |
| voice: built, init patch / filter B at `--` / worst case (one voice, the same session) | 360.90 / 240.29 / 700.12 |

### Observations

- **The allocator adds nothing measurable to the voices**: 16 voices through it cost 16 × one voice within noise (361 and 701 ns per voice against 361 and 700 for one voice in the same session). The full instrument's voices at 48 kHz: 27.7% of a core for the init patch, 53.8% for the worst case.
- **The bookkeeping is about 157 ns per note event** ((11,045 − 999) ns / 64 events per block), a linear scan of a 32-voice pool, 0.3 samples' worth at 48 kHz.
- **A fade on a spare voice costs 2.9% over a cut** at one steal in every block (an extreme rate): the spare renders for 5 ms of each 10.7 ms block. A retrigger or a delayed fade costs what a cut does.
- The `[voice]` rows are within 1–5% of the last entry (360.90 against 357.76, 240.29 against 229.20, 700.12 against 688.03): noise, as no library code changed.

---

## 2026-11-21: M1.4 voice, as built

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice]"`, 100 samples per benchmark, one run, with the stage-1 rows in the same session.
- **What changed** ([voice-architecture.md](../dsp/voice-architecture.md#as-built)): new `dsp::Voice` (voice.hpp), the decided structure: per-filter buffers on the voice's 32-sample grid, fixed scratch, each running filter fed through the Filter Control. The `[voice]` benchmarks add three rows on it; the prototype's rows stay for the alternatives.

| Benchmark (ns / sample, per voice) | this commit | stage 1 (prototype) |
|---|---:|---:|
| voice: built, init patch | 357.76 (16 voices 27.5%) | 359.98 (per-filter, grid, this session) |
| voice: built, init patch, filter B at `--` | 229.20 | 234.42 |
| voice: built, worst case | 688.03 (16 voices 52.8%) | 696.88 (per-filter, grid, this session) |

### Observations

- **The built voice costs what the prototype's per-filter grid engine did**, within noise (−0.6% to −2.2%). The decision was made on structure, not speed, and the build kept the speed.
- The prototype's rows in this session are within 3% of stage 1's (above).

---

## 2026-11-21: M1.4 voice, stage 1 (prototypes)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[voice]"`, 100 samples per benchmark, two runs; the second shown, the first within 3% on every row.
- **What is measured** ([voice-architecture.md](../dsp/voice-architecture.md#cpu)): the stage-1 `PrototypeVoice` (tests/dsp/voice/voice_candidates.hpp), one voice with its note held at the sustain, 512 samples per run at 48 kHz, both filters fed through the Filter Control at the voice's grid points. *Init*: A1 one saw, both filters LP24 on their own parts (4,677 Hz, k 0, drive ×1), Warm on. *Worst*: 4 oscillators × 8 unison saws (detuned, Stereo 10), both filters on AB, drive ×4, Warm on. The per-sample figures are **per voice**, so the share-of-a-core column below is for one voice; ×16 for full polyphony.

| Benchmark (ns / sample, per voice) | this commit |
|---|---:|
| voice: init patch, per-part, grid, stereo (filter B running on part B's silence) | 360.64 (1.73% of a core; 16 voices 27.7%) |
| voice: init patch, filter B at `--` (not run) | 235.08 |
| voice: init patch, mono path | 185.75 |
| voice: init patch, per-filter, grid / per-part, block | 357.51 / 361.27 |
| voice: worst case, per-part, grid / per-filter, grid | 691.22 / 688.54 (16 voices 53%) |
| voice: worst case, per-part, block / per-filter, block | 681.59 / 681.15 |
| voice: worst case centred (Stereo 0), stereo path / mono path | 702.51 / 410.48 |
| voice: init patch at 96 kHz (1024 samples per 512 out, no decimator) | 666.82 |

### Observations

- **The filters are the voice**: four filter channels at 95–125 ns each against about 3 ns per saw. Layouts and schedules are within noise of each other (−1.5% to +0.3%).
- **A filter on silence still costs more than half a sounding one**: 126 ns per sample for its two channels (filter A, sounding, about 225), 35% of the init voice. Task 5's skipping must cover filters whose input has nothing sounding.
- **The mono path** (exact while every voice is centred) saves 49% at the init patch and 42% on the worst case centred.
- **The voice at 2×** costs 1.85× per output sample before any decimator.
- No library code changed in this commit (the test stand-in's feed was factored into `feedFilter`, no behaviour change), so the `[filter]` and `[filter-control]` rows below stand.

---

## 2026-11-21: M1.3 Filter Control

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[filter-control]"` and `"[filter]"`, 100 samples per benchmark. The previous column is commit 9dec978 in the same session: for `[filter]`, built in a separate worktree and alternating with this commit, two runs each (means); for `[filter-control]`, one run of each.
- **What changed** ([filter-control.md](../dsp/filter-control.md#as-built)): new `filter_control.hpp` (the combining law, `constexpr`, no state), `LinearSmoother::valueAfter` (a ramp read ahead without advancing), and `LadderFilter::setResonanceModulation` (k added at the grid, interpolated like g, the sum capped at 64), which adds a few float operations per sample to the loop that already computes k. The `[filter-control]` rows now measure the built law (stage 1's (a2) and resonance prototype rows replaced), plus the voice's whole feed and the filter with k fed through the new input.

| Benchmark (ns / sample) | this commit | previous |
|---|---:|---:|
| filter-control: `filterCutoffOctaves` with keytrack, 2 filters × 16 grid points per 512 | 0.08 (40 ns per block) | 0.09 (48, the (a2) prototype) |
| filter-control: `filterResonanceK`, 2 filters × 16 grid points per 512 | 0.04 (21 ns per block) | 0.04 (21, the prototype) |
| filter-control: the voice's feed (6 ramps read with `valueAfter`, keytrack, 2 filters' cutoff and k), 16 grid points per 512 | 0.17 (87 ns per block) | |
| filter-control: filter fed W1 (the knobs via `setCutoff`) / W2 (the whole sum as modulation) | 88.54 / 93.76 | 92.91 / 87.67 |
| filter-control: filter fed W2, k through `setResonanceModulation` (built) | 89.21 | |
| filter: Warm on (tanh), k 2, drive 2 / k 6, drive 16 | 94.83 / 123.38 | 97.28 / 122.16 |
| filter: Warm off (cubic), k 2, drive 2 / k 6, drive 16 | 63.02 / 84.41 | 68.75 / 80.44 |
| filter: stereo (2 channels), Warm on / off | 193.49 / 129.24 | 188.89 / 127.60 |
| filter: a cutoff ramp every block, Warm on / off | 93.06 / 66.97 | 92.83 / 63.39 |
| filter: modulation every 32 samples, Warm on / off | 93.92 / 63.53 | 93.15 / 65.37 |
| filter: 2× with the Oversampler, Warm on / off | 197.36 / 142.07 | 201.45 / 136.70 |
| filter: 4× with the Oversampler, Warm on / off | 387.09 / 261.47 | 382.73 / 265.31 |
| filter: bypassed | 0.06 | 0.05 |
| filter: 16 voices × 2 stereo filters (64 channels), Warm on | 6,024.73 (28.9% of a core) | 5,995.51 |

### Observations

- **The Filter Control costs 0.17 ns per sample per voice**: at each of a voice's grid points, six knob ramps read in closed form, keytrack once, and both filters' cutoff and k. The filter runs at 63–123 ns per channel-sample, so this is under 0.3% of one filter.
- **The resonance modulation input costs nothing measurable**: every `[filter]` row is within −8.3% to +5.7% of the previous commit's, a mean of +0.9%, inside the spread of the runs themselves (the previous commit's own two runs differ by up to 6.5%); the bypass row is 0.05–0.06 ns either way. Its few operations sit in the per-segment control loop beside k's ramp, not in the per-channel solve. Fed through it, the filter is within noise of W1 and W2.

---

## 2026-11-15: M1.3 filter stability

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[filter-stability]"` and `"[filter]"`, 100 samples per benchmark. The previous column is commit 26cd4ec built in a separate worktree and run in the same session, alternating with this commit, four runs each (means).
- **What changed** ([filter-stability.md](../dsp/filter-stability.md#as-built)): `LadderFilter`'s solve moved to `LadderSolver` (`filters/ladder_solver.hpp`), with a nested bracketing fallback where Newton does not converge in 8 iterations, and non-finite input taken as 0. The `[filter-stability]` rows are new: they replace stage 1's candidate rows (commit 26cd4ec) with `LadderFilter` itself at the normal settings, at the corner where every sample falls back, at F3's top within the knobs' ranges, and at the DSP's ceiling of k.

| Benchmark (ns / sample) | this commit | previous |
|---|---:|---:|
| stability: normal (1 kHz, k 2, drive 2), Warm on / off | 98.13 / 65.91 | |
| stability: **corner** (0.495 fs, k 8, self-oscillating), Warm on / off | **1,900.10 / 1,022.00** (9.1% / 4.9% of a core) | |
| stability: F3's top (0.465 fs, k 6, drive 16, full-scale saw), Warm on / off | 411.09 / 232.03 | |
| stability: ceiling (0.30 fs, k 64, drive 32, full-scale saw), Warm on / off | 316.46 / 197.78 | |
| filter: Warm on (tanh), k 2, drive 2 / k 6, drive 16 | 96.69 / 126.99 | 97.92 / 124.01 |
| filter: Warm off (cubic), k 2, drive 2 / k 6, drive 16 | 64.51 / 83.09 | 65.40 / 81.68 |
| filter: stereo (2 channels), Warm on / off | 193.49 / 126.55 | 193.51 / 126.99 |
| filter: a cutoff ramp every block, Warm on / off | 96.97 / 64.79 | 95.38 / 61.99 |
| filter: modulation every 32 samples, Warm on / off | 97.07 / 64.52 | 93.85 / 65.66 |
| filter: 2× with the Oversampler, Warm on / off | 204.85 / 140.90 | 200.36 / 140.85 |
| filter: 4× with the Oversampler, Warm on / off | 391.96 / 272.41 | 390.49 / 271.24 |
| filter: bypassed | 0.06 | 0.06 |
| filter: 16 voices × 2 stereo filters (64 channels), Warm on | 6,168.15 (29.6% of a core) | 6,130.06 |

### Observations

- **The ordinary case is unchanged within noise**: every `[filter]` row is within −1.7% to +4.5% of the previous commit's, a mean of +0.9%, inside the spread of the runs themselves (the previous commit's own runs span up to 7%). That took a structure: with the fallback's call inside the per-sample solve the filter was 9% (tanh) and 12–17% (cubic) slower, because a function with any call in it, taken or not, saves and restores the callee-saved registers on every sample. As built, Newton's function calls nothing and returns by value, and the filter's run calls the fallback.
- **The corner costs 1.90 µs (tanh) and 1.02 µs (cubic) per channel-sample**, where the solver before failed at 0.22 µs. Of that, 0.2 µs is Newton's 8 iterations before the fallback (the price of bit-identity), and the rest is about 57 stage evaluations of about 28 ns. The target was about 1 µs and the ceiling 2 µs. Stage 1's prototype took 2.07 and 1.04 µs with the same trigger.
- A loud saw at F3's top within the knobs' ranges costs 4.2× (tanh) and 3.5× (cubic) the ordinary case: there the fallback runs on part of the samples.

---

## 2026-11-15: M1.3 keytracking

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[keytrack]"`, 100 samples per benchmark.
- **What changed** ([keytracking.md](../dsp/keytracking.md)): new `keytrackOctaves`, `keytrackSource` (`constexpr`, no state), and `masterTuneSemitones`. `LadderFilter` is unchanged. New rows: each function at the 16 grid points of a 512-sample block, as a voice feeds them; the master tune once per block; the filter fed at each grid point against once per block.

| Benchmark (ns / sample) | this commit | previous |
|---|---:|---:|
| keytrack: `keytrackOctaves`, 16 grid points per 512 | 0.01 (5 ns per block) | |
| keytrack: `keytrackSource`, 16 grid points per 512 | 0.01 (5 ns per block) | |
| keytrack: `masterTuneSemitones`, once per block | 0.01 (4 ns per block) | |
| keytrack: filter (Warm on, k 2) fed once per block | 91.94 | |
| keytrack: filter (Warm on, k 2) fed at each grid point | 90.45 | |

### Observations

- **Keytracking costs nothing measurable**: 0.3 ns per grid point, and feeding the filter at every grid point is within noise of feeding it once per block (90.5 against 91.9 ns; the filter alone measures 91–98 ns across runs). The candidates' figures (commit 3948d99) put the sounding-pitch prototype at 4 ns per call, its master tune `log2` per call; built, that `log2` runs once per block.

---

## 2026-11-15: M1.3 filter

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[filter]"` and `"[oversampling]"`, 100 samples per benchmark.
- **What changed** ([filters.md](../dsp/filters.md)): a new `LadderFilter` (a Newton-solved ZDF transistor ladder, tanh with Warm Drive on and a cubic soft clip off, g every 32 samples). The `Oversampler`'s halfbands now flush their outputs per sample and their states once per block, so the `[oversampling]` rows are re-measured: unchanged within noise. (A first version that flushed inside the allpass chain measured 24.0 and 43.9 ns: +50–57%.) New rows: one channel at typical and heavy settings, stereo, a cutoff ramp, modulation every 32 samples, 2× and 4× with the `Oversampler`, bypass, and a 16-voice synth's 32 stereo filters.

| Benchmark (ns / sample) | this commit | previous |
|---|---:|---:|
| filter: Warm on (tanh), k 2, drive 2 / k 6, drive 16 | 98.26 / 126.08 | |
| filter: Warm off (cubic), k 2, drive 2 / k 6, drive 16 | 63.50 / 81.81 | |
| filter: stereo (2 channels), Warm on / off | 191.58 / 126.28 | |
| filter: a cutoff ramp every block, Warm on / off | 95.37 / 64.29 | |
| filter: modulation every 32 samples, Warm on / off | 92.70 / 64.15 | |
| filter: 2× with the Oversampler, Warm on / off | 201.46 / 139.42 | |
| filter: 4× with the Oversampler, Warm on / off | 389.14 / 266.23 | |
| filter: bypassed | 0.06 | |
| filter: 16 voices × 2 stereo filters (64 channels), Warm on | 6,046.70 (29.0% of a core) | |
| Oversampler 2× up+down | 15.71 | 16.71 |
| Oversampler 4× up+down | 26.25 | 26.78 |

### Observations

- **A filter channel costs 98 ns (Warm on) or 64 ns (Warm off) per sample**, the Newton solve's serial chain of divisions and saturators. A full 16-voice patch with both parts' filters in stereo is 29% of a core: the most expensive section so far, and the first Phase 3 target (SIMD across the channels, which interleaving four already speeds 1.3–3× in the candidate benchmarks).
- **Warm off saves 35%** (the cubic has no division), as the manual says it should save CPU.
- The cutoff ramp and grid-rate modulation are free (within noise of settled): `tan` runs once per 32 samples. Bypass costs nothing.

---

## 2026-11-14: M1.2 tempo sync

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[sync]"` and `"[lfo]"`, 100 samples per benchmark.
- **What changed** ([tempo-sync.md](../dsp/tempo-sync.md)): new `NoteValue` conversions and `HostTempo`, and `Lfo::setPhaseIncrement()`. `Lfo::updateRate()` gained a branch, so the `[lfo]` rows are re-measured: unchanged within noise. New rows: one conversion; a synced LFO with the tempo reported and the increment set every block, in blocks of 512 and of 32; a 16-voice synth's 32 synced LFOs.

| Benchmark (ns / sample) | this commit | previous |
|---|---:|---:|
| sync: `notePhaseIncrement()`, one call (ns) | 3.4 | |
| sync: sine 1/8D, blocks of 512 / of 32 | 0.72 / 1.69 | |
| sync: saw 1/8D, blocks of 512 / of 32 | 0.30 / 1.35 | |
| sync: 32 synced LFOs (16 voices × 2), blocks of 512 | 13.56 | |
| LFO: sine 3 Hz, `process()` | 0.59 | 0.62 |
| LFO: saw / triangle / pulse 3 Hz, `process()` | 0.19 / 0.19 / 0.20 | 0.19 / 0.19 / 0.20 |
| LFO: 32 LFOs (16 voices × 2), sine and saw, `process()` | 13.21 | 12.83 |

### Observations

- **Sync costs about 0.1 ns per sample per LFO** at blocks of 512: one conversion (3.4 ns) and one `setPhaseIncrement` per slot per block. 32 synced LFOs cost 13.6 ns per sample, 0.07% of a core.
- At blocks of 32 a synced LFO costs 1.4–1.7 ns per sample, mostly `Lfo::process()`'s setup per call, which an unsynced LFO in blocks of 32 pays too.

---

## 2026-11-14: M1.2 LFO

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[lfo]"`, 100 samples per benchmark.
- **What changed** ([lfos.md](../dsp/lfos.md)): a new `Lfo` (a 32.32 fixed-point phase; piecewise-linear rows exact in runs from corner to corner; curved rows interpolated linearly every 32 samples; jumps slewed over 1 ms, at most 1/16 cycle; gain and offset ramped over 10 ms). No existing code changed, so no existing row is re-measured. New rows: one 512-sample block per row at 3 Hz through `process()`, the sine and saw through `next()` per sample, a 1 kHz saw (an edge and a slew every 48 samples), a sine with its gain ramping, and a 16-voice synth's 32 LFOs.

| Benchmark (ns / sample) | this commit |
|---|---:|
| LFO: sine 3 Hz, `process()` | 0.62 |
| LFO: saw / triangle / pulse 3 Hz, `process()` | 0.19 / 0.19 / 0.20 |
| LFO: S&H 3 Hz, `process()` | 0.10 |
| LFO: smooth / Lorenz 3 Hz, `process()` | 0.54 / 0.65 |
| LFO: sine / saw 3 Hz, `next()` per sample | 6.18 / 6.92 |
| LFO: saw 1 kHz, `process()` | 0.50 |
| LFO: sine 3 Hz with its gain ramping, `process()` | 1.53 |
| LFO: 32 LFOs (16 voices × 2), sine and saw, `process()` | 12.83 (0.40 per LFO) |

### Observations

- **A 16-voice synth's 32 LFOs cost 12.8 ns per sample, 0.06% of a core**: half the 64 envelopes, and a tenth of the per-sample prototypes (140 ns, measured at `dae814e`; [lfos.md](../dsp/lfos.md#cpu)).
- **The exact rows are the cheapest**: a run of a straight segment, or of a held value, is one loop with no per-sample branch. The curved rows add a `sin`, a smoothstep, or Lorenz steps once per 32 samples.
- **`next()` costs 6–7 ns**: it is `process()` of one sample, so it pays the run setup every call. Blocks should go through `process()`.
- A ramping gain costs about 1 ns per sample for its 10 ms (the per-sample smoother path).

---

## 2026-11-14: M1.2 ADSR envelope

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[envelope]"`, 100 samples per benchmark.
- **What changed** ([envelopes.md](../dsp/envelopes.md)): a new `Adsr` (exponential segments towards an overshoot target, counted in float as a closed form through `fastExp2`). No existing code changed, so no existing row is re-measured. New rows: one 512-sample block in each stage, through `process()` (a run of a stage at a time) and through `next()` per sample, and 64 envelopes (16 voices × 4) a quarter in each stage.

| Benchmark (ns / sample) | this commit |
|---|---:|
| Envelope: attack, `process()` / `next()` per sample | 0.52 / 2.65 |
| Envelope: decay, `process()` / `next()` per sample | 0.49 / 2.69 |
| Envelope: sustain, `process()` / `next()` per sample | 0.08 / 1.27 |
| Envelope: release, `process()` / `next()` per sample | 0.52 / 2.75 |
| Envelope: 64 envelopes (16 voices × 4), a quarter in each stage, `process()` | 25.25 (0.39 per envelope) |

### Observations

- **`process()` vectorizes a stage: 0.5 ns per sample**, 5× faster than `next()`. Each sample of a stage is computed from its own count, with no recursion, so clang runs `fastExp2` across lanes. The two give bit-identical output (tested).
- **A 16-voice synth's 64 envelopes cost 25 ns per sample, 0.12% of a core**, a quarter of one note's 32 oscillator voices (about 100 ns).
- The candidate models' costs (per sample through `next()`, measured at `c8c1ab1`) are in [envelopes.md](../dsp/envelopes.md#cpu). The counted exponential measured 2.8 ns there, as `next()` does here.

---

## 2026-11-14: M1.1 oscillator drift

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark.
- **What changed** ([oscillator-drift.md](../dsp/oscillator-drift.md)): a new `PitchDrift` generator (two-pole noise, updated every 64 samples). Nothing in the oscillators or the slot changed. New rows: the generator alone, and drift applied to a voice's slots, held per 64 samples (the default) or added to a note per sample.
- **Before and after, same session:** the parent (`264f362`, the measurement commit, whose library code is identical) and then this tree. Every existing row is within −10% to +10% of the parent (the run-to-run noise of this machine), except one: "noteToFrequency every sample + inline stereo gain" read 23.43 on the parent against 9.21 now; earlier sessions measured it at 9.15–9.51, so the parent's reading is the outlier.

| Benchmark (ns / sample) | parent | this commit |
|---|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.17 | 2.92 |
| minBLEP 64-tap, TriSaw 12 kHz | 17.80 | 17.97 |
| Oscillator slot, saw 440 Hz, settled | 3.28 | 3.31 |
| Oscillator slot, saw 440 Hz, pitch per sample | 6.41 | 6.45 |
| Unison slot, saw 440 Hz, 8 voices | 25.21 | 24.78 |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample | 40.12 | 38.61 |
| Unison: 32-voice note (4 slots x 8 voices) into one bus | 102.89 | 104.01 |

New rows (drift at the default, 1.5 cents and 0.5 Hz; saws at 110–880 Hz, Detune 0.2 st, Stereo 0.5):

| Benchmark (ns / sample) | this commit |
|---|---:|
| Drift: the generator alone, `process()` | 0.17 |
| Drift: the generator alone, `addTo()` a note per sample | 0.19 |
| Drift: 1 slot x 1 voice: no drift / held per 64 / a note per sample | 2.99 / 3.50 / 6.52 |
| Drift: 1 slot x 8 voices: no drift / held per 64 / a note per sample | 23.44 / 23.93 / 35.44 |
| Drift: 4 slots x 8 voices: no drift / held per 64 / a note per sample | 103.48 / 101.45 / 147.22 |

### Observations

- **Held per 64 samples, a whole note costs what it did without drift** (101.5 against 103.5 ns, within noise). The generator is 0.17 ns per sample, and each slot converts its note 8 times per 512 samples instead of once.
- **A lone one-voice slot pays +17% held:** 8 process calls of 64 samples instead of one of 512, on an oscillator of 3 ns per sample.
- **A note per sample costs a whole note +42%,** for the `exp2` per sample per slot. A voice with per-sample glide or pitch modulation pays that anyway; the drift adds 0.19 ns.
- The six candidate models' costs (measured at `264f362`) are in [oscillator-drift.md](../dsp/oscillator-drift.md#cpu).

---

## 2026-11-14: M1.1 oscillator range (the Nyquist fade, pitch steps)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark.
- **What changed** ([oscillator-range.md](../dsp/oscillator-range.md)):
  - `MinBlepOscillator` fades its output about the waveform's mean from 0.45 fs, through one predictable branch per sample (`finishing`) that is false below 0.45 fs once an onset has settled.
  - `setIncrement` checks the size of the change: a pitch step of 6.1·10⁻⁵ cycles or more adds one ramp residual, at the next sample's corner scan.
  - `addResidual()` adds in two contiguous runs instead of through a masked ring index, so clang vectorises it.
  - New rows: a saw in the fade band (0.47 fs) and above Nyquist (0.6 fs); a 6 Hz vibrato set every sample; a pitch step every 64 samples; a sweep of 10 octaves in one block (a step on most samples).
- **Before and after, same session:** the parent (`9fef36a`), this tree, the parent again (exported with `git archive`, `external/` and `.cache/` symlinked, built `relwithdebinfo`), then this tree twice more. The parent was also run once with this commit's benchmark file, for the new rows. The threshold was first 8 times lower; the rows marked † were measured with it, before the project owner chose the final threshold.

| Benchmark (ns / sample) | parent (two runs) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.03, 3.06 | 2.90, 2.90 | −5% to −4% |
| minBLEP 64-tap, triangle 440 Hz | 3.74, 3.44 | 3.31, 3.31 | −11% to −4% |
| minBLEP 64-tap, sine 440 Hz | 8.69, 8.45 | 8.54, 8.33 | −4% to +1% |
| minBLEP 64-tap, saw 5 kHz | 7.15, 7.52 | 5.38, 4.85 | −36% to −25% |
| minBLEP 64-tap, saw 12 kHz | 12.43, 12.61 | 8.38, 7.94 | −37% to −33% |
| minBLEP 64-tap, square 12 kHz | 23.93, 23.77 | 15.30, 13.87 | −42% to −36% |
| minBLEP 64-tap, triangle 12 kHz | 24.20, 24.40 | 14.10, 13.98 | −43% to −42% |
| minBLEP 64-tap, TriSaw 12 kHz | 33.84, 33.85 | 18.74, 19.23 | −45% to −43% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample (±1 st at 384 Hz) | 4.23, 4.22 | 4.36, 4.25 († 17.34, 16.86) | 0% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 96.33, 94.67 | 93.32, 91.04 | −5% to −1% |
| Oscillator slot, saw 440 Hz, settled | 3.22, 3.22 | 3.37, 3.23 | 0% to +5% |
| Oscillator slot, saw 440 Hz, pitch per sample | 6.31, 6.19 | 6.56, 6.47 († 19.25, 19.04) | +3% to +6% |
| Oscillator slot, TriSaw 12 kHz, settled | 33.44, 33.95 | 19.51, 18.78 | −45% to −42% |
| Unison slot, saw 440 Hz, 8 voices | 25.86, 25.94 | 25.46, 24.88 | −4% to −2% |
| Unison slot, saw 12 kHz, 8 voices | 107.76, 107.60 | 69.85, 68.72 | −36% to −35% |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample | 35.74, 36.08 | 41.13, 38.97 († 136.82, 134.79) | +8% to +15% |
| Unison: 32-voice note into one bus | 102.83, 101.18 | 104.27, 104.89 | +1% to +4% |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master | 4.77, 4.82 | 4.46, 4.58 | −7% to −4% |

New rows, with the parent measured once on the same benchmark file:

| Benchmark (ns / sample) | parent | this commit (two runs) |
|---|---:|---:|
| minBLEP 64-tap, saw at 0.47 fs (in the Nyquist fade) | 24.58 | 14.11, 14.15 |
| minBLEP 64-tap, saw at 0.6 fs (above Nyquist: its mean; the parent plays the clamped Nyquist tone) | 22.85 | 14.71, 13.99 |
| minBLEP 64-tap, saw 440 Hz, a 6 Hz vibrato set every sample | 4.90 | 4.77, 4.77 |
| minBLEP 64-tap, saw 440 Hz, a pitch step every 64 samples | 3.28 | 3.54, 3.45 |
| minBLEP 64-tap, saw, 20 Hz to 20 kHz in one block (a step on most samples) | 7.14 | 15.19, 15.23 |

### Observations

- **Constant pitch is unchanged below the fade band.** The fade costs one predictable branch, and the pitch-step check sits in `setIncrement`, outside the per-sample loop.
- **High notes are 25–45% faster**, at every waveform but the sine (which has no edges), because the residual add vectorises now. That is the "masked ring index" item from D6's Phase 3 list, done with two contiguous loops rather than a double-length buffer. Unison at 12 kHz gains the same: 8 voices 108 → 69 ns.
- **A pitch every sample costs about 0.4 ns more per voice** (+3 to +15%), for the change and fade checks in `setIncrement`. With the first, lower threshold, a fast vibrato crossed it on every sample and paid a residual there: the † figures.
- **A pitch step costs one residual when it happens:** +0.2 ns per sample at one step every 64 samples. A sweep that steps on most samples pays it almost every sample (7.1 → 15.2 ns).
- **A voice in the fade band or above Nyquist** costs what a Nyquist-rate saw does (14 ns), less than the parent's clamped tone (23–25 ns) thanks to the vectorised adds, although it outputs a constant. Skipping it is a Phase 3 item.
- `Unison slot, 0 voices` reads 0.14 against 0.13 ns per sample: 5 ns per block, the resolution of the measurement.

---

## 2026-11-14: M1.1 unison

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds unison (it reports its parent, `1aa1b42`, as modified).
- **What changed:** `OscillatorSlot` now holds 8 voices (a `MinBlepOscillator`, a `StereoGain`, a detune ratio, and a state each), one `WhiteNoise`, and three smoothers for the noise mix ([oscillator-unison.md](../dsp/oscillator-unison.md)). It is about 5 KB at 64 taps, against 680 bytes before. A block renders voice by voice per 64-sample chunk. With a pitch per sample, a chunk's centre frequencies are converted once, before the voices. `MinBlepOscillator` is unchanged. New rows: the slot with 1, 2, 4, and 8 voices (saw, Detune 0.2 st, Stereo 0.5, at 440 Hz and 12 kHz), 8 voices with a pitch per sample, 0 voices, noise at 8 voices, and a 32-voice note (4 slots of 8 voices, 110–880 Hz) into one bus.
- **Before and after, same session:** this tree, then the parent (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`), then this tree again; then the slot rows twice more, alternating.

| Benchmark | parent (ns / sample) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.08 | 3.05, 3.29 | −1% to +7% |
| minBLEP 64-tap, square 440 Hz | 3.50 | 3.63, 3.51 | 0% to +4% |
| minBLEP 64-tap, triangle 440 Hz | 3.59 | 3.60, 3.63 | 0% to +1% |
| minBLEP 64-tap, sine 440 Hz | 8.26 | 8.72, 8.36 | +1% to +6% |
| minBLEP 64-tap, saw 12 kHz | 12.53 | 13.03, 12.56 | 0% to +4% |
| minBLEP 64-tap, square 12 kHz | 23.61 | 22.83, 23.32 | −3% to −1% |
| minBLEP 64-tap, TriSaw 12 kHz | 33.34 | 32.01, 34.37 | −4% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 93.48 | 93.21, 92.45 | −1% to 0% |
| Oscillator slot, saw 440 Hz, settled | 3.29 (3.25, 3.64 later) | 3.28, 3.51 (3.52, 3.34 later) | within noise |
| Oscillator slot, saw 440 Hz, volume ramping | 18.13 (17.75, 18.53 later) | 4.65, 4.74 (5.06, 5.09 later) | a harness artefact in the parent; see below |
| Oscillator slot, saw 440 Hz, pitch per sample | 8.32 (8.37, 8.85 later) | 6.59, 6.12 (6.75, 6.98 later) | −21% to −26% |
| Oscillator slot, TriSaw 12 kHz, settled | 34.31 (38.47, 36.28 later) | 32.87, 34.77 (36.28, 34.65 later) | within noise |
| Oscillator slot, 32 saws into one bus (32 one-voice slots) | 119.17 (120.59, 120.37 later) | 105.08, 106.49 (107.25, 113.67 later) | −6% to −12%: layout, see below |

**Unison**, from the same two runs:

| Unison slot, saw | 440 Hz (ns / sample) | per voice | 12 kHz (ns / sample) | per voice |
|---|---:|---:|---:|---:|
| 1 voice | 3.30, 3.69 | 3.3–3.7 | 13.17, 13.45 | 13.2–13.5 |
| 2 voices | 6.76, 6.60 | 3.3–3.4 | 37.06, 27.57 | 13.8–18.5 |
| 4 voices | 13.32, 13.90 | 3.3–3.5 | 52.92, 58.61 | 13.2–14.7 |
| 8 voices | 25.11, 28.57 | 3.1–3.6 | 105.64, 115.26 | 13.2–14.4 |

| Other unison rows | ns / sample |
|---|---:|
| 8 voices, 440 Hz, pitch per sample | 36.35, 39.89 |
| 0 voices | 0.13, 0.15 |
| Noise, 8 voices, Stereo 0.5 | 2.23, 2.42 |
| 32-voice note (4 slots x 8 voices, 110–880 Hz) into one bus | 101.79, 105.31 |

### Observations

- **Unison costs one oscillator plus its gain per voice.** About 3.1–3.7 ns per voice at 440 Hz and 13–15 ns at 12 kHz, the same per voice as a one-voice slot: nothing is shared between voices yet (Phase 3). A whole 32-voice note, 4 oscillators of 8 voices with their stereo gains, costs 102–105 ns per sample: 0.5% of a core. That is within 10% of 32 bare mono oscillators (92–93 ns), and below 32 one-voice slots (105–114 ns).
- **0 voices costs nothing measurable:** 70 ns per block, the pitch conversion and the scan of 8 voice states. **Noise** with 8 voices costs 2.2–2.4 ns per sample: two draws and four multiply-adds, whatever the voice count.
- **A pitch per sample got cheaper.** The slot now converts a chunk's 64 centre frequencies in one loop before any voice renders, instead of between oscillator samples: 6.1–7.0 ns against 8.3–8.9 for the parent. With 8 voices it adds about 11 ns per sample (one `exp2` and 8 multiplies and `incrementFor`s), not 8 `exp2`.
- **The parent's volume-ramping row is a harness artefact.** In this session it measured 17.8–18.5 ns in every run, where the same code recorded 4.59 ns at its own commit (below). A standalone probe of exactly that loop (clang `-O2`, the minimum of 200 runs of 100 blocks) gives the parent 4.17 ns and this commit 4.72 ns, ramping, and 3.16 against 3.28 ns settled. So a one-voice slot is about 4% slower settled and 13% slower while a gain ramp runs: the voice loop's state checks and the gain's call through the array. The ramp lasts 10 ms per control change. The bus is not the cause: the probe gives the same numbers whether or not it is cleared between blocks.
- **The 32 one-voice slots are faster, by layout.** Each slot grew from 680 bytes to about 5 KB, so the benchmark's vector of 32 slots is laid out differently. The 680-byte layout was already shown to swing this row by 17% (the entry below). The unison rows' slots are allocated once, in an array.

Full run (the second of the two):

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,569 | 3.07 | 0.015 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,758 | 3.43 | 0.016 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,798 | 3.51 | 0.017 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,428 | 4.74 | 0.023 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,624 | 9.03 | 0.043 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 3,134 | 6.12 | 0.029 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,240 | 33.67 | 0.162 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 17,804 | 34.77 | 0.167 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 54,525 | 106.49 | 0.511 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,653 | 3.23 | 0.015 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,870 | 3.65 | 0.018 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,727 | 3.37 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,623 | 5.12 | 0.025 |
| PolyBLEP 2-point, square 5 kHz x512 | 2,878 | 5.62 | 0.027 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,937 | 5.74 | 0.028 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,645 | 5.17 | 0.025 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,743 | 5.36 | 0.026 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,681 | 5.24 | 0.025 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,915 | 5.69 | 0.027 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,621 | 5.12 | 0.025 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,833 | 5.53 | 0.027 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,372 | 4.63 | 0.022 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,144 | 4.19 | 0.020 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,824 | 5.51 | 0.026 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,138 | 6.13 | 0.029 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,396 | 4.68 | 0.022 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 52,509 | 102.56 | 0.492 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,464 | 4.81 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,822 | 5.51 | 0.026 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,780 | 5.43 | 0.026 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,303 | 6.45 | 0.031 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,884 | 7.59 | 0.036 |
| PolyBLEP 4-point, square 12 kHz x512 | 4,051 | 7.91 | 0.038 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,354 | 6.55 | 0.031 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 4,199 | 8.20 | 0.039 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,378 | 6.60 | 0.032 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,288 | 8.37 | 0.040 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,108 | 8.02 | 0.039 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 4,940 | 9.65 | 0.046 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,773 | 5.42 | 0.026 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,429 | 4.74 | 0.023 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,446 | 8.68 | 0.042 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 6,302 | 12.31 | 0.059 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,419 | 6.68 | 0.032 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 86,072 | 168.11 | 0.807 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,545 | 3.02 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,894 | 5.65 | 0.027 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,775 | 9.33 | 0.045 |
| minBLEP 32-tap, square 440 Hz x512 | 1,778 | 3.47 | 0.017 |
| minBLEP 32-tap, square 5 kHz x512 | 4,309 | 8.42 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 8,332 | 16.27 | 0.078 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,631 | 3.19 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 8,739 | 17.07 | 0.082 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,775 | 3.47 | 0.017 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 8,419 | 16.44 | 0.079 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,677 | 3.28 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 8,548 | 16.69 | 0.080 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,394 | 8.58 | 0.041 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,734 | 7.29 | 0.035 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,875 | 3.66 | 0.018 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,824 | 21.14 | 0.101 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,306 | 4.50 | 0.022 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 49,356 | 96.40 | 0.463 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,683 | 3.29 | 0.016 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,684 | 7.20 | 0.035 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,430 | 12.56 | 0.060 |
| minBLEP 64-tap, square 440 Hz x512 | 1,798 | 3.51 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 6,055 | 11.83 | 0.057 |
| minBLEP 64-tap, square 12 kHz x512 | 11,942 | 23.32 | 0.112 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,866 | 3.64 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 12,045 | 23.52 | 0.113 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,837 | 3.59 | 0.017 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 11,970 | 23.38 | 0.112 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,857 | 3.63 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 12,959 | 25.31 | 0.121 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,280 | 8.36 | 0.040 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,809 | 7.44 | 0.036 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,108 | 4.12 | 0.020 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 17,598 | 34.37 | 0.165 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,231 | 4.36 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 47,334 | 92.45 | 0.444 |
| Unison slot, saw 440 Hz, 1 voice x512 | 1,892 | 3.69 | 0.018 |
| Unison slot, saw 440 Hz, 2 voices x512 | 3,381 | 6.60 | 0.032 |
| Unison slot, saw 440 Hz, 4 voices x512 | 7,117 | 13.90 | 0.067 |
| Unison slot, saw 440 Hz, 8 voices x512 | 14,628 | 28.57 | 0.137 |
| Unison slot, saw 12 kHz, 1 voice x512 | 6,888 | 13.45 | 0.065 |
| Unison slot, saw 12 kHz, 2 voices x512 | 14,114 | 27.57 | 0.132 |
| Unison slot, saw 12 kHz, 4 voices x512 | 30,008 | 58.61 | 0.281 |
| Unison slot, saw 12 kHz, 8 voices x512 | 59,013 | 115.26 | 0.553 |
| Unison slot, saw 440 Hz, 8 voices, pitch per sample x512 | 20,425 | 39.89 | 0.191 |
| Unison slot, 0 voices x512 | 77 | 0.15 | 0.001 |
| Unison slot, noise, 8 voices x512 | 1,241 | 2.42 | 0.012 |
| Unison: 32-voice note (4 slots x 8 voices) into one bus x512 | 53,918 | 105.31 | 0.505 |
| minBLEP 64-tap, saw 440 Hz, retrigger every 64 samples x512 | 1,781 | 3.48 | 0.017 |
| minBLEP 64-tap, square 440 Hz, retrigger every 64 samples x512 | 2,109 | 4.12 | 0.020 |
| minBLEP 64-tap, triangle 440 Hz, retrigger every 64 samples x512 | 2,267 | 4.43 | 0.021 |
| minBLEP 64-tap, sine 440 Hz, retrigger every 64 samples x512 | 4,739 | 9.26 | 0.044 |
| minBLEP 64-tap, TriSaw 440 Hz, retrigger every 64 samples x512 | 2,127 | 4.15 | 0.020 |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master x512 | 2,475 | 4.83 | 0.023 |
| Oscillator slot, saw 440 Hz, retriggered every block x512 | 1,821 | 3.56 | 0.017 |

---

## 2026-11-14: M1.1 retrigger and phase

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds Phase and Retrig (it reports its parent, `0fa1524`, as modified).
- **What changed:** `MinBlepOscillator` gained `restart(phase)` and the band-limited `jumpTo(phase, since)` ([oscillator-phase.md](../dsp/oscillator-phase.md)). The per-sample fast path is unchanged. The corner scan of the slow path is now shared with the jump (it takes a span and a time offset), and the sine's phase history records each sample's step and any jump. `OscillatorSlot` gained Phase, Retrig, a `Random`, and `noteOn()`. New rows: each waveform at 440 Hz retriggered every 64 samples, a saw hard-synced at fractional times, and a slot retriggered every block.
- **Before and after, same session:** this tree, then the parent (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`), then this tree again; the slot rows then three more times, alternating.

| Benchmark | parent (ns / sample) | this commit (two runs) | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 2.96 | 2.98, 2.97 | 0% to +1% |
| minBLEP 64-tap, square 440 Hz | 3.42 | 3.41, 3.44 | 0% to +1% |
| minBLEP 64-tap, triangle 440 Hz | 3.42 | 3.50, 3.46 | +1% to +2% |
| minBLEP 64-tap, sine 440 Hz | 8.00 | 8.15, 8.22 | +2% to +3% |
| minBLEP 64-tap, saw 12 kHz | 11.99 | 12.63, 12.33 | +3% to +5% |
| minBLEP 64-tap, square 12 kHz | 22.15 | 23.17, 23.06 | +4% to +5% |
| minBLEP 64-tap, TriSaw 12 kHz | 30.95 | 32.96, 32.43 | +5% to +6% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 4.09 | 4.20, 4.09 | 0% to +3% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 94.39 | 91.23, 91.04 | −3% to −4% |
| Oscillator slot, saw 440 Hz, settled | 3.10 (3.12–3.30 later) | 3.25, 3.16 (3.16–3.25 later) | within noise |
| Oscillator slot, TriSaw 12 kHz, settled | 31.06 (31.99–33.36 later) | 33.41, 33.41 (33.38–36.21 later) | +3% to +8% |
| Oscillator slot, 32 saws into one bus | 99.87 (99.55–104.44 later) | 117.83, 116.21 (116.28–120.03 later) | +15% to +20%: a layout artefact, see below |

**The cost of a jump**, from each retrigger row against its twin without jumps in the same run (8 jumps per 512-sample block):

| Waveform, 440 Hz | Without jumps | Retriggered every 64 samples | Per jump |
|---|---:|---:|---:|
| Saw | 2.98, 2.97 | 3.10, 3.17 | 8–13 ns |
| Square | 3.41, 3.44 | 3.84, 3.77 | 21–27 ns |
| Triangle | 3.50, 3.46 | 3.98, 3.95 | 31 ns |
| Sine | 8.15, 8.22 | 8.80, 8.73 | 33–42 ns |
| TriSaw | 3.79, 3.83 | 4.25, 3.96 | 8–30 ns |

| Other rows | ns / sample |
|---|---:|
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master (a jump every 44 samples, between samples) | 4.71, 4.67 |
| Oscillator slot, saw 440 Hz, retriggered every block | 3.11, 3.11 (settled: 3.25, 3.16) |

### Observations

- **The per-sample path costs what it did.** Every oscillator row at 440 Hz is within 0–3% of the parent, and the 32-oscillator bank is 3–4% faster. Rows dominated by the slow path (12 kHz) are 2–6% slower in both runs. The shared corner scan adds a time offset to each crossing's sub-sample time, and the sine's history now writes 16 bytes per sample instead of 4.
- **A jump costs about the residual adds it makes:** 8–13 ns for the saw (one step residual, 64 taps), 20–30 ns with a bend as well, and 33–42 ns for the sine, which also evaluates sin and cos twice. One retrigger per note is invisible: a slot retriggered every block measures the same as a settled one. As hard sync (Part 2), a jump every 44 samples puts a 1 kHz saw at 4.7 ns per sample.
- **The slot bank's +15–20% is memory layout, not code.** A single slot is unchanged, the bare 32-oscillator bank is faster, and the extra state (the history's wider entries, `Random`) is never touched by a saw's per-sample loop. A probe outside the harness (32 slots, minimum of 200 runs, clang `-O2`) puts this commit's bank at 106 ns per sample, whatever padding is added to each slot. The parent's bank measures 107 ns with 16–240 bytes of padding and 115–125 ns without padding or with 500 bytes. So the bank's cost moves by up to 17% with nothing but where the slots fall in memory: the benchmark's vector grows by `push_back`, and `sizeof(OscillatorSlot)` went from 536 to 680 bytes. Placing per-voice state deliberately (alignment, one allocation per voice) is a Phase 3 item, and the unison task should reserve its oscillator storage up front.

Full run (the second of the two):

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,543 | 3.01 | 0.014 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,719 | 3.36 | 0.016 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,617 | 3.16 | 0.015 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,349 | 4.59 | 0.022 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,584 | 8.95 | 0.043 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 4,140 | 8.09 | 0.039 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,423 | 34.03 | 0.163 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 17,105 | 33.41 | 0.160 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 59,498 | 116.21 | 0.558 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,639 | 3.20 | 0.015 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,812 | 3.54 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,680 | 3.28 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,555 | 4.99 | 0.024 |
| PolyBLEP 2-point, square 5 kHz x512 | 2,873 | 5.61 | 0.027 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,709 | 5.29 | 0.025 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,592 | 5.06 | 0.024 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,664 | 5.20 | 0.025 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,612 | 5.10 | 0.024 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,763 | 5.40 | 0.026 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,562 | 5.00 | 0.024 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,792 | 5.45 | 0.026 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,225 | 4.35 | 0.021 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,080 | 4.06 | 0.019 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,745 | 5.36 | 0.026 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,031 | 5.92 | 0.028 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,326 | 4.54 | 0.022 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 52,447 | 102.44 | 0.492 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,440 | 4.76 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,667 | 5.21 | 0.025 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,614 | 5.11 | 0.025 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,180 | 6.21 | 0.030 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,646 | 7.12 | 0.034 |
| PolyBLEP 4-point, square 12 kHz x512 | 3,787 | 7.40 | 0.036 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,165 | 6.18 | 0.030 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 3,732 | 7.29 | 0.035 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,214 | 6.28 | 0.030 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,011 | 7.83 | 0.038 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,022 | 7.86 | 0.038 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 6,181 | 12.07 | 0.058 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,619 | 5.12 | 0.025 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,188 | 4.27 | 0.021 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,127 | 8.06 | 0.039 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 5,480 | 10.70 | 0.051 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,267 | 6.38 | 0.031 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 77,894 | 152.14 | 0.730 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,455 | 2.84 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,743 | 5.36 | 0.026 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,351 | 8.50 | 0.041 |
| minBLEP 32-tap, square 440 Hz x512 | 1,617 | 3.16 | 0.015 |
| minBLEP 32-tap, square 5 kHz x512 | 4,256 | 8.31 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 7,852 | 15.34 | 0.074 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,636 | 3.20 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,866 | 15.36 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,600 | 3.12 | 0.015 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,951 | 15.53 | 0.075 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,603 | 3.13 | 0.015 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,911 | 15.45 | 0.074 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,206 | 8.21 | 0.039 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,638 | 7.11 | 0.034 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,707 | 3.33 | 0.016 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,627 | 20.76 | 0.100 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,045 | 3.99 | 0.019 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 47,044 | 91.88 | 0.441 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,519 | 2.97 | 0.014 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,562 | 6.96 | 0.033 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,315 | 12.33 | 0.059 |
| minBLEP 64-tap, square 440 Hz x512 | 1,762 | 3.44 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,788 | 11.30 | 0.054 |
| minBLEP 64-tap, square 12 kHz x512 | 11,806 | 23.06 | 0.111 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,810 | 3.53 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 11,730 | 22.91 | 0.110 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,724 | 3.37 | 0.016 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 11,683 | 22.82 | 0.110 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,774 | 3.46 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 11,874 | 23.19 | 0.111 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,208 | 8.22 | 0.039 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,628 | 7.09 | 0.034 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 1,960 | 3.83 | 0.018 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,603 | 32.43 | 0.156 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,092 | 4.09 | 0.020 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 46,615 | 91.04 | 0.437 |
| minBLEP 64-tap, saw 440 Hz, retrigger every 64 samples x512 | 1,624 | 3.17 | 0.015 |
| minBLEP 64-tap, square 440 Hz, retrigger every 64 samples x512 | 1,931 | 3.77 | 0.018 |
| minBLEP 64-tap, triangle 440 Hz, retrigger every 64 samples x512 | 2,024 | 3.95 | 0.019 |
| minBLEP 64-tap, sine 440 Hz, retrigger every 64 samples x512 | 4,470 | 8.73 | 0.042 |
| minBLEP 64-tap, TriSaw 440 Hz, retrigger every 64 samples x512 | 2,026 | 3.96 | 0.019 |
| minBLEP 64-tap, saw 1 kHz synced to a 1.09 kHz master x512 | 2,393 | 4.67 | 0.022 |
| Oscillator slot, saw 440 Hz, retriggered every block x512 | 1,593 | 3.11 | 0.015 |

---

## 2026-11-13: M1.1 oscillator controls (tuning, volume, pan, invert)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds the controls (it reports its parent, `1fae0ad`, as modified).
- **What changed:** a new layer, `OscillatorSlot` ([oscillator-controls.md](../dsp/oscillator-controls.md)), around an unchanged oscillator (it only gained a `phaseIncrement()` getter). New rows: the slot settled, mid-ramp, with a pitch per sample, at 12 kHz, and as a 32-oscillator bank; and in the same benchmark case, the bare oscillator at exactly the slot's pitch, both as it is (mono) and with the same stereo gain written inline. The inline version is the fair baseline, because the slot must produce two channels.
- **Before and after, same session:** the parent commit (exported with `git archive`, `external/` symlinked, built `relwithdebinfo`) was benchmarked straight after this run, and once earlier in the session. Every oscillator row is within −3% to +5% of the parent's, which is within noise.

| Benchmark | parent (ns / sample) | this commit | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.20 | 3.16 | −1% |
| minBLEP 64-tap, square 12 kHz | 22.54 | 23.49 | +4% |
| minBLEP 64-tap, sine 440 Hz | 8.10 | 8.48 | +5% |
| minBLEP 64-tap, TriSaw 12 kHz | 32.49 | 32.63 | 0% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 4.19 | 4.34 | +4% |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) | 93.50 | 96.75 | +3% |

**The slot against the bare oscillator**, same run (and the range over three runs in this session):

| Case | Bare oscillator, mono out | Bare + the same stereo gain inline | Oscillator slot |
|---|---:|---:|---:|
| Saw 440 Hz, settled | 3.10 (3.04–3.26) | 3.53 (3.53–4.01) | 3.40 (3.24–3.43) |
| Saw 440 Hz, volume ramping | | | 4.65 (4.65–5.14) |
| Saw 440 Hz, pitch per sample (one `noteToFrequency` each) | | 9.51 (9.15–9.51) | 9.13 (8.39–9.13) |
| TriSaw 12 kHz, settled | | 33.93 (32.63–33.93) | 32.44 (32.10–33.58) |
| 32 saws into one bus | 96.75 (mono sum, row above) | | 105.13 (103.99–112.73) |

- **At ordinary pitches the slot costs about 0.2–0.3 ns per sample over the bare mono oscillator** (+6–10%), for two output channels with volume, pan, and polarity. It is **cheaper than writing the same stereo gain inline** (3.40 against 3.53 ns). The slot stages 64 samples on the stack and then applies the gains in a separate loop, which clang vectorizes (width 4, interleaved twice: `-Rpass=loop-vectorize` on `stereo_gain.hpp`). The inline loop is tied to the oscillator's sample-by-sample state and is not vectorized.
- **A volume ramp adds about 1.3–1.7 ns per sample** while it lasts (10 ms): two `LinearSmoother` steps, about 0.8 ns each in the M1.0 baseline, in a loop that cannot vectorize because each step advances the smoother.
- **A pitch per sample costs about 6 ns more than a pitch per block**, almost all of it the `std::exp2` in `noteToFrequency` (the bare oscillator's "pitch set every sample" row uses precomputed frequencies). `fastExp2` (3·10⁻⁷ relative, 5·10⁻⁴ cents) would save most of it. A Phase 3 candidate: it would make the per-sample path differ from the per-block one by that much.
- **512 voices** of the settled slot would take 512 × 3.40 = 1,741 ns of every 20,833 ns sample period at 48 kHz: 8.4% of a core, against 7.6% for bare mono oscillators.

Full run:

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz, bare oscillator (mono) x512 | 1,589 | 3.10 | 0.015 |
| minBLEP 64-tap, saw 440 Hz, bare oscillator + inline stereo gain x512 | 1,805 | 3.53 | 0.017 |
| Oscillator slot, saw 440 Hz, settled x512 | 1,742 | 3.40 | 0.016 |
| Oscillator slot, saw 440 Hz, volume ramping x512 | 2,380 | 4.65 | 0.022 |
| minBLEP 64-tap, saw 440 Hz, noteToFrequency every sample + inline stereo gain x512 | 4,869 | 9.51 | 0.046 |
| Oscillator slot, saw 440 Hz, pitch per sample x512 | 4,676 | 9.13 | 0.044 |
| minBLEP 64-tap, TriSaw 12 kHz, bare oscillator + inline stereo gain x512 | 17,374 | 33.93 | 0.163 |
| Oscillator slot, TriSaw 12 kHz, settled x512 | 16,612 | 32.44 | 0.156 |
| Oscillator slot, 32 saws (4 osc x 8 unison) into one bus x512 | 53,826 | 105.13 | 0.505 |
| PolyBLEP 2-point, saw 440 Hz x512 | 1,736 | 3.39 | 0.016 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,823 | 3.56 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,724 | 3.37 | 0.016 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,621 | 5.12 | 0.025 |
| PolyBLEP 2-point, square 5 kHz x512 | 3,020 | 5.90 | 0.028 |
| PolyBLEP 2-point, square 12 kHz x512 | 2,887 | 5.64 | 0.027 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,619 | 5.11 | 0.025 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,870 | 5.61 | 0.027 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,809 | 5.49 | 0.026 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 2,834 | 5.53 | 0.027 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,908 | 5.68 | 0.027 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,977 | 5.81 | 0.028 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,382 | 4.65 | 0.022 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,026 | 3.96 | 0.019 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 3,588 | 7.01 | 0.034 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,251 | 6.35 | 0.030 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,998 | 5.86 | 0.028 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 54,680 | 106.80 | 0.513 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,439 | 4.76 | 0.023 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,893 | 5.65 | 0.027 |
| PolyBLEP 4-point, saw 12 kHz x512 | 3,199 | 6.25 | 0.030 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,509 | 6.85 | 0.033 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,904 | 7.62 | 0.037 |
| PolyBLEP 4-point, square 12 kHz x512 | 3,767 | 7.36 | 0.035 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,217 | 6.28 | 0.030 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 3,857 | 7.53 | 0.036 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 4,499 | 8.79 | 0.042 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,124 | 8.06 | 0.039 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,001 | 7.81 | 0.038 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 5,156 | 10.07 | 0.048 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,759 | 5.39 | 0.026 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,388 | 4.66 | 0.022 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,384 | 8.56 | 0.041 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 5,604 | 10.95 | 0.053 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,336 | 6.52 | 0.031 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 82,671 | 161.47 | 0.775 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,543 | 3.01 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,661 | 5.20 | 0.025 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,511 | 8.81 | 0.042 |
| minBLEP 32-tap, square 440 Hz x512 | 1,799 | 3.51 | 0.017 |
| minBLEP 32-tap, square 5 kHz x512 | 4,083 | 7.98 | 0.038 |
| minBLEP 32-tap, square 12 kHz x512 | 7,941 | 15.51 | 0.074 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,691 | 3.30 | 0.016 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,869 | 15.37 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,669 | 3.26 | 0.016 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,950 | 15.53 | 0.075 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,717 | 3.35 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,956 | 15.54 | 0.075 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,405 | 8.60 | 0.041 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,739 | 7.30 | 0.035 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,806 | 3.53 | 0.017 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 9,758 | 19.06 | 0.091 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,148 | 4.19 | 0.020 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 61,712 | 120.53 | 0.579 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,615 | 3.16 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,679 | 7.19 | 0.034 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,483 | 12.66 | 0.061 |
| minBLEP 64-tap, square 440 Hz x512 | 1,838 | 3.59 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,935 | 11.59 | 0.056 |
| minBLEP 64-tap, square 12 kHz x512 | 12,027 | 23.49 | 0.113 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,853 | 3.62 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 11,489 | 22.44 | 0.108 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,838 | 3.59 | 0.017 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 12,084 | 23.60 | 0.113 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,844 | 3.60 | 0.017 |
| minBLEP 64-tap, triangle 12 kHz x512 | 11,987 | 23.41 | 0.112 |
| minBLEP 64-tap, sine 440 Hz x512 | 4,340 | 8.48 | 0.041 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,758 | 7.34 | 0.035 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,032 | 3.97 | 0.019 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,706 | 32.63 | 0.157 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,223 | 4.34 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 49,538 | 96.75 | 0.464 |

---

## 2026-11-13: M1.1 the 8 waveforms, as data

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, on the working tree of the commit that adds the waveforms (it reports its parent, `35460ca`, as modified).
- **What changed:** the oscillators now read each waveform from a table of corners ([oscillator-waveforms.md](../dsp/oscillator-waveforms.md)) instead of a switch. New rows: every waveform at 440 Hz and 12 kHz, and the saw at 12 kHz.
- **Before and after, same session:** the parent commit's build was benchmarked straight after this run on the same machine, so the comparison below is not muddied by noise between sessions.

| Benchmark | before (ns / sample) | after | change |
|---|---:|---:|---:|
| minBLEP 64-tap, saw 440 Hz | 3.07 | 3.16 | +3% |
| minBLEP 64-tap, saw 5 kHz | 6.72 | 7.24 | +8% |
| minBLEP 64-tap, square 440 Hz | 3.52 | 3.71 | +5% |
| minBLEP 64-tap, square 5 kHz | 11.13 | 11.66 | +5% |
| minBLEP 64-tap, square 12 kHz | 21.61 | 23.64 | +9% |
| minBLEP 64-tap, triangle 440 Hz | 3.81 | 3.86 | +1% |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample | 3.95 | 4.43 | +12% |
| minBLEP 64-tap, 32 saws | 94.70 | 90.60 | −4% |
| minBLEP 32-tap, 32 saws | 90.41 | 92.04 | +2% |
| PolyBLEP 2-point, saw 440 Hz | 2.41 | 3.28 | +36% |
| PolyBLEP 2-point, square 440 Hz | 3.12 | 5.61 | +80% |
| PolyBLEP 2-point, 32 saws | 86.64 | 108.32 | +25% |
| PolyBLEP 4-point, saw 440 Hz | 2.50 | 5.19 | +108% |
| PolyBLEP 4-point, triangle 440 Hz | 4.18 | 8.90 | +113% |
| PolyBLEP 4-point, 32 saws | 77.71 | 164.63 | +112% |

Full run:

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| PolyBLEP 2-point, saw 440 Hz x512 | 1,678 | 3.28 | 0.016 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,841 | 3.59 | 0.017 |
| PolyBLEP 2-point, saw 12 kHz x512 | 1,859 | 3.63 | 0.017 |
| PolyBLEP 2-point, square 440 Hz x512 | 2,871 | 5.61 | 0.027 |
| PolyBLEP 2-point, square 5 kHz x512 | 3,082 | 6.02 | 0.029 |
| PolyBLEP 2-point, square 12 kHz x512 | 3,136 | 6.13 | 0.029 |
| PolyBLEP 2-point, HPulse 440 Hz x512 | 2,582 | 5.04 | 0.024 |
| PolyBLEP 2-point, HPulse 12 kHz x512 | 2,914 | 5.69 | 0.027 |
| PolyBLEP 2-point, QPulse 440 Hz x512 | 2,666 | 5.21 | 0.025 |
| PolyBLEP 2-point, QPulse 12 kHz x512 | 3,084 | 6.02 | 0.029 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 2,721 | 5.31 | 0.026 |
| PolyBLEP 2-point, triangle 12 kHz x512 | 2,982 | 5.82 | 0.028 |
| PolyBLEP 2-point, sine 440 Hz x512 | 2,526 | 4.93 | 0.024 |
| PolyBLEP 2-point, sine 12 kHz x512 | 2,166 | 4.23 | 0.020 |
| PolyBLEP 2-point, TriSaw 440 Hz x512 | 2,917 | 5.70 | 0.027 |
| PolyBLEP 2-point, TriSaw 12 kHz x512 | 3,343 | 6.53 | 0.031 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,503 | 4.89 | 0.023 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 55,458 | 108.32 | 0.520 |
| PolyBLEP 4-point, saw 440 Hz x512 | 2,660 | 5.19 | 0.025 |
| PolyBLEP 4-point, saw 5 kHz x512 | 2,884 | 5.63 | 0.027 |
| PolyBLEP 4-point, saw 12 kHz x512 | 2,852 | 5.57 | 0.027 |
| PolyBLEP 4-point, square 440 Hz x512 | 3,413 | 6.67 | 0.032 |
| PolyBLEP 4-point, square 5 kHz x512 | 3,907 | 7.63 | 0.037 |
| PolyBLEP 4-point, square 12 kHz x512 | 4,108 | 8.02 | 0.039 |
| PolyBLEP 4-point, HPulse 440 Hz x512 | 3,431 | 6.70 | 0.032 |
| PolyBLEP 4-point, HPulse 12 kHz x512 | 4,068 | 7.95 | 0.038 |
| PolyBLEP 4-point, QPulse 440 Hz x512 | 3,400 | 6.64 | 0.032 |
| PolyBLEP 4-point, QPulse 12 kHz x512 | 4,085 | 7.98 | 0.038 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 4,555 | 8.90 | 0.043 |
| PolyBLEP 4-point, triangle 12 kHz x512 | 6,145 | 12.00 | 0.058 |
| PolyBLEP 4-point, sine 440 Hz x512 | 2,851 | 5.57 | 0.027 |
| PolyBLEP 4-point, sine 12 kHz x512 | 2,389 | 4.67 | 0.022 |
| PolyBLEP 4-point, TriSaw 440 Hz x512 | 4,718 | 9.21 | 0.044 |
| PolyBLEP 4-point, TriSaw 12 kHz x512 | 6,699 | 13.08 | 0.063 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 3,635 | 7.10 | 0.034 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 84,293 | 164.63 | 0.790 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,547 | 3.02 | 0.015 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,987 | 5.83 | 0.028 |
| minBLEP 32-tap, saw 12 kHz x512 | 4,495 | 8.78 | 0.042 |
| minBLEP 32-tap, square 440 Hz x512 | 1,711 | 3.34 | 0.016 |
| minBLEP 32-tap, square 5 kHz x512 | 4,263 | 8.33 | 0.040 |
| minBLEP 32-tap, square 12 kHz x512 | 7,715 | 15.07 | 0.072 |
| minBLEP 32-tap, HPulse 440 Hz x512 | 1,598 | 3.12 | 0.015 |
| minBLEP 32-tap, HPulse 12 kHz x512 | 7,939 | 15.51 | 0.074 |
| minBLEP 32-tap, QPulse 440 Hz x512 | 1,686 | 3.29 | 0.016 |
| minBLEP 32-tap, QPulse 12 kHz x512 | 7,924 | 15.48 | 0.074 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,697 | 3.31 | 0.016 |
| minBLEP 32-tap, triangle 12 kHz x512 | 7,987 | 15.60 | 0.075 |
| minBLEP 32-tap, sine 440 Hz x512 | 4,072 | 7.95 | 0.038 |
| minBLEP 32-tap, sine 12 kHz x512 | 3,405 | 6.65 | 0.032 |
| minBLEP 32-tap, TriSaw 440 Hz x512 | 1,796 | 3.51 | 0.017 |
| minBLEP 32-tap, TriSaw 12 kHz x512 | 10,417 | 20.35 | 0.098 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 2,157 | 4.21 | 0.020 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 47,125 | 92.04 | 0.442 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,619 | 3.16 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,706 | 7.24 | 0.035 |
| minBLEP 64-tap, saw 12 kHz x512 | 6,049 | 11.81 | 0.057 |
| minBLEP 64-tap, square 440 Hz x512 | 1,902 | 3.71 | 0.018 |
| minBLEP 64-tap, square 5 kHz x512 | 5,969 | 11.66 | 0.056 |
| minBLEP 64-tap, square 12 kHz x512 | 12,102 | 23.64 | 0.113 |
| minBLEP 64-tap, HPulse 440 Hz x512 | 1,856 | 3.62 | 0.017 |
| minBLEP 64-tap, HPulse 12 kHz x512 | 12,288 | 24.00 | 0.115 |
| minBLEP 64-tap, QPulse 440 Hz x512 | 1,890 | 3.69 | 0.018 |
| minBLEP 64-tap, QPulse 12 kHz x512 | 12,409 | 24.24 | 0.116 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,974 | 3.86 | 0.019 |
| minBLEP 64-tap, triangle 12 kHz x512 | 12,110 | 23.65 | 0.114 |
| minBLEP 64-tap, sine 440 Hz x512 | 3,964 | 7.74 | 0.037 |
| minBLEP 64-tap, sine 12 kHz x512 | 3,468 | 6.77 | 0.033 |
| minBLEP 64-tap, TriSaw 440 Hz x512 | 2,012 | 3.93 | 0.019 |
| minBLEP 64-tap, TriSaw 12 kHz x512 | 16,698 | 32.61 | 0.157 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 2,270 | 4.43 | 0.021 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 46,386 | 90.60 | 0.435 |

### Observations

- **minBLEP costs what it did.** At ordinary pitches it is within noise of the hard-coded switch (32 saws: 2.8 ns per oscillator-sample), and it is 5–12% slower where corners come often (5 and 12 kHz) and when the pitch is set every sample. Two changes got it there; without them it was about 60% slower:
  - a single "distance to the next corner" compare per sample;
  - two versions of the loop body, with and without the sine.
- **Cost grows with the residual adds per cycle.** At 12 kHz, 64 taps: saw 11.8 ns (1 per cycle); square, HPulse, QPulse, and triangle 23.6–24.2 ns (2); TriSaw 32.6 ns (3, because its corner at 0 both jumps and bends). The TriSaw at 12 kHz is the new worst case. 512 voices of it would take about 80% of a core at 48 kHz. Adding the step and ramp residuals of one corner in a single pass would bring it closer to the square (a Phase 3 item).
- **The sine costs 6.8–7.7 ns,** most of it `std::sin` (about 4 ns), against 3.2 ns for a saw. A polynomial sine is a Phase 3 candidate. It would need about degree 9 to keep the sine's harmonics below −100 dB.
- **The PolyBLEP reference got slower, by 25–110%.** PolyBLEP tests every corner, ahead and behind, on every sample, so reading the corners from the table instead of constants costs it on every sample. It is a test-only reference, kept for its quality numbers, which are unchanged. The D6 entry below still holds its hard-coded CPU figures, and those are what the D6 choice was made on.

---

## 2026-11-13: M1.1 oscillator anti-aliasing prototypes (D6)

- **Machine, compiler, build:** as below (Apple M1 Ultra, virtualised; Apple clang 21.0.0; `relwithdebinfo`). `tools/bench.sh "[oscillators]"`, 100 samples per benchmark, at commit `984d868` (the run was on its working tree just before it was committed).
- **Noise:** a second run differed by up to 12% on single rows (PolyBLEP 4-point square 5 kHz: 4.86 and 3.94 ns) and by 5% on the 32-oscillator rows.
- **Since then:** the minBLEP and PolyBLEP benchmarks still exist under the same names (minBLEP in `anoesis::dsp`, PolyBLEP as a test-only reference). The wavetable was removed after D6; its rows can be rerun from commit `984d868`.
- **What runs:** one oscillator, one 512-sample block per benchmark, at 48 kHz, through `next()` one sample at a time. Nothing is vectorised yet. The "32 saws" rows are one note's worth (4 oscillators × 8 unison voices at 110 to 880 Hz, ±20 cents): divide by 32 for the cost per oscillator. Quality measurements and the choice: [band-limited-oscillators.md](../dsp/band-limited-oscillators.md).

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| PolyBLEP 2-point, saw 440 Hz x512 | 1,287 | 2.51 | 0.012 |
| PolyBLEP 2-point, saw 5 kHz x512 | 1,406 | 2.75 | 0.013 |
| PolyBLEP 2-point, square 440 Hz x512 | 1,495 | 2.92 | 0.014 |
| PolyBLEP 2-point, square 5 kHz x512 | 1,821 | 3.56 | 0.017 |
| PolyBLEP 2-point, square 12 kHz x512 | 1,698 | 3.32 | 0.016 |
| PolyBLEP 2-point, triangle 440 Hz x512 | 1,794 | 3.50 | 0.017 |
| PolyBLEP 2-point, saw 440 Hz, pitch set every sample x512 | 2,001 | 3.91 | 0.019 |
| PolyBLEP 2-point, 32 saws (4 osc x 8 unison) x512 | 40,537 | 79.17 | 0.380 |
| PolyBLEP 4-point, saw 440 Hz x512 | 1,261 | 2.46 | 0.012 |
| PolyBLEP 4-point, saw 5 kHz x512 | 1,488 | 2.91 | 0.014 |
| PolyBLEP 4-point, square 440 Hz x512 | 1,459 | 2.85 | 0.014 |
| PolyBLEP 4-point, square 5 kHz x512 | 2,489 | 4.86 | 0.023 |
| PolyBLEP 4-point, square 12 kHz x512 | 2,169 | 4.24 | 0.020 |
| PolyBLEP 4-point, triangle 440 Hz x512 | 2,214 | 4.32 | 0.021 |
| PolyBLEP 4-point, saw 440 Hz, pitch set every sample x512 | 2,304 | 4.50 | 0.022 |
| PolyBLEP 4-point, 32 saws (4 osc x 8 unison) x512 | 39,833 | 77.80 | 0.373 |
| minBLEP 32-tap, saw 440 Hz x512 | 1,469 | 2.87 | 0.014 |
| minBLEP 32-tap, saw 5 kHz x512 | 2,718 | 5.31 | 0.025 |
| minBLEP 32-tap, square 440 Hz x512 | 1,639 | 3.20 | 0.015 |
| minBLEP 32-tap, square 5 kHz x512 | 3,926 | 7.67 | 0.037 |
| minBLEP 32-tap, square 12 kHz x512 | 6,710 | 13.11 | 0.063 |
| minBLEP 32-tap, triangle 440 Hz x512 | 1,831 | 3.58 | 0.017 |
| minBLEP 32-tap, saw 440 Hz, pitch set every sample x512 | 1,901 | 3.71 | 0.018 |
| minBLEP 32-tap, 32 saws (4 osc x 8 unison) x512 | 48,380 | 94.49 | 0.454 |
| minBLEP 64-tap, saw 440 Hz x512 | 1,602 | 3.13 | 0.015 |
| minBLEP 64-tap, saw 5 kHz x512 | 3,464 | 6.76 | 0.032 |
| minBLEP 64-tap, square 440 Hz x512 | 1,848 | 3.61 | 0.017 |
| minBLEP 64-tap, square 5 kHz x512 | 5,683 | 11.10 | 0.053 |
| minBLEP 64-tap, square 12 kHz x512 | 11,368 | 22.20 | 0.107 |
| minBLEP 64-tap, triangle 440 Hz x512 | 1,899 | 3.71 | 0.018 |
| minBLEP 64-tap, saw 440 Hz, pitch set every sample x512 | 1,976 | 3.86 | 0.019 |
| minBLEP 64-tap, 32 saws (4 osc x 8 unison) x512 | 47,092 | 91.98 | 0.441 |
| Wavetable (mipmapped), saw 440 Hz x512 | 1,200 | 2.34 | 0.011 |
| Wavetable (mipmapped), saw 5 kHz x512 | 1,165 | 2.28 | 0.011 |
| Wavetable (mipmapped), square 440 Hz x512 | 1,218 | 2.38 | 0.011 |
| Wavetable (mipmapped), square 5 kHz x512 | 1,279 | 2.50 | 0.012 |
| Wavetable (mipmapped), square 12 kHz x512 | 1,252 | 2.45 | 0.012 |
| Wavetable (mipmapped), triangle 440 Hz x512 | 1,238 | 2.42 | 0.012 |
| Wavetable (mipmapped), saw 440 Hz, pitch set every sample x512 | 1,772 | 3.46 | 0.017 |
| Wavetable (mipmapped), 32 saws (4 osc x 8 unison) x512 | 38,783 | 75.75 | 0.364 |

### Observations

- **At ordinary pitches, all five cost about the same:** 2.3 to 2.9 ns per oscillator-sample in the 32-saw bank (PolyBLEP 2.4, minBLEP 2.9, wavetable 2.3). The per-sample overhead (a scalar loop with a phase carried from sample to sample, and a switch on the shape) dominates. So does the difference between 32 and 64 minBLEP taps (2.95 and 2.87 ns): at 440 Hz an edge comes once every 109 samples.
- **minBLEP's cost grows with pitch; the others' doesn't.** Each edge adds `Taps` multiply-adds. A 12 kHz square has two edges every 4 samples: 13.1 ns per sample at 32 taps, 22.2 ns at 64, against 3.3 (PolyBLEP 2-point), 4.2 (4-point), and 2.5 (wavetable). The worst case is 512 voices of high squares: about 32% (32 taps) or 55% (64 taps) of one core at 48 kHz.
- **Setting the pitch every sample** costs 0.8 to 1.4 ns more: least for minBLEP (+0.8), which needs no reciprocal (it divides only at an edge), then the wavetable (+1.1, a level lookup), then PolyBLEP (+1.4, a reciprocal per sample).
- **First Phase 3 candidates:** the minBLEP residual add uses a masked ring index (`(position + i) & (Taps - 1)`), which blocks vectorisation; a double-length linear buffer would let the 32 or 64 multiply-adds per edge run 4 wide. Across unison voices, PolyBLEP vectorises with no gathers, the wavetable needs gathers, and minBLEP needs per-lane scatter of the edge events.

---

## 2026-11-13: M1.0 foundations (first baseline)

- **Machine:** Apple M1 Ultra, **virtualised** (`machdep.cpu.brand_string` reports "Apple M1 Ultra (Virtual)"), macOS arm64.
- **Compiler:** Apple clang 21.0.0 (clang-2100.3.34.2).
- **Build:** `relwithdebinfo` (`-O2 -g`), commit `M1.0h` (the commit that added the harness). `tools/bench.sh`, 100 samples per benchmark.
- **Noise:** two runs differed by up to about 10% (for example `fastExp2` 175 and 194 ns). Treat differences below about 15% as noise on this machine.

| Benchmark | ns / block | ns / sample | % of one core at 48 kHz (1 channel) |
|---|---:|---:|---:|
| std::exp2 x512 | 914 | 1.79 | 0.009 |
| fastExp2 x512 | 175 | 0.34 | 0.002 |
| std::tanh x512 | 1,269 | 2.48 | 0.012 |
| fastTanh x512 | 228 | 0.45 | 0.002 |
| LinearSmoother ramping x512 | 407 | 0.80 | 0.004 |
| ExponentialSmoother ramping x512 | 1,903 | 3.72 | 0.018 |
| LinearSmoother settled x512 | 23 | 0.05 | 0.000 |
| WhiteNoise x512 | 574 | 1.12 | 0.005 |
| Oversampler 2x up+down x512 | 8,555 | 16.71 | 0.080 |
| Oversampler 4x up+down x512 | 13,711 | 26.78 | 0.129 |
| one-pole, normal state x512 | 1,035 | 2.02 | 0.010 |
| one-pole, denormal state, IEEE mode x512 | 1,021 | 1.99 | 0.010 |
| one-pole, denormal state, flush-to-zero x512 | 1,076 | 2.10 | 0.010 |

### Observations

- **Fast math pays off:** `fastExp2` is 5.2× faster than `std::exp2`, and `fastTanh` 5.6× faster than `std::tanh`, at the accuracy in [fast-math.md](../dsp/fast-math.md). Both loops vectorise; the library calls don't.
- **Oversampling** costs about 17 ns (2×) and 27 ns (4×) per sample per channel, round trip. That is 0.08% and 0.13% of a core. Stereo 4× distortion is about 0.26%, before the distortion itself. The time goes into serial allpass chains (each section depends on the previous one), which SIMD across channels or branches could overlap.
- **Denormals cost nothing on Apple Silicon:** the same loop takes about 2.0 ns per sample with normal state, denormal state, or flush-to-zero. This is the expected ARM behaviour ([denormals.md](../dsp/denormals.md)). The x86 penalty, the reason the guard exists, still needs measuring on an x86 machine.
- **First Phase 3 candidate: `ExponentialSmoother` is 4.7× slower than `LinearSmoother`** (3.72 against 0.80 ns per sample). The hypothesis: in `fill()`, the output span is `float*` and could alias the smoother's own `float` members. So the compiler reloads and stores `distance` and `current` around every write, which puts a store-to-load round trip into the loop-carried multiply chain. Copying the state into locals for the loop should fix it. This is recorded here, not fixed, so Phase 3 starts with a measured before.
