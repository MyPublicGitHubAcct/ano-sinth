# The AmpEnv A/B link

Sylenth1 v3.053 (25 March 2019) "Added option to link AmpEnv A and B parameters." Nothing official says what the link does. This page covers M1.4 task 6: stage 1, and the decision that closes it.

- **Theory:** a parameter link against a signal link; what each means for a voice that runs two envelopes; how a link can turn on and off under a sounding note; and which of a voice's readings (the output, a crossing part, the skip test, `isActive()`) have to follow it.
- **Sylenth1:** what is known. One changelog line, one user's feature request three months before it, and the developer's reply to that request.
- **The candidates:** a parameter link on `dsp::Voice` unchanged, the same with one envelope run for both parts, and a signal link. Each turns on and off mid-note in several ways. All were prototyped on a copy of the real voice over the real `dsp::VoiceAllocator`.
- **What they measured:** identity with `dsp::Voice`, the clicks of every transition in five musical cases, the skip test and `isActive()` read right and wrong, the real-time terms, and CPU.
- **Recommendations** for the project owner, and where each piece belongs. Most of the feature isn't DSP. **The decision** (every recommendation, 2026-11-22): a parameter link with no change to the voice, the rest moved to M1.9, M1.10, M1.11 and M1.5.

Read [envelopes.md](envelopes.md) (the `Adsr`: constant rate, sustain ramped over 10 ms), [voice-architecture.md](voice-architecture.md) (two envelopes per voice, and a crossing part going through both) and [part-solo-and-skipping.md](part-solo-and-skipping.md) (the skip test and the requirement it left on this task) first.

## Theory

### A parameter link or a signal link

Two envelopes can be "linked" in two different places:

- **A parameter link** ties the *knobs*. Editing Attack on part A also sets Attack on part B, so the two envelopes always have the same settings. Each part still runs its own envelope generator. Since both are gated by the same note events, two generators with the same settings produce the same levels, sample for sample.
- **A signal link** ties the *signals*. Part B's amplitude is taken from envelope A, and B's own knobs stay where they were, unused. Unlinking brings B's own envelope back.

For a note that starts with the link already on, the two are the same sound: two identical generators, or one generator read twice. They differ in three places. The first is the knobs: what B's controls show and store, and what unlinking gives back. The second is a link that changes under a sounding note: a parameter link changes B's *settings*, a signal link changes the *signal* B is multiplied by. The third is the cost: a signal link (or a parameter link that notices the two are equal) can run one generator instead of two.

### Turning a link on under a sounding note

The two kinds behave differently at a mid-note switch.

- **Changing settings.** Our `Adsr` was built to take knob changes mid-stage without a jump (envelopes.md, E8). A new time takes effect at once, entering the full-scale curve where it passes the current level (constant rate). A new sustain ramps there over 10 ms. Linking B to A's settings is therefore exactly as smooth as automating B's four knobs at once. What it doesn't do is bring B's *level* to A's: B continues from where it is, with A's times, and meets A only when both reach the same stage and level (a sustain, or 0).
- **Switching signals.** B's gain jumps from envelope B's level to envelope A's, a step as large as their difference. It can be crossfaded over a few ms, as the sustain's and Solo's 10 ms ramps are. Or the switch can wait for the next note-on, where the envelopes restart anyway. Even then it isn't free: a note-on on a sounding voice (a steal, a retrigger in place) attacks *from the current level*, and that level is the other envelope's.

### What has to follow the link

A voice reads each part's envelope in five places: the part's output (after its filter); a part crossing into the other filter (before that filter, voice-architecture.md V2); task 5's skip test (`envelopeSilent()`: is the envelope idle, or held at a sustain of 0?); `isActive()`, which decides when the allocator frees the voice; and the Voices readout. Under a parameter link each part's own envelope still applies, so nothing changes. Under a signal link, or with one shared generator, B's own generator is unused or stale, and every reading must take the one that applies. Task 5 left exactly this requirement ([What this pushes back on](part-solo-and-skipping.md#what-this-pushes-back-on)).

## Sylenth1: what is known

The full table with ratings is in the inventory: [AmpEnv A/B link](../reference/sylenth1-features.md#ampenv-ab-link) (AL1–AL10). In short:

- **The changelog** (v3.053, 25 March 2019, both lines of the release): "Added option to link AmpEnv A and B parameters." and "Added locking options for all parameters and parts." This is the only official mention. The manual is still v2.5 (2015, checked 2026-11-22: all seven language editions on the manual page are the 2015 PDFs) and predates it.
- **The request that preceded it.** On 9 December 2018 a user asked in the LennarDigital forum on KVR ([*Linking Amp Env of A and B??*](https://www.kvraudio.com/forum/viewtopic.php?t=516243), [p=7248656](https://www.kvraudio.com/forum/viewtopic.php?p=7248656)) why there is "no kind of button to link both envelopes toghether instead of the more time consuming and workflow killing Copy/Paste", because layered sounds "are using the same envelope setting for A and B anyway". The developer answered with a routing: both parts through filter A with nothing to filter B, and Amp Env B's release at max and attack at 0, "(sort of bypassing it)", so "all oscillators are controlled by Amp Env A only" ([p=7249362](https://www.kvraudio.com/forum/viewtopic.php?p=7249362)). The user found that this "doubles more or less the decay time for the Oscillators of Part B" (both envelopes act on a crossing part, as V2 says). The developer agreed: "to get them exactly the same you'd still need to adjust both to the same position" ([p=7249554](https://www.kvraudio.com/forum/viewtopic.php?p=7249554)). The user's reply: "Back to the linking button...". Another user forgets "to copy and paste settings to the other layer". v3.053 came out 3½ months later.
- **The CC map** (manual p. 32) gives the eight knobs eight default CCs: AmpEnv A Attack–Release on 76–79, AmpEnv B on 80–83.
- **Locking** before v3.053 (v3.031, "lock function in right-mousebutton menu for certain parameters (when switching presets)"): a user found it on Pitchbend range, Mono Legato and Portamento, and on Part select, "this means i can not lock the settings of Part A but I can lock that Part A is selected" ([KVR t=484599](https://www.kvraudio.com/forum/viewtopic.php?p=6782687), 2017). v3.053 extended locking to "all parameters and parts". "Copy to part A/B" came later (v3.063, November 2019; more "Copy To" options v3.066).
- **Envelope stages as modulation destinations:** unknown. The manual doesn't list the destinations. KVR's product page counts "24 different selectable modulation parameters" per slot. A user in 2017 calls the targets "a bit limited by modern standards. Many modern synths also allow modulating individual envelope stages" ([KVR t=481791](https://www.kvraudio.com/forum/viewtopic.php?f=94&t=481791)). That suggests Sylenth1 doesn't (low; M1.5's open question).

Nobody describes the option itself: where it is, what turning it on does to B's knobs, or whether it is saved. Medium: the request and the changelog's wording ("parameters") describe a **parameter link**, made to replace copy and paste. The developer's own routing, which made one envelope act on both parts, is what the user rejected.

## The candidates

Prototyped in [link_candidates.hpp](../../tests/dsp/voice/link_candidates.hpp):

- `LinkVoice` is a copy of `dsp::Voice` as built (tasks 1–5). With no link it is `dsp::Voice` to the bit (15 of 15 renders).
- `LinkEngine<V>` runs the real `dsp::VoiceAllocator` over 16 voices (`dsp::Voice` or `LinkVoice`). It holds both parts' envelope knobs and the link, a stand-in for M1.9's parameter layer. It splits each host block at its events.

| Candidate | The voice | The knobs (engine) | Mid-note, on and off |
|---|---|---|---|
| **(a) parameter** | `dsp::Voice`, unchanged | While linked, part B's envelope is given the master's knobs. At unlink B's own come back (*restore*), or B keeps the master's (*keep*). | **snap**: the knobs change at once, and `Adsr` takes them as it takes automation. **next note**: each voice takes the change at its next note-on (needs per-voice pending settings, so a library change). |
| **(b) shared** | (a), plus the voice runs one generator for both parts while linked. Sharing starts at a note-on from silence, where the two are the same to the bit. At unlink the other generator takes the master's state and runs on. | As (a) | snap |
| **(c) signal** | Part B's gain reads the master's envelope. B's knobs are kept, and its own generator keeps running on them, unused. At a note-on from silence the switch is immediate. | Each part's own knobs | **snap**; a **10 ms ramp** (a linear crossfade from one envelope's level to the other's); **next note** (the voice's next note-on, not a legato note). |

Each was run with master A (B follows A) and, where it matters, master B. For (b) and (c), every reading of an envelope reads the one that applies. A `wrongReading` switch reads each part's own instead, to measure what that costs.

The hidden `[amp-link-report]` ([amp_link_report.cpp](../../tests/dsp/voice/amp_link_report.cpp)) prints every table below. The `[amp-link]` benchmarks are in [amp_link_benchmark.cpp](../../tests/dsp/benchmarks/amp_link_benchmark.cpp).

## What they measured

### Linked from the note-on: every candidate is `dsp::Voice`

Five sequences were rendered at host blocks of 1, 37 and 4096: a staggered chord on two parts, steals at Polyphony 2, a Mono Legato line, a chord with filter A on AB, and a resonant unison pad. Each candidate was linked from before the first note, with part B's own knobs left at different settings. Each was compared with `dsp::Voice` unlinked, both envelopes set to the master's:

| Candidate | Identical (of 15) | Allocations |
|---|---:|---:|
| `LinkVoice`, no link (against `dsp::Voice` with A ≠ B) | 15 | 0 |
| (a) parameter, snap / next note | 15 / 15 | 0 |
| (b) shared | 15 | 0 |
| (c) signal, snap / 10 ms ramp / next note | 15 / 15 / 15 | 0 |
| master B: (a) / (b) / (c) snap | 15 / 15 / 15 | 0 |

So for every note that starts with the link on, the three candidates sound the same, to the bit. That includes steals, legato and a crossing part, where envelope A then acts both before filter A and after it. They differ only when the link changes under a note, and in what B's knobs hold.

### Mid-note: clicks, and how soon each reaches the linked sound

Quiet sines were used: A1 at 220 Hz and B1 at 330 Hz, Volume 0.25, free-running, through LP24 at 4,677 Hz. AmpEnv A was A 5 ms, D 0.2 s, S 0.8, R 0.3 s; AmpEnv B was A 0.4 s, D 1 s, S 0.3, R 1.2 s. Each toggle was measured three ways:

- **Click:** energy above 1 kHz in a Hann 4096 centred 32 samples after the toggle, in dB re a unit step (filters.md's measure, as in tasks 2–5).
- **Floor:** the same model without the toggle.
- **Reached:** how long after the toggle the render is identical to the one linked throughout.

Link on, master A (dB; floor in brackets):

| Case | (a) snap | (a) next note | (c) snap | (c) 10 ms ramp | (c) next note | Reached: (a) snap / (c) ramp |
|---|---:|---:|---:|---:|---:|---|
| Held (B in its decay) | −61.1 (−70.7) | at floor | **−24.6** | −61.2 | at floor | 10.0 ms / 10.0 ms |
| Release tail | −78.1 (−81.4) | at floor | **−33.2** | −67.7 | at floor | 230 ms / 10.0 ms |
| Crossing (filter A on AB, B on `--`) | −62.8 (−72.0) | at floor | **−29.8** | −62.9 | at floor | 10.6 ms / 10.6 ms |
| Mono Legato | −64.3 (−92.2) | at floor | **−35.8** | −64.1 | at floor | 10.0 ms / 10.0 ms |
| In-place steal (Polyphony 1): the toggle | −57.5 (−71.7) | at floor | −51.6 | −69.9 | at floor | 201 ms / 10.0 ms |
| … then the steal | −57.1 (−50.9) | −47.4 | −57.1 | −57.1 | **−24.7** | |

- **(b) is (a) to the bit** through every toggle, on and off (10 of 10).
- **Master B** is the mirror image: (a) −61.9, (c) snap −27.5 and −36.5 on the crossing part, ramp −65.3 held. The parameter link then reaches the linked sound only when A and B meet (800 ms held, never within a release).
- **At the next legato note** "next note" doesn't switch (−62.2 against a floor of −59.3), so a link turned on during a legato line waits for the line to end.

Link off (restore B's own; master A):

| Case | (a) restore | (a) keep | (c) snap | (c) 10 ms ramp | (c) next note |
|---|---:|---:|---:|---:|---:|
| Held | −57.2 (−70.7) | at floor | **−24.6** | −60.5 | at floor |
| Release tail | −97.9 (−81.4) | −95.9 | **−33.2** | −68.3 | at floor |
| Crossing | −59.5 (−72.0) | at floor | **−29.8** | −62.0 | at floor |
| Mono Legato | −60.7 (−92.2) | at floor | **−35.8** | −64.2 | at floor |
| In-place steal | −58.8 (−71.7) | at floor | −51.8 | −70.8 | at floor; **−24.5 at the steal** (floor −50.9) |

What this says:

- **A signal link that snaps clicks** like a cut: −24.6 to −36.5 dB (−51.6 to −54.0 at the steal case's toggle, where the two levels happened to be close), against −24.0 for task 5's instant Solo.
- **Waiting for the next note doesn't avoid the click, it moves it**: a steal in place then clicks at −24.5 to −24.7, because a retrigger attacks from the other envelope's level.
- **The 10 ms ramp brings a signal link to the parameter link's level**: −60.5 to −71.3. This matches the sustain's ramp and Solo's, by the same mechanism.
- **A parameter link needs nothing new.** Its clicks (−57.2 to −97.9) are those of automating the four knobs at once, which `Adsr` was built for (E8). Keeping B's values at unlink changes nothing audible (at the floor in every case).
- **The difference between (a) and (c) after a toggle is how B's level behaves.** (c) takes A's level within 10 ms. Under (a), B carries on from its own level with A's times, and meets A only when their stages meet: 10 ms when B's level is below A's sustain, 201–800 ms otherwise, and not within a release from a different level. That is what "linking the settings" means, and it can be heard only on a note held through the switch.

### The skip test and `isActive()` read the envelope that applies

Saws were rendered, linked from before the note, master A. The reference is `dsp::Voice` unlinked with both envelopes set to A's.

| Case | Candidate | Against the reference | Outputs skipped | Notes busy |
|---|---|---|---:|---|
| B's own knobs at sustain 0 (decay 50 ms), A's at 1 | (c), reading right | identical | 0.0% | 1 (ref. 1) |
| | (c), reading B's own | **−3.1 dB** (part B skipped from 50 ms) | 47.9% | 1 |
| | (a) (B mirrored) | identical | – | 1 |
| Only part B sounds; B's own release 0, A's 1 s | (c), reading right | identical | 0.0% | 1 at 0.6 s (ref. 1) |
| | (c), reading B's own | **the whole tail lost** (0.0 dB: the difference is the reference) | 0.1% | **0** at 0.6 s: freed at the note-off |
| | (a) | identical | – | 1 |
| Shared, both R 0.3 s | (b), reading right | identical | 0.0% | 0 at 1.5 s (ref. 0) |
| | (b), reading B's stale generator | identical | 30.0% | **1 at 1.5 s: never freed** |
| Shared, both sustain 0 | (b), reading right | identical | 95.0% | 1 (ref. 1) |
| | (b), reading B's stale generator | identical | 47.9% | 1 |

Under a signal link, reading a part's own envelope is audibly wrong. It skips a part that sounds, and it frees a voice whose tail is still playing. Under (b), a stale generator never reaches idle, so the voice is never freed and the skip is lost. Under (a), `dsp::Voice`'s readings are right as they are: each part's own envelope is the one that applies.

### Real-time terms

A sequence on the pad turned the link on at 0.25 s, edited A's four knobs at 0.35 s, stole a voice at Polyphony 3, and turned the link off at 0.6 s. For every candidate and transition the output was the same to the bit at host blocks of 1, 37 and 4096, with 0 allocations while processing.

### CPU

16 voices were held (keys 40–55) through `VoiceAllocator`, linked from the note-on with part B's own knobs different. Times are in ns per sample, the median of three runs of `tools/bench.sh "[amp-link]"` (spread 0.1–1.7%, one row 5.0% from one slow run):

| Case | `dsp::Voice`, no link | (a) `dsp::Voice` linked | prototype, no link | (b) shared | (c) signal |
|---|---:|---:|---:|---:|---:|
| init (A1 a saw) | 2,017.25 | 2,011.43 | 2,011.56 | 2,023.77 | 2,015.02 |
| two parts | 3,925.12 | 3,932.70 | 3,937.01 | 3,954.34 | 3,944.04 |
| pad (unison into filter A on AB) | 8,680.39 | 8,639.36 | 8,624.26 | 8,662.91 | 8,665.02 |
| two parts, both envelopes in a 10 s attack | 3,955.08 | 3,960.49 | 3,960.72 | 3,950.27 | 3,958.87 |

Every candidate is within noise of `dsp::Voice`. One envelope costs 0.52 ns per sample in a moving stage (M1.2's baseline). Running one instead of two saves at most 16 × 0.52 ≈ 8 ns per sample for 16 voices, 0.2% of the two-part case, below the run-to-run spread. The init patch reads 2,017 this session against task 5's 1,958 for the same code: that 3% is the machine.

## Recommendations

*Stage 1's recommendations, as written. Where only the demo can settle a question, the recommendation is the working hypothesis in the inventory (AL1–AL10). Chosen by the project owner on 2026-11-22, every one ([The decision](#the-decision)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **What the link is** | **A parameter link (a)**: while it is on, the two amp envelopes have the same settings. The voice runs both generators, unchanged. | The changelog says "parameters", and the request it answered asked for a button instead of copy and paste. The developer's one-envelope routing was rejected in that thread. Measured: for a note that starts linked, (a), (b) and (c) are the same to the bit (135 of 135 renders), so the reading only matters at a mid-note switch and for the knobs. | (c) a signal link; (b) one shared generator. | AL1 |
| 2 | **The voice (M1.4)** | **No change to `dsp::Voice`.** Task 5's requirement is met as built: under (a) each part's own envelope is the one that applies, so `envelopeSilent()`, `isActive()` and the readout are right as they are (measured identical in every case). Record it, and close task 6 in M1.4. | Nothing to build, nothing to get wrong. Under (b) or (c) every reading would have to change. Reading the wrong one costs a part (−3.1 dB), a whole tail, or a voice never freed. | (b): one generator, which saves ≤ 0.2% (below noise) and needs a state handover at unlink. (c): a new envelope source per part with a 10 ms crossfade and four readings changed. | (ours) |
| 3 | **A mid-note switch** | **Snap: the knobs change at once, and the running envelopes take them as automation** (constant rate, sustain ramped over 10 ms). | −57.2 to −97.9 dB, the same as automating the knobs, and no new DSP. "Next note" needs per-voice pending settings in the library and still clicks at a steal (−47.4 against −50.9). | Next note. (For a signal link: the 10 ms ramp, −60.5 to −71.3; snap −24.6 to −36.5; next note moves the click to the next steal, −24.5.) | AL10 |
| 4 | **Turning it on: which values win** | **The part shown (Part select) keeps its values; the other part's four knobs take them.** Afterwards an edit to either part's envelope sets both. | Nothing the user is looking at moves. It is what copy and paste from the shown part would do. With master A or B the DSP is symmetric (−61.1 / −61.9 dB held). | Always A to B; the last part edited. | AL3 |
| 5 | **Turning it off** | **Both parts keep their (equal) values**, and edits stop mirroring. | No hidden copy to store and nothing audible (at the floor in every case). The link replaced copy and paste, and unlinking shouldn't undo the paste. | Restore B's values from before the link (−57.2 dB held, and B's old values become hidden state that presets must carry). | AL4 |
| 6 | **Where it lives** | **A preset parameter, an on/off switch saved with the patch; not host-automatable and not in the default CC map.** M1.9's parameter table holds it, M1.10's format stores it, and M1.11 draws it by the amp envelopes. | A layered patch is edited linked, so reopening it should restore that. Automating a link that only mirrors edits does nothing audible. The CC map (pp. 31–32) predates it and has no slot for it. | A global setting (as `oscdrift`); interface state (as Solo). | AL2 |
| 7 | **Automation, MIDI CC, MIDI learn** | **Every edit path mirrors**: GUI, host automation, the default CCs 76–83 and learned CCs alike. An edit to either part's knob writes both parameters, and the host is told about both, inside one gesture for a GUI drag. M1.9 has to check that a host recording automation doesn't loop on the mirrored parameter (pluginval, auval, and one host by hand). | The link is meant to keep the two equal. Mirroring only GUI edits would let automation or a CC pull them apart while the switch says "linked". | Mirror GUI edits only; or read-through (the engine feeds both envelopes from the master's knobs and leaves the other's parameters untouched: no parameter writes another, but B's controls show values that aren't heard). | AL5 |
| 8 | **Presets, Randomize, Copy to Part, locking** | **Preset load** takes the stored values as they are (a preset saved linked has them equal). If a file has the link on and A ≠ B, the part A values win, so loads are deterministic. **Randomize** draws A's four values once and mirrors them. **Copy to Part A/B** copies the envelope like any other part control, and the copy then follows the link. **Locking**: a locked knob keeps its value across a preset change, and the link mirrors edits, not loads. A locked link switch keeps the link's state. M1.9 and M1.10 decide the details. | Each keeps "linked means equal" true without the voice being involved. | Restore the mirroring at load from the shown part. | AL6, AL7 |
| 9 | **Modulation** | **Nothing for M1.4.** If M1.5 finds amp-envelope stages among the destinations (AL8; probably not), a route to AmpEnv A's attack modulates envelope A only. Modulation isn't an edit, so a parameter link doesn't carry it. **AmpEnv A and B as misc sources** stay each part's own generator. Under the link they are equal for any note that started linked. | Under (a) the sources are the generators that apply, so nothing changes. Under (c) "AmpEnv B" would read a generator nobody hears, which is a trap. | Under (c), source B reads the master. | AL8, AL9 |

### Where each piece belongs

- **`libs/dsp` (M1.4): nothing.** The voice is right as built. Task 6 closes here once the decision records that task 5's requirement is met by (a), with this page's measurements as the evidence.
- **M1.9 (`libs/interop` and the parameter table):** the switch as a preset parameter (row 6); mirroring on every edit path, with host notification for both parameters (rows 4, 5, 7); its interplay with locking (row 8). Recommend moving the PLAN.md task there, as "AmpEnv A/B link (v3)", next to "Parameter and part locking".
- **M1.10 (presets):** storing the switch; loads, Randomize and Copy to Part A/B with the link on (row 8).
- **M1.11 (GUI):** the button, and B's knobs moving with A's.
- **M1.5:** AL8 and AL9, if the destination list turns up envelope stages.

### What this pushes back on

- **Task 5's requirement on this task** (part-solo-and-skipping.md, "What this pushes back on") holds as written for a signal link or a shared generator. Under a parameter link it is met with no change: each part's own envelope is the one that applies. The measurements above show what the alternatives would have had to get right.
- **The inventory's AmpEnv link row** said "High. Built with the voice (M1.4)". High was right only for the feature's existence. Its behaviour is medium (a parameter link) to unknown (everything else), and under the recommendation it isn't built in the voice at all.
- **E8** (time changes apply at once, sustain ramps 10 ms) now carries the link's switch too. If the demo shows that an automated envelope knob behaves otherwise, the link follows.
- **The developer's 2018 routing** confirms V2 from a second direction: a crossing part goes through both envelopes, so B's envelope "doubles" its decay unless both are set alike.

## The decision

Chosen by the project owner on 2026-11-22: every recommendation (PLAN.md, Decisions log).

- **A parameter link.** While it is on, the two amp envelopes' eight knobs hold equal values. `dsp::Voice` runs both envelopes, unchanged.
- **M1.4 task 6 closes with no change to `libs/dsp`.** Each part's own envelope is the one that applies, so task 5's skip test, `isActive()` and the Voices readout are right as built. That meets task 5's requirement; the stage-1 report runs `dsp::Voice` under the link and is the evidence. No code changed, so no test was added.
- **A mid-note switch applies at once.** The running envelopes take the new settings as they take automation: constant rate, the sustain ramped over 10 ms (E8).
- **Turning it on:** the part shown (Part select) keeps its values and the other part's four knobs take them. Afterwards an edit to either part's envelope sets both.
- **Turning it off:** both parts keep their values, and edits stop mirroring.
- **The switch** is a preset parameter, on/off, not host-automatable, with no default CC.
- **Every edit path mirrors:** the GUI, host automation, the default CCs 76–83 and learned CCs. An edit to either part's knob writes both parameters, and the host is told about both (inside one gesture for a GUI drag). M1.9 checks that recording automation doesn't loop on the mirrored parameter.
- **Presets, Randomize, Copy to Part, locking:** a preset loads as stored, with part A's values winning if a file has the link on and A ≠ B; Randomize draws A's envelope once and mirrors it; Copy to Part copies the envelope and the link then keeps the copy in step; a lock acts on preset loads, the link on edits, and a locked switch keeps the link's state.
- **Modulation:** not carried by the link. AmpEnv A and B as misc sources are each part's own generator.

Where it went (PLAN.md):

- **M1.9:** a new task, "AmpEnv A/B link (v3.053)", beside parameter and part locking: the switch, the mirroring and the host notification.
- **M1.10:** storing the switch; loads, Randomize and Copy to Part with the link on.
- **M1.11:** the button by the amp envelopes; the other part's knobs moving with the edited part's.
- **M1.5:** AL8 and AL9, with the destination list.

The demo settles AL1–AL10 in M1.13. If it shows a signal link, the voice needs the 10 ms crossfade and the readings measured here: `envelopeSilent()`, `isActive()` and the readout reading the envelope that applies.

## References

- LennarDigital, [*Sylenth1 version history*](https://lennardigital.com/sylenth1/whatsnew.php): v3.031, v3.053, v3.063, v3.066. *Sylenth1 User Manual*, v2.5, 2015, pp. 17, 19–20, 31–32 ([manual page](https://lennardigital.com/sylenth1/manual.php), checked 2026-11-22).
- KVR, LennarDigital forum: [*Linking Amp Env of A and B??*](https://www.kvraudio.com/forum/viewtopic.php?t=516243) (December 2018: the request, and the developer at p=7249362 and p=7249554); [*Sylenth1 v3.031 - lock function…*](https://www.kvraudio.com/forum/viewtopic.php?p=6782687) (2017); [*Velocity to envelope amount in Sylenth1?*](https://www.kvraudio.com/forum/viewtopic.php?f=94&t=481791) (2017). KVR, [*LennarDigital updates Sylenth1 to v3.053*](https://www.kvraudio.com/news/lennardigital-updates-sylenth1-to-v3-053-44678) (news, 27 March 2019) and the [product page](https://www.kvraudio.com/product/sylenth1-by-lennardigital). Every source is rated in the [feature inventory](../reference/sylenth1-features.md#ampenv-ab-link).
- Steinberg, *VST 3 Developer Portal*: parameter changes from the edit controller (`beginEdit`/`performEdit`/`endEdit`), for row 7's notification of both parameters. Apple, *Audio Unit Programming Guide*: parameter change notifications.
- [envelopes.md](envelopes.md) (E8, constant rate, the sustain's ramp), [voice-architecture.md](voice-architecture.md) (V2, a crossing part), [part-solo-and-skipping.md](part-solo-and-skipping.md) (the skip test, its requirement on this task), [voice-allocation.md](voice-allocation.md) (`isActive()`, a retrigger in place), [legato-portamento.md](legato-portamento.md) (`legatoGateOn`), [filters.md](filters.md) (the click measure).
