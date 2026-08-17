# lamev2 — Architecture Notes

External osu! Stable/Lazer utility for a **private server** where gameplay
assistance/simulation is permitted. Architecture is strictly:

```
read game state (memory, read-only)  ->  compute movement/timing  ->  emit OS input
```

No game-memory writes, no injection, no drivers. Do not add any.

---

## Top-level flow

`core.cxx` starts a `threads::c_cache` (memory reader) and a `ui::c_overlay`
(DirectX11 + Dear ImGui overlay). Each frame the cache produces a
`osu::full_snapshot_t` (game state + parsed beatmap); the overlay ticks the
modules and renders the menu.

Module tick order (in `c_overlay::tick_modules`, per frame during play):

```
aim.update()
if autobot.enabled: relax.prepare_for_autobot(game, map, target_accuracy)   // schedule only
else:               relax.update(game, map)                                 // schedule + flush
replay.update()
if autobot.enabled: autobot.update(...);  relax.flush_for_autobot(game)      // move, then press
else:               autobot.update(...)   // early-returns when disabled
tap_assist.update()
```

### Key/press ownership (there is exactly one owner)

- **Autobot OFF, Relax ON** — `relax.update()` schedules *and* flushes presses. Standalone.
- **Autobot ON, Relax ON or OFF** — `relax.update()` is **not** called. Autobot uses the
  shared scheduler via `prepare_for_autobot()` (schedules, `force_enabled=true`, no flush),
  moves the cursor to the final scheduled position, then `flush_for_autobot()` emits the
  presses. Single owner, no duplicate queue, no double presses.

Standalone Relax timing is byte-for-byte identical whether or not the accuracy
controller exists — every accuracy-mode branch is guarded by
`m_autobot_accuracy_mode`, which is only true inside `prepare_for_autobot()`.

---

## Autobot movement model

**ONE MotionState. ONE MovementPlan. ONE output path.**

The engine is split so the whole planner + evaluator can be unit-tested off-line:

- **`core/autobot/motion_core.hxx`** — the pure, host-independent engine
  (`motion_engine_t`). No Windows / Relax / projection. All movement math lives
  here: MotionState, MovementPlan, the quintic evaluator, slider geometry +
  follower, spinner integrator, the planner, the commit/continuity check, and the
  flight recorder. Everything happens in osu! playfield coordinates.
- **`core/autobot/autobot.hxx`** — the thin Windows adapter (`c_autobot`). It reads
  the OS cursor and projects it to playfield space, turns the shared Relax
  scheduler into a `schedule_view_t`, drives `motion_engine_t::update()` once per
  frame, projects the engine's single position back to the screen and emits it, and
  owns the two screen-space safeguards the pure engine cannot see (persistent
  external-cursor mismatch, one anomalous playfield rect).

Mental model, evaluated once per frame:

```
current MotionState + current MovementPlan + current time  ->  next cursor position
```

**No teleporting.** The only direct position assignment is seeding the MotionState
from the observed cursor at session start / re-anchor. After that every position
evolves continuously.

### The one plan (`plan_t`, `plan_type_t`)

Exactly one plan owns the cursor: `gameplay`, `recovery`, `slider`, `spinner`,
`startup`, or `break_decor`. A trajectory plan (gameplay / recovery / decorative
segment) is a timed **quintic Hermite** curve from the captured `(p0,v0,a0)` to
`(p1,v1,0)`. Slider-follow and spinner plans are evaluated procedurally against
geometry but carry their captured entry state so the transition in is continuous.

Trajectory model = **C2-continuous quintic (minimum-jerk-style) with a fixed timed
arrival**, chosen for: position+velocity+acceleration continuity across replans (no
kink when a new plan starts mid-flight), exact arrival at the scheduled press
timestamp, closed-form (numerically trivial) evaluation, and stable replanning
because every new segment reads the current `(p,v,a)`.

### The planner (`plan_and_reconcile`)

Each frame picks the single authoritative plan:

1. `find_active_long()` — a slider/spinner inside `[press, end]` owns (`slider` /
   `spinner`).
2. else the next object (`last_completed + 1`) is the target. If we are late
   (`time_left <= 0`) → `recovery` toward the **same** object. Otherwise decorative
   motion is allowed **only** when the return-time budget proves the object is still
   reachable (`time_left > acquisition_lead + safety + slack`); then `startup`
   (before the first note) or `break_decor`. Otherwise `gameplay`.
3. no objects left → local `break_decor` idle.

A plan is created **only** on an identity change (type, object index, or a scheduled
arrival that moved > 12 ms) — never per frame, so there are no replan loops or stale
reuse. Every new plan begins from the **actual current MotionState**.

**Decorative motion is local and bounded.** Startup/break segments are short quintic
hops around an anchor set to the cursor's *current* position, with damped inherited
velocity. There is no far-away staging point, so decorative motion can never read as
a jump to an unrelated location, and the return to gameplay is an ordinary continuous
plan from wherever the cursor is. Because the planner re-evaluates the return budget
every frame, decorative motion yields to gameplay acquisition automatically — a
decorative plan can never own the cursor while acquisition is required.

### Immutable target points

`target_point(index)` selects one interior point per object (spinners get an orbit-
entry point at radius ~70) and caches it write-once. It never regenerates on replan,
schedule refresh, lookahead, or UI frame.

### Sliders / spinners (continuous entry, no snap)

- **Slider:** on entry the plan captures `entry_offset = position - ball`; each frame
  `desired = ball(t) + entry_offset·e^(-age/45ms)`, so the first follow frame equals
  the incoming position (offset = full) and converges onto the ball. Slider Laziness
  keeps it loose. Exit is a normal gameplay plan from the slider-exit MotionState.
- **Spinner:** the integrator starts from the actual incoming angle / radius /
  direction (no snap to centre or to full RPM); the cosmetic ellipse blends in by the
  entry factor and the radius ramps via a spring, so the first orbit frame matches the
  incoming point. RPM is a ramped target.

### Continuity budget / flight recorder

`commit_motion` writes the MotionState and checks every step against a kinematic
budget (`max(|v|)·dt + ½·max(|a|)·dt² + slack`). A produced position is always
finite and (by construction) within budget; any violation increments
`unexpected_discontinuities` (**expected 0**) and is captured in a bounded 24-entry
**flight recorder** with the exact plan id/type, object, dt, displacement vs. bound,
and reason — so a live jump can be attributed to its plan after the fact.

### External cursor feedback (adapter)

The adapter emits absolute positions and remembers the last one. A single
stale/delayed `GetCursorPos` sample must not tear down a plan: re-anchor requires the
observed cursor to differ from the last emitted point by **> 6 px for 3 consecutive
frames**; only then does `engine.reanchor()` rebase on the observed state and let the
next plan start from there (`diagnostics.external_reanchors`).

### dt / clock / geometry

`real_dt` (steady_clock) is clamped to `[0.001, 0.025] s`; a hitch or tab-out cannot
snap. `control_time = max(game.cur_time, floor(relax.prepared_game_time()))` drives
quintic progress, so arrival lands on the scheduled press game-time. A backward jump
of `> 200 ms` (retry/rewind) starts a new session. One projection helper
(`impl/util/playfield.hxx`); `verify_projection()` still cross-checks it into
`diagnostics.projection_self_check_error`. The adapter holds the frame (no emit) if a
single anomalous playfield rect would turn a small internal move into a huge screen
jump.

### Diagnostics

`diagnostics_t` describes the architecture directly: live plan type/id, object index,
progress, time-to-arrival, MotionState position/velocity/acceleration, target and
requested/observed cursor; plus counters (plans created, gameplay/slider/spinner
plans, startup/break segments, recovery plans, plan invalidations, external
reanchors, unexpected discontinuities, objects completed). Surfaced in the Autobot
diagnostics panel.

---

## Target Accuracy (`core/relax/relax.hxx`, autobot-accuracy mode only)

Private-server simulated-performance control. **Timing outcomes only — never cursor
teleportation.**

osu! standard accuracy = `(300·n300 + 100·n100 + 50·n50) / (300·N)`; per-object
fraction 300→1, 100→1/3, so each controlled 100 costs `2/3` of accuracy loss.

A **debt-based integral controller** drives long-run accuracy to the target:
every object adds `(1 - target)` to a running loss debt; when the debt reaches half
of one 100's worth it is spent on the next **suitable** circle. Suitable = a circle
with both neighbour gaps ≥ 170 ms (isolated, so the ordering guard never clobbers the
placed press and the outcome reliably registers). Because the debt counts *every*
object but is only spent on circles, slider/spinner dilution is handled exactly — the
previous probability controller assumed all-circles and overshot toward 100% on real
maps. Controlled 100s are spaced ≥ 3 objects apart (anti-clumping).

The controlled press is placed firmly inside the map's 100 window using the actual OD
hit windows (`window_300 = 79.5 - 6·OD`, `window_100 = 139.5 - 8·OD`, HR/EZ applied),
between `window_300 + 3 ms` and `window_100 - 4 ms`, sign chosen randomly but never
crossing the previous hit. `accuracy_telemetry()` exposes target, projected final
accuracy, controlled-100 count, and outstanding debt.

Offline simulation converges to within **0.08%** of target for 95/97/98.5/99% across
pure-circle → 65%-slider maps.

---

## Frozen / approval-gated systems

Do **not** substantially change without owner approval: Relax timing distributions,
Advanced Singletap, Aim Assist, Adaptive Aim, the Lazer runtime resolver, the slider
-tail resolver, low-level input, and the memory-reading architecture. Autobot
*integration* may use small shared helpers, but standalone behavior of these must be
preserved.
