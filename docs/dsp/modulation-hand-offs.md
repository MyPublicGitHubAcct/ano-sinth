# Modulation: global routes and the hand-offs

The last task of M1.5 (task 5). [modulation.md](modulation.md) built the model, [modulation-sources.md](modulation-sources.md) the sources and [modulation-destinations.md](modulation-destinations.md) the per-voice destinations. Two destinations are left, **Phaser Frequency** and **Distortion Amount**. They act on the master effects, which don't exist until M1.7. This page settles what the voice does with a route to them, and hands everything outside `libs/dsp` on to the milestones that build it:

- **M1.6:** Step Velocity.
- **M1.7:** the global destinations.
- **M1.9:** the slots as parameters, their automation and their CCs.
- **M1.10:** presets.
- **M1.11:** the popups.

- **Theory:** routes a consumer never reads; a modulation slot as parameters, and why a menu's host value depends on how many entries it has; persisting a choice by key; default CCs and MIDI learn.
- **Sylenth1:** what is known (inventory §6, new [Global destinations, parameters and CCs](../reference/sylenth1-features.md#global-destinations-parameters-and-ccs), M28–M30). New for this task: the manual's default CC table (pp. 31–32) has no modulation control.
- **Stage 1:** two candidates for global routes in the voice, measured; the recommendations, and the hand-offs. Chosen by the project owner on 2026-11-23, every one ([The decision](#the-decision)).
- **Stage 2:** `compile()` by scope, the voice's `RouteCompiler`, and a flaw it found (a recompile that gave the same routes still started a crossfade); the tests, the deliberate breaks, and CPU ([As built](#as-built)).

Read [modulation.md](modulation.md) first (scope, the registry, and [question 5](modulation.md#question-5-a-per-voice-source-on-a-global-destination), a per-voice source on a global destination).

## Theory

### Routes a consumer never reads

The registry gives each destination a **scope** ([registry.hpp](../../libs/dsp/include/anoesis/dsp/modulation/registry.hpp)). A per-voice destination is read by each voice. A global destination is read once, by the effect it belongs to. Task 2 decided what a global destination reads: each per-voice source's **global copy**, never a voice's value (modulation.md, recommendation 7).

A route then has exactly one consumer: the voices, or the engine. If both compile every route, a route that only the engine reads still has effects in each voice:

- the voice runs the route's source, because `CompiledRoutes::uses()` says a route reads it;
- the voice leaves its no-routes path and sums a target nobody reads;
- a change to the route is a change of the voice's routes, so it starts the voice's 10 ms crossfade, and the crossfade holds back the voice's other changes until it ends.

The first two cost CPU. The third changes the sound. Filtering by scope at compile time removes all three: each consumer compiles only its own routes, from the same 16 slots and the same `compile()`.

### A modulation slot as parameters

A slot holds three values: a source, a destination and an amount. As host parameters they are two kinds:

- **The amount** is a continuous knob from −10 to +10, like any other. Its host value maps linearly onto the range (inventory M5).
- **The source and the destination** are menus. A host stores a parameter as a normalised value in [0, 1]. For a menu of n entries, entry i is i / (n − 1). That ties the host's value to the menu's length: **adding an entry moves every entry's value.** An automation lane recorded against 28 destinations plays back on other entries once there are 30.

This matters here because Phase 2 adds destinations and sources as registry rows (CLAUDE.md). There are three ways to keep host data stable:

1. **Not automatable.** The menus are preset state, stored by key (below). The host never holds their values. The amounts stay automatable.
2. **A fixed capacity.** Each menu is declared with room to spare (for example 64 entries, the unused ones shown as "-"), so its host values never move. This works, but hosts show the spare entries.
3. **Automatable, as is.** A new release that adds entries breaks old automation of the menus.

### Persisting a choice by key

A preset stores each menu's choice as the row's **string key** (decided in task 2: `"osc.volume.a"`, `"lfo1"`, …). It never stores a position or an enum value. A preset therefore survives reordering and new rows, and a key it doesn't know (from a newer version) can load as "-".

### Default CCs and MIDI learn

A default CC map assigns MIDI controllers to parameters with no setup. MIDI learn lets a user assign one. Both act on parameters, so whatever isn't a parameter (or isn't automatable) can't take a CC either. A bipolar knob mapped from 7-bit MIDI has no exact centre: 0–127 over −10 to +10 puts 63 at −0.08 and 64 at +0.08. A centred map (64 → 0) fixes that, at the cost of unequal steps on the two sides.

## Sylenth1: what is known

From the manual, and from the developer's posts already in the inventory (§6, [Global destinations, parameters and CCs](../reference/sylenth1-features.md#global-destinations-parameters-and-ccs)):

- **The default CC map has no modulation control** (manual pp. 31–32, §6.1 "MIDI Control Changes"). It has 80 entries (the wheel, the pedal and All Notes Off among them), including LFO 1/2 Rate, Gain and Offset (CC 84–89), Distortion Amount (93) and Phaser CenterFreq (96). It lists no amount, no source or destination selector, and no Mod Envelope knob. The manual's "Each knob has a default CC parameter, listed in the appendix" (p. 10) is therefore true of "most knobs", as that page's first sentence says, not all.
- **MIDI learn on the amounts works.** A user: "Only via Host by routing the wheel (Midi learn) to one of the two Mod1-Env-destination amount knobs". Another had trouble centring an amount from a CC (63 just below 0, 64 just above), which fits a linear map with no centre (M30). Learned CCs "will still be active after you close and restart your host" (p. 11).
- **Automation:** the developer in 2010: "Sylenth1 fully supports both parameter automation and midi automation on all its controls." v3.050: "Improved host automation for buttons and option controls." So the amounts are automatable, and probably the menus too (M28).
- **The menus:** the destination popup has headers ("- Oscillators -", "- Filters -") over its columns (manual p. 19's screenshot, and the demo's, M1). A slot's display shows the choice in short form ("Cutoff AB", "Pan AB", "LFO1 Gain"; pp. 19–20). Double-clicking an amount resets it to 0 (p. 19).
- **The preset format** (the third-party template, names only) holds 16 amounts, 16 destination choices and 4 misc source choices.
- **Nothing on Copy to Part** (v3) and the modulation slots, which belong to no part (M29).

## The candidates

A route to Phaser Frequency or Distortion Amount, in `dsp::Voice`:

- **(a) Today:** `compile()` keeps every route, so each voice compiles the global routes too.
- **(b) The voice compiles only `Scope::voice` routes.** A global route is invisible to it. The engine (M1.7) compiles the `Scope::global` routes from the same slots.

(b) renders exactly as (a) with the global routes removed from the patch, and that is how the tests render it ([global_routes_test.cpp](../../tests/dsp/modulation/global_routes_test.cpp)).

## What they measured

Over the real `VoiceAllocator` at 48 kHz, through the destinations' test engine. The global routes come from an S&H LFO, velocity and a mod envelope, so the voice would run sources nothing else reads. They appear, change and go mid-note.

| Case | (a) against (b) |
|---|---|
| Only global routes, the init voicing with a Noise slot, host blocks of 1, 37 and 4096 | **the same to the bit**; no allocation |
| The busy patch (task 3) plus three global routes, no change mid-note, host blocks of 1, 37 and 4096 | **the same to the bit** |
| A global amount changed 97 samples before a per-voice change (Mix A's route, +5 → −2) | differ: up to −14.1 dBFS (sample peak of the difference) |
| … 300 samples before | differ: −20.2 dBFS |
| … at the same sample, or 600 before (after the 480-sample crossfade) | the same to the bit |
| LFO 2 already read by a global route; a route from it to Mix A appears at 9001 (sine / S&H) | differ: −26.0 / −21.6 dBFS |

- **A global route alone changes nothing audible**, and no random stream moves (a Noise slot and an S&H LFO both agree).
- **A global change holds back a per-voice change** made within its 10 ms crossfade. In (a), Mix A's new amount arrives up to 10 ms late, so the voice's sound depends on a route it never reads.
- **A late per-voice route finds its LFO already running in (a).** In (b) it starts the LFO from its note-on state at the next grid point, which is task 3's decided rule for any late route. In (a), whether that rule applies depends on whether a global route happened to read the same LFO.

**CPU,** one held voice (benchmark "[global-routes]", 4 runs, medians; [baselines](../performance/baselines.md#2026-11-23-m15-global-routes-stage-1)):

| Routes | ns / sample | against none |
|---|---:|---:|
| none, which is (b) with any number of global routes | 130.3 | |
| (a) 4 global routes (an envelope, an LFO, velocity, the wheel) | 129.6 | within the noise |
| (a) 16 global routes | 134.3 | +4.0 (+3%) |
| (a) 16 global routes, a change at every grid point | 139.0 | +8.7 (+7%) |

That is per voice: 16 voices pay it 16 times, for nothing they read.

## Recommendations

*Stage 1's recommendations, as written. Chosen by the project owner on 2026-11-23, every one ([The decision](#the-decision)).*

| # | Question | Recommendation | Why | Alternatives | Settled by |
|---|---|---|---|---|---|
| 1 | **Global routes in the voice** | **(b) The voice compiles only its own routes.** `compile()` takes a scope, and `RouteCompiler` compiles for one. The voice's is `Scope::voice`. M1.7's engine compiles `Scope::global` from the same slots. | Removes the dependence of a voice's sound on routes it never reads (−14.1 dBFS from a delayed change, −21.6 from a late LFO) and the +3–7% per voice. Every patch without global routes renders as today, to the bit. | (a) as is. | (ours) |
| 2 | **The amounts as parameters** (M1.9) | **16 host-automatable float parameters, −10 to +10, linear** (M5), default 0, each with a stable parameter ID per slot. MIDI-learnable. | The developer: "all its controls"; a user learned a CC to one. Task 4's crossfade already takes an amount's steps without clicks, so the parameter needs no ramp. | (none) | M5, M28 |
| 3 | **The source and destination menus as parameters** (M1.9) | **Preset state, not host-automatable**: the 16 destination choices and the 4 misc source choices are stored by key, with no host value and no MIDI learn. | A menu's host value moves whenever Phase 2 adds a row, which would break automation recorded earlier. Changing a route by automation is rare next to automating its amount. | **Automatable with a fixed capacity** (closest to Sylenth1's v3.050, probably; hosts show the spare entries). Automatable as is (breaks old automation once rows are added). | M28 (Sylenth1's behaviour); ours is a choice for Phase 2 |
| 4 | **Default CCs** (M1.9) | **None for any modulation control**: no amount, no menu. The Mod Envelopes' knobs have none either. Learned CCs as for any knob. | The manual's map has none, and its free CCs are left for the user. | Assign some of the free CCs (2–4, 6, 30–63, …). | The manual (pp. 31–32) |
| 5 | **A CC on an amount** (M1.9) | **M1.9 decides one 7-bit law for every bipolar knob** (amounts, Pan, LFO Offset, …): linear, as Sylenth1's appears to be, or centred at 64. | It is the same question for every bipolar knob, so it belongs to the parameter layer. | Decide it here for the amounts alone. | M30 |
| 6 | **Presets** (M1.10) | Each slot saved as **{source key, destination key, amount}**. The four misc source choices are saved by key, and the Mod Envelope and LFO slots' fixed sources aren't saved. **A key a build doesn't know loads as "-"** with its amount kept, and the load goes on. | Keys survive reordering and new rows (task 2). A preset from a newer version still loads. | Refuse the preset; map unknown keys to a fallback row. | D7 |
| 7 | **The popups** (M1.11) | The destination popup is **"-" and the 27 in three headed columns**: Oscillators (12), Filters (6) and Misc (9), the same in every slot (M1). The source popup is "-" and the 11 (M2). The slot shows the choice's short label. Labels and groups come from M1.9's parameter table, and Phase 2's rows join a column by their group. Double-click resets an amount to 0. | The demo's popups (M1, M2) and the manual (p. 19). | (none) | M1, M2, M4 |
| 8 | **The global destinations** (M1.7) | **The engine compiles the global routes, sums them on the absolute 32-sample grid from the sources' global copies, and crossfades any change of them over 10 ms, as the voices do.** It also clamps each effect's knob plus its modulation to the knob's range (`knob10` until M1.7 gives the effects' knobs) before the knob's law. A global destination with no route costs nothing. | The voices' crossfade answers M24's clicks, and a global destination has the same clicks. Task 2's decision (global copies, ramped on the absolute grid) is unchanged. | Step at a grid point and ramp to the next. | M10, M24 |
| 9 | **Step Velocity** (M1.6) | **No change to M1.6's requirement.** Step Velocity is already a global source, so a route from it to a global destination reads the same value the voices read. | (as recorded on M1.6 task 4) | (none) | M23, AR18 |
| 10 | **Copy to Part** (M1.10) | **The modulation slots aren't copied**: they belong to no part. A route to an A, B or AB destination is left as it is. | The panel is shared between the parts (pp. 19–20). | Mirror A and B destinations onto the target part. | M29 |

### What the library needs (stage 2, recommendation 1)

- **`compile(slots, scope)`** keeps only the routes whose destination row has that scope. `RouteCompiler` holds its scope, and `dsp::Voice` uses `Scope::voice`. `uses()`, `routesTo()`, `empty()` and the crossfade then see only the voice's routes.
- **Tests first:**
  - the three cases where (a) and (b) differ become "the same as without the global routes", to the bit, at host blocks of 1, 37 and 4096;
  - only global routes is the init patch, with no allocation;
  - a `Scope::global` compile keeps exactly the global routes (for M1.7);
  - task 2's 400 random patches: a voice compile equals today's compile with the global routes removed.
- **CPU:** 16 global routes cost what no routes costs.

### What this pushes back on

- **Task 2's `compile()`** ([routing.hpp](../../libs/dsp/include/anoesis/dsp/modulation/routing.hpp)) gains a scope.
- **Task 3's late-route rule** (a source starts from its note-on state at the next grid point) now holds for every late route. Under (a), a global route reading the same LFO could exempt it.
- **M1.9's open question** ("task 5 decides automation and CCs") is answered by recommendations 2–5.
- **The inventory:** §12 annotated (no modulation control in the default map), and M28–M30 new.

## The decision

Chosen by the project owner on 2026-11-23: every recommendation (Decisions log).

- **Global routes in the voice:** the voice compiles only its own routes. `compile()` takes a scope, and `RouteCompiler` compiles for one: `Scope::voice` in `dsp::Voice`, `Scope::global` in M1.7's engine, from the same 16 slots. Built in stage 2 ([As built](#as-built)).
- **M1.9:**
  - the 16 amounts are host-automatable floats from −10 to +10, linear, default 0, MIDI-learnable;
  - the 16 destination menus and the 4 misc source menus are preset state, stored by key, not host-automatable and not MIDI-learnable;
  - no modulation control has a default CC;
  - one 7-bit CC law for every bipolar knob, decided in M1.9 (M30).
- **M1.10:** each slot saved as {source key, destination key, amount}, the misc sources by key; a key a build doesn't know loads as "-" with its amount kept; Copy to Part leaves the modulation slots alone (M29).
- **M1.11:** the destination popup "-" and the 27 in three headed columns (Oscillators 12, Filters 6, Misc 9), the same in every slot; the source popup "-" and the 11; the slot shows the short label; double-click resets an amount to 0.
- **M1.7:** the engine compiles the global routes, sums them on the absolute 32-sample grid from the sources' global copies, crossfades any change of them over 10 ms as the voices do, and clamps each effect knob plus its modulation to its range before its law.
- **M1.6:** no change.

**The inventory (M28–M30):** M28's menus are our own choice, made for Phase 2's rows, whatever the demo shows; M29 is our working hypothesis; M30 goes to M1.9.

## As built

Stage 2 (2026-11-23) built recommendation 1 in `libs/dsp`. The rest are requirements now recorded on M1.6, M1.7, M1.9, M1.10 and M1.11 in [PLAN.md](../../PLAN.md).

- **`compile(slots, scope)`** ([routing.hpp](../../libs/dsp/include/anoesis/dsp/modulation/routing.hpp)) keeps the live routes of one scope, in slot order. `compile(slots)` still keeps every route: both go through one loop, which checks the destination row's scope after the liveness test, so `compile(slots)` is unchanged.
- **`RouteCompiler`** ([route_compiler.hpp](../../libs/dsp/include/anoesis/dsp/modulation/route_compiler.hpp)) takes its scope at construction, `Scope::voice` by default, and `scope()` reports it. `dsp::Voice` names it: `RouteCompiler{Scope::voice}`. M1.7's engine will hold one with `Scope::global`.
- **`sameRoutes(a, b)`** ([routing.hpp](../../libs/dsp/include/anoesis/dsp/modulation/routing.hpp)): the same routes in the same order, each amount to the bit. `dsp::Voice` starts its crossfade only when a recompile gives different routes.

### What differs from stage 1, and why

**Found: a recompile that gave the same routes still started a crossfade.** Every `Patch::set()` bumps the patch's version, so a voice recompiles at its next grid point after *any* slot changes. Before stage 2, the voice treated every recompile as a change of its routes and started its 10 ms crossfade. With the scope filter alone, a change to a global slot therefore still started a crossfade in every voice. That crossfade blended identical sums, so it was exact in itself, but it held back the next real change for up to 10 ms: the −14.1 dBFS of stage 1's delayed-change case was still there. The same held for a per-voice slot re-sent unchanged, as host automation and preset reloads may do.

Stage 1's reference, (b), is the patch with its global routes removed, so it never bumps the version for them. Matching it needs the voice to ask whether the routes changed, not whether it compiled. `RouteCompiler::update()` keeps its meaning (true when it compiled, pinned by task 2's tests). The voice compares the routes before and after with `sameRoutes()`. The same fix went into task 4's prototype (`DestVoice`), so the library and the prototype still agree to the bit wherever task 4's tests compare them.

Task 4's decision was "a crossfade of any change of the routes", and a slot set to what it holds is no change of the routes, so this fix is that decision built as written.

### Tests (stage 2)

[global_routes_test.cpp](../../tests/dsp/modulation/global_routes_test.cpp), 8 cases (`[modulation]`), and two compile-time checks:

- **`compile()` by scope:** on 400 random patches, each scope's compile equals `compile()` of the slots with the other scope's routes emptied, and the two counts add up to `compile()`'s. A `static_assert`: a global route is no route to a voice, and the engine's compile keeps it.
- **`RouteCompiler`:** the voice's scope by default and the engine's on request, each the matching `compile()`, with no allocation.
- **`sameRoutes()`:** a `static_assert` that a route's amount, source, destination and count each matter, and that a route moved to another slot or a global route added doesn't. On 400 random patches it agrees with the test's own comparison.
- **The voice is the patch without its global routes, to the bit, at host blocks of 1, 37 and 4096:**
  - only global routes, with a Noise slot and an S&H LFO (no allocation);
  - the busy patch with global routes;
  - a global change 97 samples before a per-voice change (stage 1: −14.1 dBFS);
  - a late route on an LFO that a global route reads, on a sine and on S&H (stage 1: −26.0 and −21.6).
- **A slot re-sent unchanged** is the patch without the re-send, at host blocks of 1, 37 and 4096.
- **An amount's change alone still crossfades:** Mix A from −5 to −10 reaches exact silence on the crossfade's last sample, not at the grid point.
- **The report** ("[global-routes-report]") now prints the library against the reference: no difference in any case. Stage 1's numbers come from commit a455b3e.

**Red first:** against inert stubs that ignored the scope, the `static_assert` failed and 4 of 5 runtime cases failed (the init-patch case passed, as stage 1 found it would). The re-send case failed before the `sameRoutes()` fix.

**Deliberate breaks:** 8 tried, 7 caught (by a `static_assert` or by 2–17 failing cases):
- `compile()` ignoring the scope;
- `RouteCompiler` ignoring it;
- the voice compiling global routes;
- the voice without `sameRoutes()`;
- `sameRoutes()` ignoring the amount (caught only after the amount-only crossfade test and the `static_assert` were added);
- ignoring the source;
- ignoring the count (caught only after a prefix case was added).

The eighth, `sameRoutes()` ignoring `routed` and `used`, is equivalent, since both follow from the routes. The comparison was removed.

All 58 `[modulation]` cases pass, task 3's and task 4's comparisons with their prototypes included.

### CPU (stage 2)

One held voice, "[global-routes]", 4 runs, medians ([baselines](../performance/baselines.md#2026-11-23-m15-global-routes-as-built)):

| Routes | stage 1 (a) | as built |
|---|---:|---:|
| none | 130.3 | 128.6 |
| 4 global routes | 129.6 | 128.3 |
| 16 global routes | 134.3 | 127.7 |
| 16 global routes, a change at every grid point | 139.0 | 130.6 |

Global routes now cost what no routes costs. The last row still recompiles and compares at every grid point (+2 ns). `compile()` for 16 routes costs 22.4–23.3 ns, against 22.30 before.

## Tests

- [global_routes_test.cpp](../../tests/dsp/modulation/global_routes_test.cpp): above.
- [destination_benchmark.cpp](../../tests/dsp/benchmarks/destination_benchmark.cpp): "[global-routes]".

## References

- Sylenth1 user manual v2.5: p. 10 ("3.4 MIDI control"), p. 11 (MIDI learn), pp. 19–20 (the modulation section), pp. 31–32 ("6.1 MIDI Control Changes").
- The inventory, §6 and §12 ([sylenth1-features.md](../reference/sylenth1-features.md)).
- JUCE `AudioParameterChoice`: a choice's normalised value is its index over the number of choices less one.
