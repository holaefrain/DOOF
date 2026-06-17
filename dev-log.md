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
| 2 | Node-based envelope engine | Not started | — |
| 3 | Envelope canvas (GUI) + minimal preset save/load | Not started | — |
| 4 | Layers + mixer | Not started | — |
| 5 | Click layers | Not started | — |
| 6 | Sub tuning + phase tools | Not started | — |
| 7 | FX suite + routing matrix | Not started | — |
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

_Nothing yet — log decisions here as they are made._

---

## Open Questions (unresolved across sessions)

_Nothing yet — log questions here as they arise; mark resolved with the date._

---

## Sessions

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
  - `memory/feedback_read_project_reference.md` — reminder to read `project-reference.md` at the start of every session.
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
