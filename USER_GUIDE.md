# DOOF — User Guide

DOOF is a kick-drum synthesizer plugin (VST3 + Standalone). This guide reflects the plugin as built through **Phase 6** (sub tuning and phase tools).

DOOF is under construction, so parts of the interface describe features that exist and parts are still stubs. Where something isn't built yet, this guide says so rather than leaving you to find out.

## Loading DOOF

- **VST3**: the build produces `DOOF.vst3`. Install it in your system's VST3 folder (e.g. `~/Library/Audio/Plug-Ins/VST3/` on macOS), or in a custom folder your DAW is set to scan, and rescan plugins. DOOF is listed under the vendor **4Eternity**. Use a Release build for anything beyond development — Debug works but costs far more CPU.
- **Standalone**: run the built `DOOF.app` directly. Standalone has no host play head, so the beat grid never appears there — that needs a real DAW host.

DOOF is played with MIDI notes. A note-on triggers the kick, landing on the exact sample the note arrives at rather than the start of the host's buffer. Retriggering while a note is still ringing chokes the old one and starts the new one cleanly. On a Click layer the new hit starts immediately and the old one fades out underneath it over 5 ms, so fast rolls stay tight.

---

## The layer strip

Across the top are **five layers**. Each is an independent sound source with its own envelopes and its own place in the mix.

Each layer cell has:

| Control | What it does |
|---|---|
| **Number** | Which layer this is, 1–5 |
| **Type** | **Off**, **Sub**, or **Click** |
| **M** | Mute — lights **red** when on |
| **S** | Solo — lights **amber** when on |
| **Level** | That layer's contribution to the mix |

**Click anywhere on a layer** — its background, or any of its controls — to select it. The selected layer has a white outline, and the editing area below shows *that* layer.

Out of the box, layer 1 is a Sub and layers 2–5 are Off.

### Types

- **Sub** — the sine-based body of the kick, edited with the envelope canvas below.
- **Click** — the transient on top of the kick: a short tick, burst or sample, edited with the [click panel](#the-click-panel).
- **Off** — the layer is not in the mix at all.

### Mute, solo, and what you see

Solo is **multi-solo**: any number of layers can be soloed at once, and all of them stay audible.

| Layer's flags | Nothing soloed | Something soloed |
|---|---|---|
| neither | audible | **silent** |
| solo | audible | audible |
| mute | silent | silent |
| solo **and** mute | **silent** | **silent** |

The last row is the rule worth remembering: **mute always wins over solo.** A layer with both set is silent, but its solo still counts — so it goes on silencing everything that isn't soloed.

The strip shows you this directly:

- **Soloed layers light up** with an amber background.
- **Layers silenced by someone else's solo are dimmed.**

Dimming means specifically "silent, and nothing on this layer says why." A muted layer isn't dimmed — its red **M** is the visible reason. An **Off** layer isn't dimmed either, for the same reason.

One deliberate quirk: **soloing an Off layer does nothing.** An Off layer makes no sound, so letting its solo silence everything else would give you a silent patch with no visible cause.

Layers are summed straight, with no division by how many are active — so turning a layer on never changes how the others sound. Five layers at full level will therefore exceed full scale. That's expected; the master limiter arrives in Phase 9.

---

## The click panel

Selecting a **Click** layer shows the click panel in place of the envelope canvas.

| Control | What it does |
|---|---|
| **Type** | Which click this layer plays — see below |
| **Tone** | Brightness, 0–100%. Darker settings round the click off; brighter ones sharpen it. Tone changes brightness, not volume. |
| **Decay** | How long a built-in click lasts, 1–50 ms (8 ms out of the box). It's a real length: the click is completely silent from that point on. |
| **Level** | The layer's level — the same control as the slider on the layer strip, just bigger |

Knob values can be typed in: Decay takes milliseconds (`12` or `12 ms`), Tone and Level take percentages.

### Click types

- **Tick** — a filtered impulse, the sharpest of the four: a beater on a hard head.
- **Noise** — a burst of white noise: broadband air.
- **Snap** — band-limited noise, narrower and more focused than Noise.
- **Thump** — a short sine blip, well above the sub: body rather than air. Tone sets its pitch.
- **Sample 1–4** — four slots for recorded clicks.

Every hit of a built-in click is identical, the way a sample would be — no random variation from one note to the next.

### Sample slots

A sample slot plays its file once, to its own end. **Decay doesn't apply to samples** — the sample's own shape is its envelope — so the Decay knob greys out while a slot is selected. Tone still works, as a plain high-cut.

The type list shows what's behind each slot: `Sample 1 - kick-top.wav`, or `Sample 2 (empty)`. **An empty slot is silent.** It doesn't fall back to a built-in click, and the panel says so in its status line. That's deliberate: a preset saved against a slot sounds the same, or stays silent, wherever it's opened, rather than quietly turning into a different sound.

Samples are built into the plugin, not loaded at runtime. To fill the slots, place `.wav` files in `resources/clicks/` and rebuild — `resources/clicks/README.md` covers slot order and the licensing rules. Out of the box the slots are empty.

Samples recorded at any sample rate play at their correct pitch and length whatever rate your session runs at, and stereo files are folded to mono.

### Building a kick

Put a **Sub** on one layer and a **Click** on another and play them together — the sub carries the low end, the click carries the attack. Use each layer's **Level** to balance them, and **Tone** to set how much top the click adds. The factory sub already has a slight attack of its own, so a little click goes a long way.

---

## The envelope canvas

Selecting a **Sub** layer shows the canvas, which overlays two curves for that layer on one shared time axis:

- **Cyan** — the pitch envelope (Hz)
- **Orange** — the amp envelope (0–1)

Each curve is normalised to the canvas's full height independently, so despite their different units they share the same vertical space.

The canvas always edits **the selected layer**. Switching layers swaps the curves under you; your zoom and pan stay where they are, so you can compare the same time window across two layers.

### Editing nodes

- **Move a node** — click and drag its dot. A readout near the cursor shows its live value and time.
- **Add a node** — set the **Editing** dropdown to **Pitch** or **Amp**, then double-click empty canvas space. The node is added to the curve named there.
- **Delete a node** — double-click it, or right-click it and choose **Delete Node**.
- **Reshape a curve** — click and drag the curve line itself, *between* two nodes. The curve bends to follow your cursor at the point you grabbed. Hold **Shift** for finer control.

### Right-click menu

Right-clicking the canvas opens a menu:

- **Delete Node** — only when you right-clicked on a node.
- **Snap to Nearest Note** — pitch nodes only. Moves that one node onto the nearest note, whether or not Snap to Note is on.
- **Keytrack** — pitch nodes only. Ticked when the node follows the keyboard; choose it again to turn it off. See [Keytracking](#keytracking).
- **Lock Phase Here** — pitch nodes only. Ticked on the locked node; choose it there to remove the lock. See [Phase lock](#phase-lock).
- **Reset Curve** — returns that curve to its out-of-the-box shape.
- **Copy Curve** / **Paste Curve** — copies a curve's shape via the system clipboard. This works **across curves and across layers**, so you can paste a pitch shape onto an amp curve, or reuse one layer's envelope on another.

### Zoom and pan

- **Mouse wheel** zooms the time axis, centred on your cursor.
- **Click and drag empty space** pans.

Both are view-only. Neither ever changes the envelope data.

### Length

Each curve has its own **work-area length** — how much time you're editing, distinct from the full 4-second range the engine can hold.

Set it either way:

- Drag the small **triangle handle** at the bottom of the canvas (one per curve).
- Type into the **Pitch Len (s)** / **Amp Len (s)** fields.

The two stay in sync. Adding and moving nodes is bounded to the work area, so Length is a way to zoom your *editing* into the part of the sound you care about.

Length is per curve **and** per layer.

### Log / Linear pitch scale

**Log Scale (Pitch)** switches the pitch curve between linear and logarithmic vertical mapping. It's display-only — it never touches the envelope data or the audio.

It applies to the pitch curve alone. The amp curve is always linear, since its range includes exactly 0, where a log scale is undefined.

**This setting is remembered per layer**, along with the Editing dropdown — so each layer keeps the view you last used on it.

### Snap to Note

**Snap to Note** makes pitch nodes land exactly on a musical note while you drag them or add them. The readout shows the note, as in `C1  65.41 Hz`. With snap off, the readout shows how far you are from the nearest note instead, as in `67.0 Hz  C1 +42c`.

Snapping uses equal temperament and follows **Tune**: a node snapped to C1 plays C1 at whatever reference Tune sets. Amp nodes are never snapped. Like Log Scale, this setting is remembered per layer.

### Beat grid

Faint vertical gridlines mark beat boundaries, synced to your host's tempo. VST3 in a DAW only; the Standalone app has no tempo source on macOS.

### Markers

- **A white ring** marks a keytracked node.
- **A dashed magenta line** labelled PHASE LOCK marks the locked node.
- **A shaded band** labelled SUSTAIN LOOP shows the stretch that loops while a note is held. It only appears when the layer's **Sustain** is on and two or more nodes are keytracked.

---

## The Sub panel

Under the canvas, a **Sub** layer has its own row of controls.

### Harmonics

A Sub layer is a pure sine by default. Harmonics add a series of overtones on top of it, which you hear as grit or presence. They fade away and leave the clean sine underneath.

| Control | What it does |
|---|---|
| **Harmonics** | Which overtones: **All** (2nd to 8th), **Odd** (3rd, 5th, 7th — hollow, square-ish), **Even** (2nd, 4th, 6th, 8th — fuller), **Octaves** (2nd, 4th, 8th — the cleanest reinforcement) |
| **Gain** | How loud the first overtone is, compared with the fundamental. **0% turns harmonics off.** |
| **Ratio** | How quickly each higher overtone gets quieter. At 50%, each one is half the level of the one before. |
| **Decay** | How long the harmonics take to die away. After that, only the clean sine remains. |

The harmonics follow the pitch sweep exactly. DOOF also leaves out any overtone that would land too close to the top of the audio range, so high notes don't produce harsh aliasing.

### Sustain

With **Sustain** on, holding a note loops the sound between your **first and last keytracked nodes**. When you let go, it plays on from where it was and finishes its tail. The jump back to the start of the loop is crossfaded, so it doesn't click, even if the loop's two ends are at different levels.

Sustain needs at least **two keytracked nodes**. With fewer, there's nothing to loop between, and it does nothing. Without Sustain, how long you hold a note makes no difference: the kick plays its whole envelope either way.

### Phase

**Phase** rotates the sine, from 0 to 360 degrees. Normally it sets where the waveform starts. If a node is [phase-locked](#phase-lock), Phase sets the waveform's phase at that node instead, and the note starts from zero.

---

## Tuning and keytracking

### Tune and Key Root

These two controls sit in the toolbar and apply to the whole patch:

- **Tune** shifts every Sub layer by up to a semitone either way, in cents. It moves the A4 = 440 Hz reference, so Snap to Note follows it.
- **Key Root** is the note at which keytracked nodes play exactly as drawn. It defaults to **C3**, which is the note Ableton's Drum Rack sends a pad's instrument unless you change it, so a DOOF kick in a Drum Rack plays as drawn. You can type a note name (`A2`, `F#1`, `Bb-1`) or a MIDI number.

Note names follow Ableton's convention: middle C is C3.

### Keytracking

Right-click a pitch node and choose **Keytrack** to make it follow the keyboard. Each semitone you play above or below Key Root moves a keytracked node by a semitone.

Nodes you don't flag stay exactly where you drew them. That's the point: keytrack the attack, and leave the low-end foundation where it sits in your mix. Between a keytracked node and an unflagged one, the curve blends smoothly from one to the other.

Tune, Key Root and the settings in the Sub panel take effect from the next note. They never change a note that's already sounding.

### Phase lock

Right-click a pitch node and choose **Lock Phase Here**. From that node on, the waveform's phase is fixed: editing any node *before* the lock no longer shifts the tail. Use it to keep a kick's tail in phase with a bass part while you keep reshaping the attack.

DOOF holds the lock by bending the pitch very slightly over the tenth of a second before the locked node. The bend rises and falls smoothly, so there's no click. There's only one lock per curve; locking another node moves it.

---

## Undo and redo

- **Cmd+Z** to undo, **Cmd+Shift+Z** to redo (Ctrl on Windows).
- The **Undo** / **Redo** buttons do the same thing.

Keytrack and phase-lock changes are undoable too. All layers and both curves share **one** undo history, so undo always reverts whatever you did most recently, wherever you did it. A whole drag — however many small mouse movements it was made of — undoes in a single step. The history holds 30 steps.

Loading a preset clears the history: a freshly loaded patch isn't an edit you can undo your way out of.

---

## Presets

**Save…** and **Load…** write and read a `.doof` file. It holds the complete patch: every layer's type, level, mute and solo, each layer's click type, tone and decay, each layer's harmonics, sustain and phase, both envelopes for all five layers (including which nodes are keytracked and phase-locked), each layer's Length and view settings, Tune, Key Root, and the master gain.

A preset stores which sample *slot* a Click layer uses, not the file. Renaming or reordering the files in `resources/clicks/` changes what those presets play.

This is the same state your DAW saves with its project, so a patch that sounds right in the plugin will sound right when you reopen the session.

Older `.doof` files still load. A preset saved before layers existed opens as the single-layer patch it described, with the other four layers returned to their factory state.

---

## Not built yet

So you know where the edges are:

- **FX, routing, EQ, analyzer, master limiter** (Phases 7–9).
- **Sidechain triggering** (Phase 10).
- **Preset browser and drag-out export** (Phase 11) — for now, presets are plain `.doof` files you manage yourself.
- **The finished skin** (Phase 12). The current flat dark look is a working surface, not the intended design.
