# DOOF — User Guide

DOOF is a kick-drum synthesizer plugin (VST3 + Standalone). This guide reflects the plugin as built through **Phase 5** (click layers).

DOOF is under construction, so parts of the interface describe features that exist and parts are still stubs. Where something isn't built yet, this guide says so rather than leaving you to find out.

## Loading DOOF

- **VST3**: the build produces `DOOF.vst3`. Install it in your system's VST3 folder (e.g. `~/Library/Audio/Plug-Ins/VST3/` on macOS) and rescan plugins in your DAW.
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

### Beat grid

Faint vertical gridlines mark beat boundaries, synced to your host's tempo. VST3 in a DAW only; the Standalone app has no tempo source on macOS.

---

## Undo and redo

- **Cmd+Z** to undo, **Cmd+Shift+Z** to redo (Ctrl on Windows).
- The **Undo** / **Redo** buttons do the same thing.

All layers and both curves share **one** undo history, so undo always reverts whatever you did most recently, wherever you did it. A whole drag — however many small mouse movements it was made of — undoes in a single step. The history holds 30 steps.

Loading a preset clears the history: a freshly loaded patch isn't an edit you can undo your way out of.

---

## Presets

**Save…** and **Load…** write and read a `.doof` file. It holds the complete patch: every layer's type, level, mute and solo, each layer's click type, tone and decay, both envelopes for all five layers, each layer's Length and view settings, and the master gain.

A preset stores which sample *slot* a Click layer uses, not the file. Renaming or reordering the files in `resources/clicks/` changes what those presets play.

This is the same state your DAW saves with its project, so a patch that sounds right in the plugin will sound right when you reopen the session.

Older `.doof` files still load. A preset saved before layers existed opens as the single-layer patch it described, with the other four layers returned to their factory state.

---

## Not built yet

So you know where the edges are:

- **Sub tuning and phase tools** (Phase 6).
- **FX, routing, EQ, analyzer, master limiter** (Phases 7–9).
- **Sidechain triggering** (Phase 10).
- **Preset browser and drag-out export** (Phase 11) — for now, presets are plain `.doof` files you manage yourself.
- **The finished skin** (Phase 12). The current flat dark look is a working surface, not the intended design.
