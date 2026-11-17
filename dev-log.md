# DOOF — Dev Log

Running record of sessions, decisions, thought process, and phase progress.
Each session gets an entry. Reference this alongside `project-reference.md` to understand *why* the code looks the way it does.

---

## How to use this file

- **One entry per work session.** Start a new `### Session YYYY-MM-DD` block at the top of the Sessions section.
- **Log decisions made** (especially anything that overrides or extends `project-reference.md`).
- **Log thought process** for non-obvious choices so the next session picks up cleanly.
- **Log open questions** discovered mid-session that didn't get resolved.
- **Link to commits** when a phase verify test passes.
- This file is read by Claude at the start of each session alongside `project-reference.md`.

---

## Phase Status

| Phase | Name | Status | Verify passed? |
|---|---|---|---|
| 0 | Project scaffolding | Complete | ✅ |
| 1 | Engine skeleton + one audible sub | Complete | ✅ |
| 2 | Node-based envelope engine | Complete | ✅ |
| 3 | Envelope canvas (GUI) + minimal preset save/load | Complete | ✅ |
| 4 | Layers + mixer | Complete | ✅ |
| 5 | Click layers | Complete | ✅ |
| 6 | Sub tuning + phase tools | Complete | ✅ |
| 7 | FX suite + routing matrix | In progress | — |
| 8 | EQ + spectral analyzer | Not started | — |
| 9 | Master limiter + output | Not started | — |
| 10 | Triggering | Not started | — |
| 11 | Preset bank, export/drag-out | Not started | — |
| 12 ★ | Skinning + layout polish (TARGET) | Not started | — |
| 13 | Import & Analyze (optional) | Not started | — |
| 14 | Hardening & release prep | Not started | — |

---

## Cumulative Decision Log

Decisions made *during build* that supplement or override `project-reference.md §8`.
(Decisions already locked in §0/§0.5/§8 of the reference doc are not duplicated here.)

- **2026-11-16 — Phase 6 design decisions (user's choices from four options put to them):**
  - **Harmonics are a series**, not a single overtone: Gain = first harmonic's level vs the fundamental, Ratio = falloff between successive harmonics, Decay = a real fade duration, presets All / Odd / Even / Octaves (up to the 8th).
  - **Keytrack reference = a settable `master.keyRoot`, default C3 (MIDI 60)**, chosen because Ableton's Drum Rack sends C3 to a pad's instrument by default. Closes §8's "keytrack reference note".
  - **Pitch snap = nearest equal-tempered note**, measured in cents. Snapping works on drawn values at A4 = 440; master tuning is applied to the Sub's output, so snapped nodes play on the tuned grid without double tuning.
  - **Phase lock = smooth correction**: a Hann-shaped pitch bend over up to 100 ms before the lock node, solved exactly from the same per-sample arithmetic the voice will run. Rotation sets the phase at the lock node when there is one, else at the note start.
- **2026-11-16 — Phase 6 decisions made in the build** (not put to the user, recorded so they can be revisited): master tuning moves every Sub node (±100 cents); tuning, key root, harmonics, sustain and phase are latched at note-on like the click controls; the sustain loop runs first-to-last keytracked node with a 10 ms crossfaded wrap, and a release inside the crossfade completes that wrap; the keytrack weight between a flagged and an unflagged node blends linearly in time; Snap to Note is a per-layer view preference like Log Scale; the editor grew to 800 x 700; state version 4.
- **2026-11-17 — Phase 7 design decisions (user's choices from four options put to them; closes §7.1/§8's FX questions):**
  - **6 module slots per FX channel** (Insert A, Insert B, Master: 18 slots). More can be added later without breaking presets; never fewer.
  - **Free reordering by drag-to-swap**: dropping a module on any other slot (any channel) swaps the two, settings and macro assignments included.
  - **Both compressor forms**: a full single Compressor module, and a Dual Compressor module (two stages in series in one slot, three controls each within the slot's six).
  - **Routing "Both" = parallel at full level**: the layer feeds Insert A and Insert B unattenuated and their outputs sum into Master. Per-insert output levels (`fx.insertA.level`, `fx.insertB.level`) balance it. "Dry" skips the inserts but still passes through Master.
- **2026-11-17 — Phase 7 decisions made in the build** (not put to the user, recorded so they can be revisited):
  - The slot type list is fixed at 14 entries for good (a Choice's entry list is part of its range): None, Clip, Tube, Wave, Tape, Drive, Bit Crusher, Ring Mod, Filter, EQ, Compressor, Dual Comp, Delay, Reverb. §3.7's "EQ" is a 3-band slot EQ (two shelves and a bell = the slot's six controls); Phase 8's 4-band analyzer EQ is a separate thing.
  - Every slot has `type`, `on`, `mix` and six generic `p1`-`p6` parameters (§7.1's fixed generic slot parameters). Each module maps them to its own controls and units; picking a module in the GUI loads that module's defaults, and host automation of `type` leaves the six values where they are.
  - Oversampling is one global setting (`fx.oversampling`: Off / 2x / 4x / 8x) applied to the nonlinear modules (the five distortions and Ring Mod), using JUCE's linear-phase FIR half-band filters with integer latency. Linear-phase so every route can be delay-compensated exactly and stay phase-coherent with the others, which a kick's layers depend on.
  - Latency is a function of slot types and the oversampling setting only - never of bypass or mix - so toggling a module on/off never moves the kick in time. Parallel routes are delay-aligned inside the engine; the total is reported to the host.
  - Module objects are created on the message thread when a slot's type changes and handed to the audio thread lock-free, so an empty slot costs no memory (a 2 s delay line in all 18 slots would be ~55 MB at 192 kHz). A type change crossfades through the dry signal over 10 ms.
  - Master gain (`sub.gain`) is applied after the Master FX, then the DC blocker, which moves to the very end of the chain so distortion-generated DC is removed too. With every slot empty and every layer Dry the output is bit-identical to Phase 6.
  - Macros offset their targets: effective = base + depth x macro, clamped, with depth in -100%..+100% (§7.1's range + polarity in one value). Assignments are structured state saved with the preset (state version 5), not APVTS parameters.
- **2026-11-16 — Universal (Intel + Apple Silicon) build deferred until the product is finished** (Phase 14 release prep). Until then DOOF is built for the development Mac only, which is Intel x86_64; add `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` when the time comes.
- **2026-11-16 — The company behind DOOF is 4Eternity.** `COMPANY_NAME` is `4Eternity` (the vendor hosts list DOOF under) and `BUNDLE_ID` is `com.4eternity.doof`. The manufacturer code stays `ECsy`: JUCE derives the VST3 class ID from the manufacturer and plugin codes, so changing either would orphan DOOF in every saved set, and hosts never display it. If it is ever to change to match the new name, it has to happen before DOOF is used in sets anyone cares about.

---

## Open Questions (unresolved across sessions)

- **(2026-10-06) Should the factory sub's 2 ms attack be softened now that a Click layer supplies the transient?** The FFT showed the attack is a small transient of its own (98% of the sub's energy above 300 Hz is in its first 10 ms). On 2026-11-16 the stacked kick was judged cohesive with the attack unchanged; the question stays open by the user's choice.
- **(2026-11-16) The Ableton DAW pass is still open.** Done: DOOF stays in a saved Live 12 project. Not yet done: patch recall after a full quit, host automation, sample rates and buffer sizes, export/freeze, fast rolls and multiple instances, and the GUI checks no test covers (clicking a strip's own controls, real mouse/keyboard on the click panel, idle CPU). Not a gate on any phase; run alongside Phase 6.

---

## Sessions

---

### Session 2026-11-16 (continued) — Phase 6

**Goal:** Phase 6 — sub harmonics, selective keytracking, sustain mode, pitch snap, master tuning, phase rotate + lock/align.

**Built, step by step (one feature commit, tests included, per step):**
1. **Tuning + keytracking** (`ffdd07d`) — `master.tune`, `master.keyRoot`; an optional `keytrack` flag per pitch node; a keytrack weight table baked into the snapshot alongside the pitch and amp tables; `SubVoice` applies `2^(weight × semitones / 12)` and the tuning ratio per sample. The envelope clock became an integer sample count (same drift fix ClickVoice got).
2. **Sustain** (`6ed6ea5`) — `layerN.sub.sustain`; the engine handles note-offs for the first time (sample-accurate, Sub voices only, monophonic: an older key's release never cuts a newer note). Loop = first to last keytracked node, crossfaded wrap.
3. **Harmonics** (`c1d24a3`) — four parameters; Chebyshev recurrence (one sin + one cos per sample for the whole series); fade-out above 0.40–0.45 × sample rate so nothing aliases.
4. **Phase** (`df251a1`) — `layerN.sub.phase`; an optional `phaseLock` node flag (one per envelope); smooth correction.
5. **Pitch snap** (`dbc203a`) — `PitchSnap.h` (juce_core only), canvas drag/add snapping, a one-off menu item, a note-aware readout.
6. **GUI** (`a3e916e`) — `SubPanel`, Tune + Key Root (parses typed note names), Snap to Note toggle, Keytrack / Lock Phase Here in the node menu, canvas markers (keytrack rings, PHASE LOCK line, SUSTAIN LOOP band while sustain is on).
7. **State** (`faa9367`) — round trip of every Phase 6 setting; version 3 presets load with Phase 6 at defaults.
8. **Verify** (`a09427b`) — §6's four checks end to end.

**Issues found and fixed along the way:**
1. **Measurement, not code, three times — each traced before any threshold moved.** (a) Zero crossings on a pitch *sweep* depend on the oscillator's phase, which differs every sustain pass; a 20 ms window gave a 0.2% spread, so the loop-replay check compares 40 ms windows to 1% and separately requires the sweep to be visible. (b) A decaying harmonic measured through a rectangular window leaks into its neighbours' bins at about −50 dB, which made absent harmonics read as present; the Goertzel measurement is now Hann-windowed. (c) The phase lock looked inexact at 1e-4 until traced to the 1 ms pitch table: interpolating just past the lock node carries a little of the approaching segment's slope. A flat approach holds to 1e-5 (the lock itself is exact); a sloped one to 2e-3, documented in the test.
2. **A test mistake on note numbering** — expected E0 at MIDI 28 when 20.60 Hz is MIDI 16 (E-1 in Ableton's naming). The code was right.
3. **The Phase 4 canvas test hard-coded a node pixel for a 472 px canvas**, which the Sub panel changed. It now computes the pixel from the live canvas height, as every new canvas test does.

**Verify result (Phase 6 §6):** all four measured through the real processor/editor.

| §6 check | Result |
|---|---|
| Flag two nodes, play C1 then C2 — only flagged nodes shift; foundation stays | ✅ flagged plateau (drawn 400 Hz) plays 100 Hz on C1, 200 Hz on C2; unflagged foundation 50 Hz on both |
| Hold with sustain — loops between keytracked nodes, completes tail on release | ✅ still looping at 400 Hz at 1.5 s; after release the tail reaches 50 Hz and finishes |
| Pitch snap + drag — nodes land exactly on note frequencies | ✅ dragged node sits within 1e-6 cents of a note and plays it (and the tuned note at +20 cents) |
| Phase-lock, edit an earlier node, correlate against a reference bass tone | ✅ tail phase moved 0.0017° locked vs 170.9° unlocked; correlation with the reference tone > 0.999 |

Every new test was bite-tested (about 25 deliberate breaks across the eight steps, each failing the test aimed at it). 143 subtests across both binaries (72 + 71), 0 failures; all four targets build with zero compiler warnings.

**Hands-on check:** the Phase 6 Release build was installed in `/Volumes/External SSD/Ableton/VST3 Plug-In` (`894ed0d`) and checked in Live on 2026-11-17: it works. **Phase 6 marked complete on 2026-11-17.**

**Next steps:** Phase 7 — FX suite + routing matrix.

**Open questions this session:**
- The 1 ms pitch table limits how exactly a phase lock holds when the curve approaching the lock node is steep (≤ 0.05° in the tests). Inaudible, but a finer table or exact segment evaluation near the lock node would remove it if it ever matters.

---

### Session 2026-11-16

**Goal:** Get DOOF into Ableton Live for real use, under the 4Eternity name.

**What happened:**
1. **Company name** — `COMPANY_NAME` is now `4Eternity`, `BUNDLE_ID` `com.4eternity.doof`. Manufacturer/plugin codes (`ECsy`/`Doof`) unchanged on purpose — see the Cumulative Decision Log.
2. **Release VST3 installed** to `/Volumes/External SSD/Ableton/VST3 Plug-In/DOOF.vst3` (Live's custom VST3 folder on the external drive), built from `cmake-build-release/` (gitignored). Live 12 Suite 12.4.16, Intel.
3. **Bug: every VST3 this repo built had a broken code signature.** With a Makefile generator JUCE ad-hoc signs the bundle, then writes `moduleinfo.json` into it, so `codesign --verify` failed with "a sealed resource is missing or invalid". JUCE only re-signs inside its copy step, which `COPY_PLUGIN_AFTER_BUILD FALSE` turns off. Fixed with a POST_BUILD step re-running JUCE's `checkBundleSigning.cmake` after the manifest; Debug and Release both verify. Found only because the installed bundle was checked — nothing in the test suite looks at the packaged plugin.

**DAW pass (carried from Phase 4) — first results, from the user in Live 12:**
- ✅ DOOF stays in a saved Live project and reloads with it.

**Phase 5 verify — final item:** ✅ the cohesive-kick check passed by ear (user, Live 12): a click stacked on the factory sub sounds like one kick. With the automated checks already green (2026-10-06 entry), **Phase 5 is complete.**

**Still open:** the rest of the DAW pass — patch recall after a full quit, host automation, sample rates and buffer sizes, export/freeze, fast rolls and multiple instances, and the GUI checks no test covers (clicking a strip's own controls, real mouse/keyboard on the click panel, idle CPU). These are host-level checks carried from Phase 4, not a Phase 5 gate.

---

### Session 2026-10-06

**Goal:** Finish Phase 5 — tests for the sample library, the Click layer's contextual panel, and the §6 verify checks — then bring the docs up to date.

**Earlier Phase 5 work, 2026-08-06 (reconstructed from the commits — that session left no log entry):**

1. **Sample-accurate MIDI dispatch** (`c1697f0`, `1c2aad5`) — `processBlock` used to drain the whole `MidiBuffer` before rendering, so every note-on fired at sample 0 of its block: up to 11 ms early at 512 samples. Inaudible on the sub's slow attack, which is how it survived four phases; fatal for a click. The block is now rendered in spans between events (`renderRange`).
2. **`ClickVoice`** (`1a64806`, `7f60606`) — four synthesised transients (tick, noise, snap, thump), a one-pole tone control, and an exact-duration decay. juce_core only, like `SubVoice`, so it tests in the lean target. Retriggering ramps the outgoing hit *in parallel* with the new one rather than choking first — a 5 ms serial choke is longer than the whole click and would throw away the sample-accurate onset. Testing caught two off-by-one bugs in the decay.
3. **Click parameters** (`f981177`) — per layer `click.type` (a fixed 8-entry list: 4 synthesised + 4 sample slots, never to grow), `click.tone` (0–1), `click.decay` (1–50 ms, skewed around the 8 ms default).
4. **Routing** (`d2b935e`, `64924a1`) — each layer ticks both voices and mixes the one its type selects. This pinned a bug that had been live since Phase 4: a Click layer rendered its *SubVoice*, so it sounded exactly like a Sub.
5. **Sample pipeline** (`8e2901a`, `bb68015`) — any `.wav` in `resources/clicks/` is embedded at build time and decoded once by `ClickSampleLibrary` on the message thread; `ClickVoice` plays it with linear-interpolated rate conversion. The folder ships empty on purpose (§8 licensing), and an empty slot is silent rather than a fallback.

**This session:**

1. **Sample tests** (`bf2bde5`) — `Phase5ClickSamplePlaybackTest` (lean target) drives `ClickVoice` from hand-built `ClickSample` structs: slot content, onset and natural end, decay not truncating a sample, pitch and length kept across rate conversion both ways, a ramp probe proving the interpolation is linear rather than drop-sample, tone as a low-pass that keeps DC, and the retrigger ramp on a sample. `ClickSampleLibraryTest` encodes WAVs in memory and checks exact float decode, 16-bit within one step, stereo folded to the average, refused input leaving the slot untouched, and the 10 s cap. `Phase5ClickSampleEngineTest` loads slots into the real processor: each Sample N choice reaches only slot N, with a sample-aligned onset.
2. **Click panel** (`d913a20`) — `src/ClickPanel.h/.cpp` replaces the placeholder whenever the selected layer is Click: a type list naming each slot's file or "(empty)", Tone/Decay/Level knobs, and a status line. One panel re-pointed at the selected layer by rebuilding its attachments, the same way the canvas is re-pointed. `Phase5ClickPanelTest` covers it.
3. **Verify tests** (`4fe119f`) — `Phase5VerifyTest`; results below.

**Bugs/issues found and fixed this session:**

1. **`Phase5ClickRoutingTest` (c) would have failed in any build that ships samples** — it asserted all four sample slots are silent, which is only true while `resources/clicks/` is empty. It now skips slots the build has filled. Same treatment for every new test that assumes an empty slot.
2. **`loadSlotFromMemory`'s comment was wrong** — it said a refused load "leaves the slot empty"; the code leaves it exactly as it was (it returns before touching storage). The code's behaviour is the right one, so the comment was corrected and the behaviour pinned.
3. **The editor's "DOOF" watermark showed through the panel**, and Tone/Level printed seven decimal places. Both found only by rendering the editor to PNG — the Phase 4 lesson again. Fixed with an opaque panel background and percentage text.

**Decisions made this session:**
- **Decay shows in ms, Tone and Level in %.** `ParamIDs.h` called the ms display a Phase 12 skinning concern; it is done now because it is display-only (installed after the attachment, which would otherwise overwrite it) and "0.0080000" in the flat skin was unreadable. Stored values and ranges are unchanged.
- **The panel's Level is the layer's own `level` parameter** — a second attachment beside the strip's slider, not a new parameter. §6's "per-click level" is the layer level, as `createParameterLayout` already decided.
- **Decay is greyed out, not hidden, for a sample slot** — samples play to their own end, and hiding it would make the layout jump as the type changes.
- **`DOOFAudioProcessor::getClickSamples()`** added — const for the panel's slot names, non-const so integration tests can load a generated WAV through the real engine (`resources/clicks/` is empty, so there is no other way to reach a loaded slot).

**Verify result (Phase 5 §6):**

| §6 check | Result |
|---|---|
| Click onset sample-aligned to note-on | ✅ every synthesised type stacked on the sub, plus a loaded sample: silent before the note-on, sounding *on* it (stricter than the sub-only dispatch test, whose first sample is sin(0) = 0), bit-identical to the same note at sample 0 — including a note-on on a block boundary |
| Fast-trigger repeatedly — no clicks at sample boundaries | ✅ a dense retrigger pattern (37-sample bursts, hits on block boundaries, a doubled hit) renders **bit-identically** at 1, 64, 333, 512 and 1024-sample blocks; the outgoing hit ramping rather than cutting is pinned at voice level for both synthesised hits and samples |
| FFT: transient energy high, sub energy low | ✅ 65536-point FFT split at 300 Hz: the sub has 99.96% of its energy below; the clicks have 97.6% (tick) to 99.8% (snap) above; the sub leads the low band by 45–53 dB |
| Stack a click on a sub — one cohesive kick | ✅ by ear — confirmed by the user in Live 12 on 2026-11-16 |

Every new test was bite-tested by breaking the code it guards: drop-sample interpolation, no rate conversion, every choice reading slot 0, a summing stereo fold, the panel pinned to layer 1, the poll not refreshing the panel, no ms conversion, a DC blocker reset per block, and a note-on dispatched one sample late. Each failed the test aimed at it.

94 subtests across both binaries (43 + 51), 0 failures; `DOOFTests`, `DOOFIntegrationTests`, `DOOF_VST3` and `DOOF_Standalone` build with zero compiler warnings.

**Worth knowing from the FFT:** above 300 Hz the click leads the sub by only 2.4 dB (tick) to 9.8 dB (thump). Traced rather than assumed: 98% of the sub's high-band energy is in its first 10 ms — the factory amp envelope rises 0→1 in 2 ms, which is a small transient of its own — and none comes from the tail. The sub is 32 dB louder overall than a tick, so even 0.04% of it is comparable. Not a defect, but relevant to the by-ear check: the default sub already has some click in it.

**Still outstanding:** the by-ear cohesive-kick check, then Phase 5 can be marked complete. The DAW pass carried over from Phase 4 also still applies. *(By-ear check passed 2026-11-16; Phase 5 complete — see that entry.)*

**Next steps:** Phase 6 — sub tuning and phase tools.

**Open questions this session:**
- Should the factory sub's 2 ms attack be softened now that a Click layer exists to supply the transient? It is a sound-design call, best made during the by-ear check. *(2026-11-16: still open, by the user's choice. The stack was judged cohesive with the attack unchanged — tracked under Open Questions.)*
- The Phase 4 GUI tests log `juce_Component.cpp:3022` assertions — the editor calls `grabKeyboardFocus()` in its constructor while not on screen. Harmless headless and not a test failure, but noise in the log; a `isShowing()` guard would silence it.

---

### Session 2026-08-02

**Goal:** Phase 4 — five layers, per-layer type/level/mute/solo, multi-solo per §3.3, the horizontal layer selector, and swapping the contextual panel with the selected layer's type.

**Phase 4 built, step by step:**

1. **Audibility rule first, in isolation** — `src/LayerAudibility.h/.cpp`: §3.3's truth table as a pure function, deliberately free of any JUCE dependency so the engine, the GUI's dimming, and the lean `DOOFTests` target all call the *same* function. This is what makes it structurally impossible for the displayed state to disagree with what is audible.
2. **Parameters** — `src/ParamIDs.h` (shared, so nothing builds ID strings by hand) adds 20 parameters: per layer `type` (Off/Sub/Click), `level`, `mute`, `solo`. `sub.gain` is unchanged at 0.8 and the defaults are chosen so the out-of-the-box patch is bit-identical to pre-Phase-4 — every Phase 1/2/3 render assertion stays valid verbatim.
3. **Five layers** — `src/Layer.h` bundles one layer's pitch model, amp model, publisher and voice. Held as `std::array<std::unique_ptr<Layer>, 5>` because `Layer` is neither default-constructible nor movable (its publisher holds references to the two models beside it). One publisher per layer, so editing a layer rebuilds two lookup tables rather than ten.
4. **Mixer** — `processBlock` sums all five layers through smoothed per-layer gains driven by the audibility rule. 5 ms ramp, matching `SubVoice`'s own choke fade.
5. **State** — preset/session state re-nested under `LAYER { index }` children with an explicit root `version`. Layer view preferences added at version 3.
6. **Layer selector** — `src/LayerStrip.h/.cpp`, one self-contained cell per layer (§0.5: panels have no opinion about where they sit), all controls APVTS-attached.
7. **Canvas retargeting + contextual panel** — the canvas points at the selected layer; Sub shows it, Click and Off replace it with a placeholder.

**Bugs/issues found and fixed during Phase 4:**

1. **Seeding filled the entire undo history** — the factory patch went through the normal undoable edit path, leaving ~70 transactions across ten models. That both filled the whole 30-step cap with factory setup and let Cmd+Z at startup dismantle the default patch. Latent since Phase 3 at two models; five layers made it 5× worse. Fixed with `clearUndoHistory()` after seeding.
2. **A gesture could be orphaned by a layer switch** — a gesture is begun in `mouseDown` on one model and ended in `mouseUp` on whatever `modelFor()` returns *by then*. Switching layers mid-drag left the outgoing model inside an open transaction forever, and since `addNode` skips `beginNewTransaction` while in a gesture, **every later edit on that layer silently coalesced into it** — one Cmd+Z would then discard an unbounded amount of work. Fixed with `cancelActiveGesture()`, which runs before the pointers move so each gesture ends on the model it began on.
3. **Mute was invisible** — the default `TextButton` on/off colours are near-identical at this size, so a muted layer looked exactly like an unmuted one. Found only by rendering the editor to a PNG and looking at it; no unit test could have caught it, since the toggle *state* was correct throughout. Fixed with explicit red/amber on-colours.
4. **Non-ASCII in string literals** — an em-dash and an en-dash inside `expect` messages tripped JUCE's `CharPointer_ASCII` assertion (`juce_String.cpp:327`), which fires on any non-ASCII `char*` since JUCE can't infer the source encoding. Both pre-existing. Comments are unaffected — only literals matter.
5. **`resetLayerParametersToDefaults` was dead code** — added under the belief that JUCE keeps a parameter's *current* value when the loaded state omits it. It doesn't: `updateParameterConnectionsToChildTrees` appends a bare `PARAM` child, and `valueTreeChildAdded` → `setNewState` then reads `getProperty(value, getDenormalisedDefaultValue())`, falling through to the default. Confirmed empirically, then removed. **The view preferences are the genuine exception** — nothing resets those, so their explicit reset on the legacy path is load-bearing.

**Decisions made this session:**
- **A soloed *Off* layer does not count as soloed.** §3.3 says only "≥1 layer soloed" and doesn't address type; the literal reading would silence the whole patch with no visible cause. One line plus two assertions to flip.
- **"Dimmed" is narrower than "silent."** §3.3 reserves dimming for a layer silenced *because of someone else's solo*; a muted or Off layer already shows why through its own controls. Expressed as `!isAudible(flags, anySoloed) && isAudible(flags, false)` — a difference of the shared rule, so it cannot drift from the mixer's view of what silences a layer.
- **Layers are summed straight, not divided by the active count** — dividing would change every other layer's sound whenever one was enabled. Five layers at unity therefore exceed full scale, which is arithmetic, not a defect; the master limiter that tames it is Phase 9.
- **Zoom/pan is camera state, not layer data** — it stays put across a layer switch, so two layers can be compared at the same time window.
- **Log scale and editing curve are per layer** (user decision), which is what took the preset schema to version 3.
- **View preferences live in the processor, not the editor** — the host may close and reopen the editor at any point, and they persist in the preset.
- `JUCE_MODAL_LOOPS_PERMITTED=1` on the **test target only**, so tests can pump the message loop and observe the ~30 fps GUI polls actually firing. Never on the `DOOF` target — modal loops inside a plugin are a hosting hazard.

**On verification method (worth carrying into Phase 5):**

Every test written this phase was checked by deliberately breaking the production code and confirming the failure. That caught **four tests that passed while proving nothing**, none of which would have been found by running the suite normally:

- An undo test that asserted on node counts — the shared `UndoManager` meant `undo()` popped a *different* model's transaction, so the assertion held either way.
- A superposition test blind to every layer reading layer 0's voice: that voice is advanced once per layer per sample, so each layer-alone render picks off a different one of five consecutive samples — genuinely different signals that still sum consistently. **Self-consistent assertions cannot catch this**; an absolute check (each layer given a higher pitch must show strictly more zero crossings) is what closes it.
- Two tests that mutated state before asserting, and so could never see initialisation bugs. A GUI is always constructed against state that already exists — plugin open, preset load, session restore — so **every component needs at least one assertion that touches nothing first**.
- A Phase 3 preset load that didn't check view preferences, leaving the one genuinely load-bearing reset untested.

Two smaller lessons: a "nothing changed" refactor claim deserves a real null test (the canvas pointer refactor was confirmed byte-identical in both its rendered output and a scripted interaction fingerprint), and **rendering the editor to a PNG via `createComponentSnapshot` needs no window and no new tooling** — it is how the invisible-mute bug was found, and how §6's "the rest visibly dim" was actually checked rather than assumed.

**Verify result (Phase 4 §6):** All green.

| §6 check | Result |
|---|---|
| Audibility function against all 16 truth-table combinations | ✅ table written out by hand, not derived from the implementation |
| Solo layers 1 and 3 — only those audible, the rest visibly dim | ✅ engine test + confirmed by rendered screenshot |
| Mute on a soloed layer — silent (mute wins) | ✅ |
| All layers at unity — no unexpected clipping | ✅ mix equals the sum of the parts; asserts peak > 1.0 so the test is genuinely in the clipping regime |
| Click between layers while a note rings — no dropout | ✅ stated as a null test: the render is bit-identical with and without switching |

57 subtests across both binaries, 0 failures, clean build. **Not covered by automated test:** that the ~30 fps polls skip their repaint when nothing changed (an unparented component never paints), and that a click landing on a strip's *child control* also selects the layer (needs real event dispatch).

**Still outstanding:** the DAW pass for host-dependent behaviour (parameter automation from the host, session save/restore through the host rather than through `.doof`).

**Next steps:** Phase 5 — Click layers. The contextual panel already has its placeholder in place.

**Open questions this session:**
- Storing view preferences in the preset means a shared preset carries its author's log-scale setting. Fine for personal patches, mildly odd for factory content. Splitting session state from preset state is real work since `getStateInformation` is the same blob for both — revisit at Phase 11 when the preset browser exists.

---

### Session 2026-07-28

**Goal:** Phase 2 build — node-based envelope engine, Bezier evaluator, lookup-table precompute, atomic-pointer snapshot publish, and wiring SubVoice/PluginProcessor to it.

**Phase 2 built, step by step:**

1. **Envelope node data model** — `src/EnvelopeModel.h/.cpp`. `EnvelopeIDs` namespace defines the `ValueTree` schema (`ENVELOPE` > `NODE { time, value, cpOutTime, cpOutValue, cpInTime, cpInValue }`). `EnvelopeModel` wraps it with `addNode`/`moveNode`/`deleteNode`/`setControlPoints`, each one undo step via a private `UndoManager`. Nodes are full cubic-Bezier (two independent control points per node, chosen over a single-tension quadratic scheme per explicit user decision). Moving a node shifts its control points by the same delta so curve shape travels with it.
2. **Bezier evaluator + lookup table** — `src/BezierSegment.h/.cpp` (pure math, no JUCE dependency: De Casteljau point-at-`t`, plus bisection-based time→value inversion) and `src/EnvelopeEvaluator.h/.cpp` (`buildTable`: walks all segments into a 4096-sample table over a 4 s domain; holds flat before the first/after the last node).
3. **Immutable snapshot + atomic publish** — `src/EnvelopeSnapshot.h` (immutable pitch+amp tables plus domain/resolution metadata) and `src/EnvelopePublisher.h/.cpp` (`ValueTree::Listener` on both models → rebuild → `std::atomic<const EnvelopeSnapshot*>::exchange`; old snapshots retired via double-buffered `juce::Timer` at 50 ms, not freed inline).
4. **Wired to the audio thread** — `SubVoice` now reads pitch/amp via interpolated table lookups against a snapshot handed in through `setSnapshot()`, called once per block. `PluginProcessor` owns `pitchEnvelopeModel`, `ampEnvelopeModel`, and `envelopePublisher` (in that declaration order — the publisher holds references to the models). `src/DefaultEnvelopes.h/.cpp` seeds the out-of-the-box shape (monotonic 150→50 Hz pitch sweep, linear-attack/near-silent-by-400ms amp), shared between production and the Phase 1 regression tests so they stay meaningful.

**Bugs/issues found and fixed during Phase 2:**

1. **Doc comment error** — `EnvelopeModel::addNode`'s default (coincident) control points don't give a "gently-eased" curve as originally documented; they're mathematically exactly linear in time (both time and value are driven by the same nonlinear Bezier-parameter function, which cancels out when eliminated between them). Comment corrected.
2. **`juce::Timer` needs a `MessageManager`** — the headless `DOOFTests` console app never created one, so `EnvelopePublisher`'s retirement timer silently never fired (tripped a JUCE assertion, and left a `ShutdownDetector` leak at exit). Fixed with `juce::ScopedJuceInitialiser_GUI` at the top of the test binary's `main()` — JUCE's own documented pattern for console apps touching such classes.
3. **Idle-detection generalization** — Phase 1's `kAmpAttackSec`-based idle guard only made sense for the one hardcoded envelope shape. Replaced with a `hasExceededIdleThreshold` flag so it works for arbitrary table-driven envelope shapes.

**Decisions made this session:**
- Cubic Bezier with two independent control points per node (not a single-tension quadratic scheme) — more GUI work in Phase 3, more expressive.
- Table domain fixed at 4 s / 4096 samples for now; revisit if Phase 3's Length control needs more.
- Snapshot reclamation via double-buffered `juce::Timer` retirement (not `shared_ptr` atomics, which use an internal lock JUCE's implementation isn't guaranteed lock-free on the audio thread).
- `CMAKE_EXPORT_COMPILE_COMMANDS` enabled project-wide so editor tooling resolves JUCE includes.

**Verify result:** All Phase 2 unit tests green (28 test cases across `EnvelopeModel`, `EnvelopeEvaluator`, `EnvelopePublisher`, plus the re-verified Phase 1 suite) via both the direct binary and `ctest`. `DOOF_VST3` and `DOOFTests` both build clean, zero warnings.

**Phase 3 started — Step 0 (throwaway canvas spike):**
- Built a disposable `SpikeCanvas` (file-local class in `PluginEditor.cpp`, not shipped) with two draggable dots over its own private `EnvelopeModel`, temporarily wired into the Standalone editor. Manually verified by the user in the built Standalone app.
- **Bug found (JUCE lifecycle):** the spike component was invisible/zero-size at first — `setSize()` was called before the child component was added, but `setSize()` is what triggers the initial `resized()` layout pass, so a child added afterward never gets bounds set. Fix: add all children *before* calling `setSize()`. General JUCE gotcha worth remembering for the real canvas and any future editor components.
- **Design lesson found (the actual point of the spike):** `EnvelopeModel::moveNode` can return a *different* index than the one passed in, since it re-sorts nodes when a drag crosses a neighbour's time. The spike initially discarded that return value, so after a crossover the tracked "dragged node" index went stale and subsequent drag updates silently moved the wrong node. **Requirement for Step 3's real drag implementation: always reassign the tracked node index from `moveNode`'s return value, every call, for the whole gesture — never assume the index is stable.**
- Spike code fully reverted (`PluginEditor.h/.cpp` back to pre-spike state) once it had taught us both lessons above — it was never meant to ship.

**Phase 3 built, step by step (continuing this session):**

1. **Shared undo history + gesture API** — `EnvelopeModel` takes an optional external `UndoManager*` (null → owns its own, preserving Phase 2 behaviour). `PluginProcessor` owns one shared `UndoManager` (capped at 30 transactions) passed to both `pitchEnvelopeModel` and `ampEnvelopeModel`. `beginGesture()`/`endGesture()` let a whole continuous edit (a drag) coalesce into one undo step. Unit-tested: multi-move-in-gesture undoes as one step, non-gesture edits keep their own steps, undo across pitch/amp is chronological.
2. **Canvas rendering** — `EnvelopeCanvas` (new): one canvas overlays pitch (cyan) + amp (orange) curves on a shared time axis, each normalized into its own value range (§3.4: "Central canvas: pitch + amp curves"). Beat grid reads host BPM via `AudioPlayHead`, redrawn on tempo change. Log/Linear vertical-axis toggle for the pitch curve only (amp always linear — its range includes exact 0, where log is undefined); a pure rendering-coordinate change, provably unable to touch the model or audio.
3. **Node interaction** — click-drag moves a node (gesture-wrapped, with a live value/frequency readout); double-click empty space adds a node to whichever curve is selected in an "Editing: Pitch/Amp" dropdown; double-click or right-click a node deletes it (right-click later became a context menu, see below); click-drag the curve line between nodes reshapes its Bezier control points (De Casteljau basis-weight math keeps the curve passing through the cursor), Shift held for fine-drag; right-click anywhere opens a context menu (Delete Node when on a node, Reset Curve, Copy Curve, Paste Curve — Paste added ad hoc mid-session, not originally planned, enabling cross-curve pitch↔amp shape copying via the system clipboard). Cmd+Z/Cmd+Shift+Z plus on-screen Undo/Redo buttons.
4. **Zoom & pan** — mouse wheel zooms centered on the cursor; left-drag on empty canvas space (unclaimed by any other gesture) pans. Both are pure view-window transforms — verified by direct code inspection that neither code path calls any `EnvelopeModel` mutator.
5. **Length control** — a per-model "Length" (work-area duration) stored as a `ValueTree` property, undoable, adjustable via both a draggable canvas handle and a numeric field (kept in sync both ways). Node add/move is now bounded to `[0, Length]` instead of the full 4 s table domain. Initial camera view on load = `max(pitch length, amp length)`.
6. **Repaint strategy** — a 30 fps timer polls host tempo and repaints only when it's actually changed (most ticks do nothing — the beat grid previously only updated when an edit happened to trigger a repaint). `EnvelopeCanvas` is now opaque with its own background fill, so its repaints no longer require the parent editor to redraw behind it.
7. **Minimal preset save/load** — `getStateInformation`/`setStateInformation` now nest both envelope models' trees (tagged by a `curve` property, since both share the same `ENVELOPE` type) alongside `apvts` state, under one `DOOFState` root. "Save…"/"Load…" buttons write/read the identical serialised state to/from a `.doof` file via `FileChooser`.
8. **Final verify pass** — see below.

**Bugs/issues found and fixed during Phase 3:**

1. **`EDITOR_WANTS_KEYBOARD_FOCUS FALSE`** (set back in Phase 0, before any keyboard interaction existed) was blocking some VST3 hosts from ever routing key events to the editor — Cmd+Z did nothing. Flipped to `TRUE`.
2. **Curve hit-test measured distance to discrete sample points, not the drawn line** — with only 32 samples per segment, most of the visible curve fell in gaps too far from any sample point for a tight hit radius to catch, so clicking directly on the visible line often didn't register. Fixed to measure point-to-segment distance against the same piecewise-linear segments the path is actually drawn with.
3. **`EnvelopeModel::setState()` reassigned `tree` (`tree = newState`) instead of mutating it in place.** A plain reassignment doesn't fire any `ValueTree::Listener` callback, which silently orphaned `EnvelopePublisher`'s listener (attached once, in its constructor, to whatever object `tree` referenced at that time) — **after any preset load, the audio thread would stop receiving snapshot updates entirely**, including for edits made after the load, even though the canvas kept displaying them correctly (it reads the model directly, not through the publisher). Found by the Step 7c integration test itself — exactly the kind of bug a full render-based test is for. Fixed by mutating the tree in place (`removeAllChildren` + `copyPropertiesFrom` + re-adding children).
4. **JUCE's macOS Standalone wrapper never calls `AudioProcessor::setPlayHead()`** — that only happens under `JUCE_IOS` for Inter-App Audio — so `getPlayHead()` is always null in Standalone on this platform. The beat grid can only be verified in a real DAW host, not Standalone (the plan's own Step 8a note assumed otherwise).
5. **`juce::Label` defaults to black text**, invisible against the dark editor background (unlike `TextButton`/`ComboBox`, which default to a visible colour under this LookAndFeel) — the Length fields were invisible until explicit colours were set.

**Decisions made this session (Phase 3):**
- One canvas overlaying both curves (not two separate `EnvelopeCanvas` instances), matching §3.4's "Central canvas" literally over Step 2a's own more ambiguous wording.
- Right-click opens a context menu everywhere (on a node: adds "Delete Node"; off a node: just Reset/Copy/Paste), superseding 3b's original instant-delete-on-right-click.
- "Reset Curve" restores the *whole* curve to its `DefaultEnvelopes` shape, not just the clicked node/segment.
- Panning is plain left-drag on empty canvas space, no modifier key — that gesture was otherwise unclaimed.
- Added a second CTest target, `DOOFIntegrationTests`, for tests needing the full `PluginProcessor`/`PluginEditor`/`EnvelopeCanvas` chain (and therefore the full `audio_utils`/`audio_processors`/`gui_basics`/`dsp` dependency chain) — keeps `DOOFTests` lean and fast for day-to-day iteration (per its own long-standing design intent) while giving full-engine tests like the save/reload null-match a permanent, re-runnable home. Expected to be reused by later phases needing similar full-engine checks.

**Verify result (all six §6 Phase 3 checks):**
- Audible pitch-drag: confirmed manually (drag a pitch node down while triggering notes — pitch audibly drops, curve follows the cursor).
- Log/lin null test: verified by code inspection — `setPitchLogScale` only sets a bool and repaints, never touches either model.
- BPM grid alignment: confirmed manually in a real DAW at 128 and 178 BPM — gridline shifts to the correct position each time.
- Zoom/pan node count: verified by code inspection — `mouseWheelMove` and the panning branch of `mouseDrag`/`mouseUp` only touch `viewStartTime`/`viewDuration`/`clampView()`, never any `EnvelopeModel` mutator.
- Undo-one-step-reverts-whole-drag: permanent unit tests (`EnvelopeModelTest` (i)/(j)/(k)) plus manual confirmation via the Undo button and Cmd+Z.
- Save/reload null-match: new permanent `Phase3PresetRoundTripTest` in `DOOFIntegrationTests`, passing.

`DOOFTests`: 26/26 passing. `DOOFIntegrationTests`: 1/1 passing. `DOOF_VST3` and `DOOF_Standalone` both build clean, zero warnings.

**Next steps:** Phase 4 — Layers + mixer.

---

### Session 2026-06-16

**Goal:** Phase 0 verification + Phase 1 build.

**Phase 0 verified:**
- CMake builds VST3, Standalone, and DOOFTests targets cleanly.
- `ctest` passes (sanity test green).
- Phase 0 verify criteria all met before advancing.

**Phase 1 built:**

New files:
- `src/SubVoice.h` / `src/SubVoice.cpp` — monophonic sine voice with hardcoded pitch+amp envelopes and choke-on-retrigger (5 ms linear anti-click fade). State machine: Idle → Playing → Choking → Playing.
- `src/SubVoice.cpp` added to both the plugin target and the `DOOFTests` target.

Updated files:
- `PluginProcessor.h/.cpp` — APVTS wired (`sub.gain`, stable ID `"sub.gain"`); `prepareToPlay` prepares the voice and computes the DC blocker coefficient from the actual sample rate; `processBlock` iterates MIDI for note-ons, ticks the voice, applies gain + DC blocker, writes mono output to L+R.
- `getStateInformation` / `setStateInformation` now serialise/restore APVTS state as XML.
- `tests/BasicTests.cpp` — four Phase 1 verify tests added (RMS decay, pitch falls over time via zero-crossing, no NaN/Inf, no large jump at choke).

**Bugs found and fixed during Phase 1:**

1. **Immediate-idle bug** — `ampAt(0) = 0.0` (linear attack starts at zero), which was below `kIdleThreshold = 1e-4`, causing the voice to kill itself on the very first sample. Fix: guard the idle check with `envTime >= kAmpAttackSec`.

2. **Zero-crossing test window mismatch** — early window was 20 ms but late window was 50 ms. At lower frequency, the extra samples compensated and crossing counts were approximately equal. Fix: both windows set to equal 20 ms duration; early 2–22 ms (~150 Hz), late 200–220 ms (~58 Hz).

3. **Late-RMS threshold too tight** — original `lateRms < 0.02` at 500–1000 ms fails because the amp at 500 ms is still ~0.24. Fix: extended render to 3 s and used 2.5–3 s as the late window, where amp ≈ 2–8 × 10⁻⁴.

**Decisions made this session:**
- Block-accurate MIDI processing for Phase 1 (all events handled at block start, not sample-accurate). Sample-accurate triggering deferred to a later phase.
- DC blocker cutoff ~10 Hz, coefficient computed from actual sample rate in `prepareToPlay`.
- `sub.gain` is the first stable APVTS parameter (ID `"sub.gain"`, version 1).

**Verify result:** All 4 Phase 1 assertions green. Plugin and Standalone build clean with zero warnings.

**Next steps:** Phase 2 — node-based envelope engine (ValueTree data model, Bezier evaluator precomputed to lookup table, immutable atomic-pointer snapshot to audio thread).

---

### Session 2026-06-15

**Goal:** Project setup — understand scope, create dev log and memory system.

**What happened:**
- Read and internalized `project-reference.md` in full.
- Set up persistent Claude memory for this project:
  - `memory/feedback_read_project_architecture.md` — reminder to read `project-architecture.md` at the start of every session.
  - `memory/project_doof_overview.md` — DOOF overview, committed target (Phase 12), and key locked decisions.
  - `memory/MEMORY.md` — index.
- Created this dev log (`dev-log.md`) as the running record of sessions and thought process.

**Thought process / notes:**
- The project has no code yet — everything is in the design doc. Phase 0 is the first real build step.
- The three-layer separation (audio engine / view components / skin) and the real-time-safety rule are the two architectural pillars that every implementation decision will serve.
- The atomic-pointer-swap envelope snapshot is the trickiest correctness constraint early on — build the plainest correct version in Phase 2 before adding any cleverness.
- The voice state machine (Idle → Body → Sustain-loop → Release, with Choke from any state) should be drawn out on paper before Phase 1 code starts. It touches gate, sustain mode, and choke — tangled to discover in code.
- Phase 0 is intentionally trimmed: VST3 + Standalone first, AU/CI/golden-harness/multi-host deferred to Phase 14.

**Next steps:**
- Begin Phase 0: set up CMake project, JUCE dependency, VST3 + Standalone targets, unique plugin/manufacturer IDs, empty processor that loads, git repo, basic pluginval run, unit-test target.

**Open questions this session:** None new (all open questions are in `project-reference.md §8`).

---
