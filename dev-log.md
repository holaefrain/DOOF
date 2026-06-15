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
| 0 | Project scaffolding | Not started | — |
| 1 | Engine skeleton + one audible sub | Not started | — |
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
