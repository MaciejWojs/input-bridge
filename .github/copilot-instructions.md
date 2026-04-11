---
name: input-bridge
description: Workspace instructions for the input-bridge native Node.js addon project.
applyTo:
  "**/*.ts,**/*.js,**/*.cpp,**/*.hpp,**/*.h"
---

# Input Bridge Workspace Instructions

## What this project is

`@maciejwojs/input-bridge` is a low-level cross-platform native Node.js addon for simulating hardware input events.

It provides a unified JavaScript API over OS-specific input systems.

Supported platforms:
- Windows (SendInput)
- Linux (Wayland / X11 depending on backend)

---

## Core goal

Provide a reliable, low-latency, cross-platform input simulation layer.

Key requirement:
👉 correctness and timing stability are more important than architectural elegance.

---

## Input pipeline (CRITICAL MENTAL MODEL)

All input flows through this strict pipeline:

1. OS input backend (Windows / Linux)
2. Native hook layer (system APIs)
3. Event queue / batching layer
4. N-API bridge (`addon.cpp`)
5. JavaScript API (`lib/`)

👉 ALL bugs MUST be traced through this pipeline BEFORE proposing a fix.

---

## Key files

- `lib/` — JavaScript API surface (DO NOT break)
- `src/addon.cpp` — N-API bridge layer
- `src/platform_input.hpp` — shared backend interface + event queue
- `src/win/platform_input_win.cpp` — Windows SendInput backend
- `src/linux/platform_input_linux.cpp` — Linux input backend
- `src/platform_input_stub.cpp` — fallback implementation
- `binding.gyp` — native build configuration
- `playground/` — test environment

---

## Build and test workflow

Use only existing scripts:

- `bun install`
- `bun run build`
- `bun run rebuild`
- `bun run prebuildify`
- `bun run prebuildify:all`
- `bun run test:load`

Use `bun run rebuild` whenever native code changes.

---

## Debugging policy (STRICT)

When debugging issues:

- NEVER guess root cause.
- NEVER jump to refactoring as first solution.
- ALWAYS identify affected pipeline stage first.
- If unclear → request logs, reproduction steps, or raw events.
- Prefer minimal, surgical fixes over structural changes.
- Do NOT modify unrelated code paths.

---

## Debugging checklist (MANDATORY)

Before proposing any fix, determine:

- At which pipeline stage is the event failing?
  - OS backend
  - native hook
  - event queue
  - addon bridge
  - JS layer

- Is the issue:
  - event loss
  - event duplication
  - timing/latency
  - incorrect key mapping
  - platform-specific behavior

If unknown → ask for evidence instead of guessing.

---

## Change discipline (STRICT)

- Do NOT refactor unrelated code.
- Do NOT rename APIs unless explicitly required.
- Do NOT modify event pipeline structure without justification.
- Do NOT optimize prematurely.
- If a refactor is necessary → explain why BEFORE applying it.

---

## Performance constraints (CRITICAL)

This is a low-level input system.

- Input handling must be low-latency.
- Avoid blocking operations in event path.
- Avoid heavy logging in hot paths.
- Event order correctness is critical.
- Thread safety is required in native layers.

---

## Conventions

- Maintain a single JS-facing API (InputBridge or equivalent).
- Do NOT expose OS-specific APIs to JS.
- Keep platform-specific logic inside `src/win/` and `src/linux/`.
- Extend platform support via backend interface, not API changes.
- Prefer clarity over abstraction in native code.
- Write comments in English only.

---

## Forbidden actions

- Do NOT expose raw OS hooks to JavaScript.
- Do NOT break public JS API without explicit instruction.
- Do NOT replace build system (`node-gyp`, `prebuildify`).
- Do NOT introduce new dependencies unless required for OS integration.
- Do NOT refactor native core for stylistic reasons.
- Do NOT assume OS behavior without verifying backend implementation.

---

## Notes for agents

- Prefer minimal, high-signal responses.
- Base reasoning only on:
  - code
  - logs / runtime behavior
  - reproduction steps
- If uncertain → ask for more data instead of guessing.
- Treat this repository as performance-critical system software.

---

## Golden rule

Never assume runtime behavior.

Only reason from:
- actual OS backend implementation
- captured input behavior / logs
- reproduction steps
- defined input pipeline architecture