# DOOF — User Guide

DOOF is a kick-drum synthesizer plugin (VST3 + Standalone). This guide covers the envelope canvas and its current controls. It reflects Phase 3 as built so far — features not listed here (zoom/pan, Length control, preset save/load, right-click context menu) haven't been built yet.

## Loading DOOF

- **VST3**: build produces `DOOF.vst3`; install it in your system's VST3 folder (e.g. `~/Library/Audio/Plug-Ins/VST3/` on macOS) and rescan plugins in your DAW.
- **Standalone**: run the built `DOOF.app` directly. Note that on macOS, Standalone has no host play head, so the beat grid (below) never appears there — that requires a real DAW host.

## The envelope canvas

The central canvas overlays two curves on one shared time axis:

- **Cyan** — the pitch envelope (Hz)
- **Orange** — the amp envelope (unitless, 0–1)

Each curve is normalized to the canvas's full height independently, so despite their different units they share the same vertical space.

## Editing nodes

- **Move a node**: click and drag its dot. A readout near the cursor shows the node's live value and time while dragging.
- **Add a node**: set the "Editing" dropdown (top bar) to **Pitch** or **Amp**, then double-click empty space on the canvas. The new node is added to whichever curve is selected.
- **Delete a node**: double-click it, or right-click it.
- **Reshape a curve**: click and drag the curve line itself, between two nodes (not on a node dot). The curve bends to follow the cursor at the point you grabbed. Hold **Shift** while dragging for finer, slower control.

## Undo / Redo

- **Keyboard**: Cmd+Z to undo, Cmd+Shift+Z to redo (Ctrl on Windows).
- **Buttons**: "Undo" / "Redo" in the top bar do the same thing.
- Pitch and amp edits share one undo history, so undo always reverts whichever edit happened most recently, regardless of which curve it touched. A full drag (move or reshape) — even one made of many small mouse movements — undoes in a single step.

## Log / Linear pitch scale

The "Log Scale (Pitch)" checkbox in the top bar switches the **pitch curve only** between linear and logarithmic vertical mapping. It's a display-only change — it never touches the underlying envelope data or affects audio. The amp curve is always linear (its value range includes exactly 0, where a log scale is undefined).

## Beat grid

Faint vertical gridlines mark beat boundaries, synced to your host's current tempo. This only appears when hosted in a real DAW (via VST3) — the Standalone app has no tempo source on macOS.
