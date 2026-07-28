# DOOF — Kick Drum Synthesizer

**Project design & build reference — FINAL base file**

> A kick-drum synthesizer plugin built in JUCE / C++, in the spirit of Sonic Academy's Kick 3.
> Single source of truth for scope, architecture, decisions, and the phased build plan.
> Update it as decisions are made. Nothing here is built yet — this is the plan.

---

## 0. Snapshot of decisions made

| Decision | Choice | Notes |
|---|---|---|
| Plugin name | **DOOF** | |
| Formats | VST3, AU, Standalone | AU is macOS-only; AU added in Phase 14 |
| Platforms | macOS + Windows | |
| Framework | JUCE / C++ | |
| Primary goals | Deep understanding of synthesis + JUCE; portfolio piece; high quality | Push for polish, no shortcuts |
| **Completion target** | **Phase 12 or bust** — feature-complete *and* hardware-skinned | See §0.5. Phase 13 optional, Phase 14 = distribution polish |
| First skin | Flat "developer" skin | Build all functionality here first |
| Later skin(s) | Hardware-machine look (visual inspiration: Behringer TD-3, RD-8/9) | **Purely visual** — no step sequencer, stays a kick synth |
| Layer selector | Horizontal strip | Locked |
| Solo behaviour | Multi-solo (solo-in-place), additive | "Mute wins" on the both-set conflict (§3.3) |
| Voice behaviour | **Monophonic**; new hit chokes the previous tail (short anti-click fade) | Voice state machine in §3.0 |
| Output | **Mono core; stereo only after the FX/reverb stage** | FX domain (inserts + master) is stereo throughout (§7.1) |
| Envelope handoff | Precompute each envelope to a lookup table; publish an immutable snapshot to the audio thread via a single **atomic-pointer swap** | Audio thread never reads the live `ValueTree` (§2) |
| Parameter IDs | Stable, namespaced string IDs; never renamed or removed once presets exist | §2 |
| Preset format | A preset **is** the plugin's saved state blob, written to a file — no parallel format | §7.5 |
| FX swappable params | Fixed generic "slot parameters" per slot, mapped per module | §7.1 |
| Modulation scope | **Closed**: envelopes + the 4 FX macros only — no LFOs / mod matrix | Prevents scope creep |
| Layout modes | Build **paged first**; components position-agnostic so single-screen + a user toggle can be added later | Toggle exposure handled in Phase 12 |
| Import & Analyze | **Last** feature; project counts as complete without it | Hardest DSP on the list (§3.12) |

---

## 0.5 Project strategy & milestones

**The commitment is Phase 12: a fully featured *and* hardware-skinned DOOF.** Not the trimmed instrument — the whole thing, FX, EQ, limiter, triggering, presets, export, and the hardware skin. Phase 13 (Import & Analyze) is optional research beyond the finish line; Phase 14 is distribution polish for if/when DOOF ships.

That's an ambitious target for a solo build from a basic-C++ start, so the plan is structured to keep it reachable rather than letting it collapse late:

- **The phase order yields playable checkpoints the whole way.** Even though Phase 12 is the goal, DOOF makes sound at Phase 1, is editable at Phase 3, and is a complete-sounding multi-layer instrument by Phase 6. You're never far from something that works — momentum and motivation stay intact, and a stall never leaves you with nothing.
- **Prototype the hard centerpiece first.** The envelope canvas (Phase 3) is the biggest single jump. Build a throwaway spike of just the canvas before committing to the real one.
- **Start simplest-correct on the risky code.** The lock-free snapshot handoff is a subtle-bug magnet — implement the plainest correct version (one atomic-pointer swap to an immutable struct) before any cleverness.
- **Every phase has a conductable test** (see §6). Don't advance until the current phase's verification passes — that's what keeps quality high and bugs cheap as the codebase grows toward Phase 12.

---

## 1. What DOOF is

DOOF is a synthesizer that builds kick drums from editable envelopes rather than fixed samples. The core idea (inherited from Kick 3): a **Sub layer** is defined by two node-based envelopes — a **pitch** envelope (the fast downward sweep that gives a kick its "boom") and an **amp** envelope (the volume contour) — joined by Bezier curves. **Click layers** add the transient/attack using samples, noise, and texture. Multiple layers stack into a finished kick, which is then shaped by FX, EQ, and a master limiter, and triggered by MIDI or by an audio sidechain.

---

## 2. Architecture philosophy — the three-layer separation

Keep three concerns separate so each can change without breaking the others.

1. **Audio engine (the stable core).** All sound generation, the mixer, mute/solo logic, FX, limiter. Knows nothing about pixels. Source of truth.
2. **View components (the structure).** Each panel — envelope canvas, layer strip, FX rack, EQ — is its own self-contained `Component` that draws and resizes itself but has *no opinion about where it sits*. A layout manager arranges them. This is what makes paged-vs-single and resizing cheap.
3. **Skin (the surface).** A `LookAndFeel` subclass plus per-skin image/colour assets that govern *how* components draw. Swappable at runtime. This is what makes the flat→hardware skin swap cheap.

**Parameters** connect engine and view: JUCE's `AudioProcessorValueTreeState` (APVTS) holds every automatable parameter, links them to GUI controls, and handles save/restore of plugin state.

**Parameter IDs are forever.** Every APVTS parameter gets a stable, namespaced string ID (e.g. `layer3.sub.tune`). Once any preset or session has been saved, an ID is never renamed or removed — deprecate instead. Breaking an ID breaks every saved sound and every host automation lane.

**Parameters vs complex state — crossing the thread boundary.** Simple knob/toggle values live in APVTS, which is atomic and audio-thread-safe. The envelopes (node positions + Bezier data), routing, and FX-slot assignments are *structured* state held in a `ValueTree` — great for editing, undo, and serialization, but a `ValueTree` is **not** safe to read on the audio thread while the GUI edits it. The pattern: the GUI edits the model; on every edit we bake an **immutable, flattened snapshot** (each envelope precomputed into a lookup table) and publish it to the audio thread via a **single atomic-pointer swap**. The audio thread loads that pointer once per block and reads only the snapshot/table — never the live `ValueTree`. Old snapshots are freed on the message thread once no longer referenced. This also settles a performance question: Bezier curves are evaluated **once at edit time into a table**, not per-sample at audio rate.

**Editor lifecycle.** The processor outlives the editor — the window is opened and closed repeatedly while audio keeps running. The editor attaches its listeners, timers, and the analyzer's FIFO consumer on construction and detaches them all on destruction; no engine state ever lives in the editor. (A classic source of leaks and use-after-free crashes if ignored.)

Key JUCE classes we'll lean on:
- `juce::AudioProcessor` — the engine host; `processBlock()` is where audio happens.
- `juce::AudioProcessorEditor` — the GUI root.
- `juce::Component` — every panel/control.
- `juce::AudioProcessorValueTreeState` — parameters + state.
- `juce::ValueTree` + `juce::UndoManager` — state model with undo/redo.
- `juce::LookAndFeel` — skinning.
- `juce::dsp` module — oscillators, filters, gains, FFT, oversampling, convolution.

> **Cardinal rule — real-time audio safety.** On the audio thread (`processBlock` and anything it calls): no memory allocation, no locks/mutexes, no file or network I/O, no exceptions, and no unbounded or blocking operations. Everything the audio thread needs is pre-allocated and handed over lock-free. Every engine decision in this document serves this rule; when in doubt, it wins.
>
> **Rule of thumb for the whole project:** build the engine for a feature first and verify it headless (no GUI), then add the minimal GUI to drive it, then refine. The GUI is never where logic lives.

---

## 3. Full feature inventory (scope map)

Grouped by subsystem. The **Tier** column is our build priority: **C** = core/early, **M** = mid, **L** = late, **F** = final.

### 3.0 Voicing & signal path — Tier C
- **Monophonic.** One kick sounds at a time. A new trigger **chokes** the currently ringing tail — the previous voice is cut (short anti-click fade or brief crossfade) and the new one starts immediately.
- **Mono core, stereo after FX.** The synth engine, layers, mixer, and dry kick are **mono**. Stereo is introduced only downstream by FX that produce it (reverb, delay, ring mod). The output bus is stereo so those effects have somewhere to live.
- **DC blocker** on the output (high-pass roughly 5–20 Hz, *cutoff revisitable*).
- `ScopedNoDenormals` in `processBlock` to avoid denormal CPU spikes in long tails/reverb decays.

**Voice state machine (single global voice):**
- **Idle** → on note-on → **Body**
- **Body** — pitch + amp envelopes play through. → if note still held *and* sustain mode on → **Sustain-loop**; → if envelopes finish → **Idle**; → on note-off → **Release**
- **Sustain-loop** — loop between the two keytracked nodes while held. → on note-off → **Release**
- **Release** — finish the tail. → when tail completes → **Idle**
- **Choke** (transient, from *any* state on a new note-on) — fast fade-out of the current voice, then immediately re-enter **Body**.

Define and verify this state machine on paper before Phase 1; it's cheap now and tangled to discover in code (it touches gate, sustain mode, and choke).

### 3.1 Core synthesis — Tier C
- Node-based **pitch** envelope and **amp** envelope per Sub layer.
- High-quality **Bezier curves** between nodes.
- **Sub oscillator** (sine fundamental to start; shape options later).
- **Sub harmonics**: harmonic content above the fundamental — **Gain**, **Ratio**, **Decay**, plus presets.

### 3.2 Layers — Tier C/M
- **5 layers**, each independently a **Sub** or a **Click** (or off). Not a fixed arrangement.
- Per-layer **type switch**, on/off.

### 3.3 Mixer: level / mute / solo — Tier C
- Per-layer **level** control and (later) a small output **meter**.
- **Mute** and **Solo** flags per layer, independent. **Multi-solo**: any number soloed at once.
- Audibility rule:

| Layer flags | No layer soloed | ≥1 layer soloed |
|---|---|---|
| neither | audible | silent |
| solo on | audible | audible |
| mute on | silent | silent |
| solo + mute | **silent (mute wins)** | **silent (mute wins)** |

- GUI mirrors engine: soloed layers light up; layers silenced *because* of someone else's solo are dimmed.

### 3.4 Editing canvas / visualization — Tier C/M
- Central canvas: pitch + amp curves over the waveform.
- **Beat grid** behind the waveform, synced to host **BPM**.
- **Log / Linear** vertical-axis toggle.
- **Zoom & pan**.
- **Frequency readout** while dragging nodes.
- **Length** control = work-area length; sync across layers or independent per layer.
- **Undo / redo**, 30 steps.
- **Node interaction:** click-drag to move; double-click empty space to add a node, double-click (or right-click) a node to delete; drag the curve between nodes to reshape its Bezier; hold a modifier for fine-drag; right-click for a context menu (snap, reset, copy curve).
- **Undo granularity:** one gesture = one undo step. A drag is wrapped in a single transaction (begin on mouse-down, commit on mouse-up) so the 30-step history walks back whole edits, not pixels.
- **Repaint strategy:** continuously-updating views (canvas, spectrum, meters) repaint on a shared timer at ~30–60 fps using small repaint regions, not full redraws; consider the JUCE OpenGL renderer for the canvas only if profiling shows it's needed.

### 3.5 Tuning / pitch tools — Tier M
- **Selective keytracking**: per-node choice of whether it follows keytracking, so the low-end foundation stays put while other nodes track pitch.
- **Sustain mode**: loop between keytracked nodes while held, then finish the tail on release.
- **Pitch snap**: snap nodes to nearest harmonic note.
- **Master tuning** in cents (default reference A4 = 440 Hz).

### 3.6 Phase tools — Tier M
- **Rotate** the Sub layer's phase.
- **Phase lock / phase align**: lock phase from a chosen node onward; edits before it don't disturb downstream phase alignment.

### 3.7 FX suite — Tier L
- Modules: Clip, Tube, Wave, Tape distortion; Bit Crusher; Ring Modulator; Delay; Reverb; Filter; Drive; **dual Compressors**; EQ.
- **2 insert channels + a master channel**; 5 layers routable through them; swappable module slots.
- **4 FX Macros** assignable to any internal FX parameter.
- (Deep dive §7.1.)

### 3.8 EQ + spectral analyzer — Tier L
- **4-band EQ** with a **real-time spectral analyzer**. (Deep dive §7.2.)

### 3.9 Output stage — Tier L
- **Master limiter**: adjustable release + lookahead; pre/post gain selection.

### 3.10 Triggering / input — Tier M/L
- Standard **MIDI** note triggering; **Gate** (note length sets kick length); **Velocity**, **Portamento**, **Keytrack**, **Pitch Wheel**.
- **MIDI housekeeping** (default): full note range triggers; respond to all-notes-off and silence cleanly on host transport stop.
- **TRIGGER**: fire DOOF from a sidechain audio input's transients — no MIDI needed. (Deep dive §7.3.)

### 3.11 Presets, library & export — Tier C→L (built incrementally)
- **Minimal save/load (early, ~Phase 3–4):** serialize the current state to a `.doof` file and reload it, so sounds can be banked *during the build*.
- **Full preset bank/browser (Phase 11):** factory + user presets, sub-layer + click sub-presets, categories/favourites, init patch. (Deep dive §7.5.)
- **Export to WAV (Phase 11, render core reusable earlier):** render the kick at a chosen velocity/key to a `.wav` file; drag-out to DAW or onto a DOOF layer. (Deep dive §7.4.)

### 3.12 Import & Analyze — Tier F (final, optional)
- Drag in a kick sample → isolate the click transient, resynthesize the sub as editable pitch+amp envelopes.
- Drag in a full song → isolate/extract the kick from the mix.
- Research-grade (pitch tracking, transient/source separation). Project is complete without it.

---

## 4. UI layout (the blueprint)

Single window. Eight regions:

1. **Transport / preset bar** — name (DOOF), preset browser, host BPM, undo/redo, menu/settings.
2. **Page tabs** — Edit · FX · EQ · Limiter · Settings (paged model first).
3. **Layer selector** — horizontal strip of 5 layers; index, type (Sub/Click/off), mute/solo, level.
4. **Main edit canvas** — beat grid, pitch + amp curves, draggable Bezier nodes, frequency readout.
5. **Canvas toolbar** — log/lin, pitch snap, phase lock, zoom, pan, length.
6. **Contextual parameter panel** — swaps with the selected layer's type.
7. **FX macros** — 4 assignable macro knobs (full rack on the FX page).
8. **Master** — output meter, limiter on/off, pre/post gain.

**Layout strategy:** build **paged** first (simpler component tree, easier debugging); keep panels position-agnostic; add the single-dense-screen arrangement later as a second layout; only then decide whether to expose a user toggle (stored in plugin state). Resizable window; single-screen needs a larger minimum size.

---

## 5. Build workflow

**Environment & tooling (Phase 0, trimmed — see §6):** JUCE via **CMake**; git from commit zero; basic **pluginval**. Defer CI, the golden-render harness, AU, and multi-host testing until there's a stable engine (~Phase 3).

**Shared render core:** the offline "render one note to a buffer through the full signal path" routine powers **both** WAV export (§7.4) and the golden-render regression suite (§8). Build it once.

**The per-feature loop:**
1. Model the parameter(s) in APVTS / `ValueTree`.
2. Implement the engine/DSP and verify it **headless** (unit test or render-to-buffer + inspect).
3. Add the minimal GUI control to drive it (flat dev skin).
4. Manual test in standalone, then a DAW.
5. Commit. Update this doc's decision log if anything changed.

**Skinning order:** everything built in the flat developer skin; the hardware skin is a late, separate pass touching only `LookAndFeel` + assets (Phase 12).

---

## 6. Phase plan with verification tests

Each phase lists what to **Build** and a concrete **Verify** test you can actually conduct, with a clear pass condition. **Do not advance until the Verify test passes.** Tests are a mix of automated (unit test / render-and-measure) and manual (do X in a DAW, observe Y); where both apply, both are listed.

### Phase 0 — Project scaffolding (trimmed)
- **Build:** CMake project; **VST3 + Standalone** (defer AU); **unique plugin + manufacturer IDs**; bus layout stub (**mono in / stereo out**, sidechain input declared for later); empty plugin that loads; git repo; basic `pluginval`; a unit-test target.
- **Verify:** Build all targets. Launch the Standalone app and load the VST3 in one DAW — both instantiate with no error. Run `pluginval` at basic strictness — it passes. Run the unit-test target — it executes and reports green. **Pass = all four happen cleanly.**

### Phase 1 — Engine skeleton + one audible sub
- **Build:** APVTS with a few params (stable IDs); a single **mono** Sub layer = sine oscillator driven by **one hardcoded** pitch+amp envelope; MIDI note triggers it; **monophonic choke-on-retrigger** + short anti-click fade; DC blocker + `ScopedNoDenormals`; basic gain.
- **Verify:** In Standalone, play a MIDI note — you hear a kick. Automated: render the note to a buffer and assert (a) RMS rises then decays toward zero, (b) the fundamental falls over time (FFT or zero-crossing rate), (c) no NaNs/denormals, (d) no sample spikes. Fire a second note ~30 ms into the first and assert no sample-to-sample jump above a small threshold at the choke. Run an audio-thread allocation check — **zero allocations in `processBlock`.** **Pass = audible kick + all assertions green.**

### Phase 2 — Node-based envelope engine
- **Build:** Envelope data model (`ValueTree`, serializable + undoable); evaluator that **precomputes each envelope into a lookup table** on edit; **immutable snapshot** handed to the audio thread via a single atomic-pointer swap (audio thread never reads the live `ValueTree`). Pitch + amp both use it.
- **Verify:** Unit tests — feed known node sets and assert the evaluator returns expected values at sampled times and the curve is continuous; add/move/delete nodes and assert output changes accordingly; serialize → deserialize and assert identical state. Concurrency test — one thread hammers edits while another reads snapshots; assert no torn reads and (via an assertion/flag) the audio path never touches the live tree. **Pass = all unit + concurrency tests green.**

### Phase 3 — The envelope canvas (GUI)
- **Build:** Canvas drawing pitch+amp curves + nodes; full node interaction (§3.4); beat grid synced to BPM; log/lin; zoom/pan; frequency readout; length. **Minimal preset save/load** (serialize state to a `.doof` file, reload) lands here.
- **Verify:** Drag a pitch node down — the kick audibly drops and the curve follows the cursor. Toggle log/lin, render a note before and after — **null test confirms bit-identical audio** (axis-only change). Set host to 128 BPM — grid lines land on beats; repeat at 174. Zoom/pan, then count nodes — none lost or moved. Drag a node in one continuous motion, press undo once — the **whole** drag reverts. Save to `.doof`, alter the patch, reload — the reloaded render **null-matches** the original. **Pass = each check behaves as stated.**

### Phase 4 — Layers + mixer
- **Build:** 5 layers; per-layer type (Sub/Click/off), level, mute, solo; multi-solo per §3.3; horizontal layer selector; selecting a layer swaps the contextual panel.
- **Verify:** Unit-test the audibility function against **all 16** combinations of the §3.3 truth table. In the GUI, solo layers 1 and 3 — only those are audible, the rest visibly dim; add a mute to a soloed layer — it goes silent (mute wins). Set all layers to unity and render — assert no unexpected clipping. Click between layers while a note rings — no audio dropout. **Pass = truth-table tests green + GUI matches.**

### Phase 5 — Click layers
- **Build:** Click playback — built-in clicks, noise, basic texture; per-click level/tone; layered with subs.
- **Verify:** Stack a click on a sub — it sounds like one cohesive kick. Render and assert the click onset is **sample-aligned** to note-on. Fast-trigger repeatedly — no clicks at sample boundaries. FFT the render and assert transient energy is in the high band, sub energy in the low band. **Pass = cohesive kick + spectral split confirmed.**

### Phase 6 — Sub tuning + phase tools
- **Build:** Sub harmonics (gain/ratio/decay + presets); selective keytracking; sustain mode; pitch snap; master tuning; phase rotate + lock/align. (DOOF is now a complete-sounding multi-layer instrument — a natural playable checkpoint, though the target is still Phase 12.)
- **Verify:** Flag two nodes as keytracked, play C1 then C2 — only the flagged nodes shift pitch; the unflagged foundation stays put (measure their frequencies). Hold a note with sustain mode on — the sound loops between the keytracked nodes, then completes the tail on release. Engage pitch snap and drag — nodes land exactly on harmonic note frequencies. Phase-lock at a node, edit an earlier node, measure correlation against a reference bass tone — downstream phase alignment unchanged. **Pass = each behaviour measured as stated.**

### Phase 7 — FX suite + routing matrix
- **Build:** FX module set; 2 inserts + master (stereo domain); per-layer routing; swappable slots; 4 macros. (§7.1.)
- **Verify:** For each module — bypass = **bit-identical null test**; engaged = measured response matches spec (e.g. filter −3 dB point at the set cutoff; bitcrusher quantization steps visible in the waveform). Route only layer 2 to Insert A, solo that route — only layer 2's processed signal is present. Assign a macro to a cutoff and sweep — the cutoff tracks. Compare oversampled vs non-oversampled distortion via FFT — less aliasing with oversampling. Automate a param fast — no zipper noise. **Pass = every module + routing + macro check passes.**

### Phase 8 — EQ + spectral analyzer
- **Build:** 4-band EQ; real-time FFT analyzer overlay. (§7.2.)
- **Verify:** Set an EQ band and sweep a sine through it — measured gain at the band frequency matches the control within tolerance. Feed a 1 kHz sine — the analyzer peak sits at 1 kHz at the correct level. Run a stall/thread check with the analyzer active — **no audio-thread blocking**; CPU within budget. **Pass = measured EQ response + correct analyzer + no stalls.**

### Phase 9 — Master limiter + output
- **Build:** Limiter with adjustable release + lookahead; pre/post gain; output metering.
- **Verify:** Send a hot signal at several settings and render — assert the **peak never exceeds the ceiling**. Confirm the plugin reports its lookahead latency (check delay compensation in the DAW). A/B light settings (transparent) vs aggressive (no pumping at sane release). **Pass = ceiling never breached + latency reported.**

### Phase 10 — Triggering
- **Build:** MIDI velocity/portamento/pitch-wheel/keytrack/gate; MIDI housekeeping (all-notes-off, transport-stop silence); TRIGGER sidechain. (§7.3.)
- **Verify:** Hold notes of different lengths in gate mode — kick length follows. Map velocity, play soft vs hard — response changes as designed. Route a drum loop to the sidechain — DOOF fires on each transient with low latency; count triggers vs hits across several loops for false-positive/missed rates; assert no double-trigger on one hit. Load in a host with no sidechain — plugin loads fine, TRIGGER simply unavailable. **Pass = gate/velocity/trigger all behave + graceful no-sidechain.**

### Phase 11 — Preset bank, export/drag-out
- **Build:** Full preset browser (factory + user, categories/favourites, init patch); WAV export at chosen velocity/key; drag-out to DAW and onto a DOOF layer. (§7.4, §7.5.)
- **Verify:** Save a patch as a preset, load a different one, reload the first — **identical render** (round-trip). Export to WAV, re-import that WAV against DOOF's live output on the same note — they **null** against each other. Open the export in another app — valid `.wav`. Drag the export onto a DOOF layer — it loads. Undo across a preset load + a structural change — state stays consistent. **Pass = round-trip + export null-match + valid file.**

### Phase 12 — Skinning + layout polish ★ TARGET (Phase 12 or bust)
- **Build:** Hardware-machine `LookAndFeel` + assets (filmstrip knobs, LED readouts, panel art); flat skin retained; single-dense-screen layout; optional paged/single toggle persisted in state; finalize resizing. **This is the committed finish line — feature-complete and fully skinned.**
- **Verify:** Switch flat → hardware skin, render a note before and after — **null test confirms audio/behaviour identical** (visuals only). Resize to min and max — every control stays legible/usable in both skins. Toggle paged↔single — no control lost, state persists. Profile with the heavy skin loaded — no meaningful CPU/redraw regression. **Pass = skin swap is visual-only + usable at all sizes + state preserved.**

### Phase 13 — Import & Analyze (optional, beyond the target)
- **Build:** Sample import → click isolation + sub resynthesis to editable envelopes; full-song kick extraction.
- **Verify:** Define the pass tolerance **before** starting. Import a set of reference kicks — assert the resynthesized sub's pitch/amp envelope tracks each source within tolerance, and the isolated click keeps its transient (compare onsets/spectra). Render the resynth vs source — perceptually close within your error metric. Drop in a full track — the extracted kick is recognizable. **Pass = resynth within the pre-defined tolerance across the test set.**

### Phase 14 — Hardening & release prep (for distribution)
- **Build:** Add AU; cross-format/host validation; CPU/latency optimization; edge cases (extreme params, automation storms, sample-rate/buffer changes); installer/signing/notarization as needed.
- **Verify:** Run `pluginval` at **strict** level on VST3, AU, and Standalone on both OSes — all pass. Change sample rate and buffer size mid-session in the DAW — no crash, no artifacts. Run a long stress session with heavy automation under a leak detector — no leaks or crashes. Save a session in one host, reopen in another — state restores. CPU within target on the reference machine. **Pass = strict pluginval green everywhere + stable under stress.**

---

## 7. Deep-dive design notes

Design intent + approach. **Exact JUCE API signatures to be confirmed at build time**, not assumed here.

### 7.1 Full FX routing matrix
Route any of 5 layers through 2 insert FX channels, then a master FX channel; swappable slots; 4 macros.
```
Layer 1 ┐
Layer 2 ┤  per-layer assign     Insert A (N module slots) ┐
Layer 3 ┼──────────────────────► Insert B (N module slots) ┼─► Master FX ─► Limiter ─► Out
Layer 4 ┤  (A, B, both, or dry)                            ┘
Layer 5 ┘
```
- Each layer has a routing assignment (APVTS param). Each insert is an ordered list of module slots; a slot holds a module type + params. A common `FXModule` interface (`prepare`/`process`/`reset`/param list) makes slots interchangeable. Nonlinear modules get oversampling (`juce::dsp::Oversampling`).
- **Channel domain:** the FX section (inserts + master) is **stereo end-to-end**; the mono kick is copied to L/R at the FX entry, so every `FXModule` processes stereo uniformly.
- **Swappable-module host automation:** VST3/AU expect a *fixed* parameter list. Solution (decided): a fixed generic set of **slot parameters** per slot that each module maps onto.
- **Macros:** 4 macro params, each with a list of (target param, range, polarity) assignments.
- Smooth all user-facing params; report any added latency for host compensation.
- **Open:** slots per insert; fixed vs free reordering; dual-comp as one module or two slots.

### 7.2 EQ with spectral analyzer
- **EQ:** 4 bands of biquad/IIR (`juce::dsp::IIR`) — typical low shelf, two bells, high shelf (confirm). Per-band freq/gain/Q/on-off; smooth coefficient changes.
- **Analyzer:** windowed FFT (`juce::dsp::FFT`), Hann, size ~2048 with overlap (*finalize in design*). **The FFT runs off the audio thread** — the audio thread only copies samples into a lock-free FIFO; a timer/GUI path computes magnitudes and paints. Log frequency axis, smoothed magnitude, correct dB scaling, EQ curve overlaid on the live spectrum.
- **Test:** scaling verified against known tones; zero audio-thread stalls; EQ response matches controls.

### 7.3 TRIGGER — sidechain setup
- **Plumbing:** declare a sidechain input bus (main out + sidechain in); read the sidechain buffer in `processBlock`. Validate `isBusesLayoutSupported` so non-sidechain hosts still load (TRIGGER simply unavailable). *Confirm the exact bus pattern + per-DAW routing — historically fiddly.*
- **Detection:** envelope follower (fast attack, slower release) + threshold; rising edge = trigger; **retrigger lockout** prevents double-fires; optional kick-band-limited detection. Optional velocity from transient level.
- **Modes:** replace (detection-only source) vs augment (layered).
- **Test:** several reference loops/tempos; measure latency + false-positive/missed-hit rates; verify graceful no-sidechain behaviour.

### 7.4 Export to WAV
**Goal:** export the current kick to a `.wav` file.
- **Render:** offline-render one note at the chosen velocity/key through the **exact audible signal path** (layers → FX → limiter) into a stereo buffer, capturing the **full tail** (including FX/reverb tails, not just the synth tail). This render routine is the shared core also used by golden-render tests.
- **Write:** `juce::WavAudioFormat` → `AudioFormatWriter` (via `createWriterFor` on a `FileOutputStream`) → `writeFromAudioSampleBuffer`. Default 24-bit at the session sample rate; expose bit-depth/sample-rate options later. *Confirm exact writer call at build time.*
- **Deliver:** an "Export WAV…" button with a `FileChooser` to pick the destination (simplest), and/or drag-out via `DragAndDropContainer::performExternalDragDropOfFiles` (*confirm call*). Show progress/feedback during render.
- **Test:** exported audio null-matches the internal render (sample-accurate where possible); file is valid and DAW-accepted on both OSes.

### 7.5 Preset bank & library
**Goal:** save sounds for reuse and later tuning; browse factory + user banks.
- **A preset = the plugin's saved state.** Reuse the same serialization as `getStateInformation`: copy the APVTS/`ValueTree` state, write it (XML or binary) to a file. Loading reads the file and replaces the state — every parameter and all envelope/node data restored. Add a `stateVersion` field with a load-time migration step so older presets keep working as the format grows.
- **Layout on disk:** **factory presets** embedded in the binary (`BinaryData`); **user presets** saved under the OS user data directory (`File::getSpecialLocation(userApplicationDataDirectory)`) in a `DOOF/Presets` folder. A "bank" is simply a subfolder.
- **Granularity:** full-kit presets, plus **sub-layer** and **click** sub-presets (save/load a single layer) so building blocks are reusable.
- **Browser (Phase 11):** list/browse, save-as (with name + category), overwrite, delete, favourite, and a defined **init patch**. Categories/tags are metadata stored in the preset.
- **Early form (Phase 3–4):** a bare save-to-file / load-from-file via `FileChooser` — enough to bank sounds during development; the browser is layered on later.
- **Test:** save → load restores the sound exactly (null-match a render before/after); migration loads an older `stateVersion`; factory presets load from `BinaryData`; user presets persist across sessions.

---

## 8. Decision log & open questions

**Decided:** see §0 + §0.5. Engine-first per-feature workflow; analyzer/FFT off the audio thread; mute-wins on solo conflict; oversampling for nonlinear FX; monophonic choke voicing + voice state machine; mono core / stereo after FX; FX domain stereo end-to-end; envelopes precomputed to lookup tables + immutable atomic-pointer snapshot; stable namespaced parameter IDs (never reused/removed); preset = saved state blob with `stateVersion` migration; fixed generic FX slot params; modulation closed to envelopes + 4 macros; DC blocker; `ScopedNoDenormals`; golden-render suite (shares the export render core); unique plugin/manufacturer IDs; factory content via `BinaryData`, user samples/presets on disk; real-time-safety cardinal rule; editor attaches/detaches listeners+timers on open/close; ~30–60 fps shared-timer partial repaints; one-gesture-one-undo; log skew for freq/time params; **Phase 12 (feature-complete + hardware skin) is the committed target**; every phase gated by a conductable Verify test; trimmed Phase 0 (VST3 + Standalone first, AU/CI/golden-harness/multi-host deferred).

**Open / to revisit:**
- Preset browser depth beyond the basics — how rich the categories/tag/search system gets (Phase 11, low coupling).
- FX: slots per insert; fixed vs free reordering; dual-comp as one module or two.
- Analyzer FFT size/overlap/window; EQ band types and ranges.
- Sidechain: pass-through vs detection-only; kick-band-limited detection.
- Export: user-selectable bit depth/sample rate; default tail-length cap.
- Drag-onto-layer behaviour before Phase 13 exists (likely load-as-sample).
- DC-blocker cutoff value; keytrack reference note.
- Total reported latency / PDC consolidation as oversampling + lookahead are added.
- Test-host list (which DAWs, which "strict" host) — finalize at first multi-host pass (~Phase 3) and confirm for AU in Phase 14.
- CPU target + reference machine for performance gates.
- macOS signing/notarization details (only at distribution, Phase 14).

---

## 9. Originality & licensing

- **Design originality:** recreating the *techniques* is fair — none of the DSP is proprietary. DOOF ships with its **own name, UI artwork, and branding**. The hardware skin is *inspired by* the industrial drum-machine aesthetic (knobs, metal, LEDs), not a 1:1 copy of any specific product's panel art or trade dress.
- **Framework & content licensing:** (1) JUCE itself is licensed in tiers, and the terms/revenue thresholds have changed over time — verify the *current* JUCE license before any distribution. (2) Any factory clicks/samples/presets must come from license-clear sources. Neither affects a private/learning build; both matter the moment DOOF is distributed.

---

## 10. JUCE class quick-reference

| Need | Likely class / tool | Confidence |
|---|---|---|
| Plugin engine | `juce::AudioProcessor` | high |
| GUI root | `juce::AudioProcessorEditor` | high |
| Any panel/control | `juce::Component` | high |
| Parameters + state | `juce::AudioProcessorValueTreeState` | high |
| State model + undo | `juce::ValueTree` + `juce::UndoManager` | high |
| Skinning | `juce::LookAndFeel` (subclass, `setLookAndFeel`) | high |
| Oscillators/filters/gain | `juce::dsp` module | high |
| FFT (analyzer) | `juce::dsp::FFT` | high |
| Oversampling (FX) | `juce::dsp::Oversampling` | high |
| WAV write (export) | `juce::WavAudioFormat`, `AudioFormatWriter` | confirm signature |
| Audio file read | `juce::AudioFormatManager` | high |
| File dialogs | `juce::FileChooser` | high |
| User data location | `File::getSpecialLocation(userApplicationDataDirectory)` | high |
| Save/restore state | `AudioProcessor::getStateInformation` / `setStateInformation` | high |
| File drag-out | `DragAndDropContainer::performExternalDragDropOfFiles` | confirm signature |
| File drag-in | `juce::FileDragAndDropTarget` | high |
| Sidechain bus | `BusesProperties` / `isBusesLayoutSupported` | confirm pattern |
| Validation | pluginval | high |
| Unit tests | `juce::UnitTest` or Catch2/GoogleTest | high |

> Items marked "confirm" are correct in concept; verify the exact current API when we reach that phase rather than trusting memory.