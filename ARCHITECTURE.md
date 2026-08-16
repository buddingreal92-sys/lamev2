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

## Autobot movement model (`core/autobot/autobot.hxx`)

The cursor is **one continuous state** (`m_motion.position/velocity/acceleration`).
**It never teleports.** Every destination is reached through a committed,
time-based **quintic trajectory** (`ensure_object_trajectory` / `evaluate_trajectory`)
whose `t=0` derivatives equal the current motion state, or through the slider
follower / spinner orbit / decorative segment evaluators.

### Destination ownership (priority high -> low)

`m_destination_owner` (`slider`/`spinner` > `gameplay` > `recovery` > `startup` >
`break_dance`/`break_idle`). Decided each frame in `update()`:

1. `find_active_long_object()` — if a slider/spinner is currently active, it owns
   (`update_slider` / `update_spinner`). Decorative motion is stopped.
2. Otherwise the next object (`m_last_completed_index + 1`) is the gameplay target.
   Decorative choreography is allowed **only** for startup (before the first note)
   or a **genuine break** (gap `> 2600 ms`) *and* only while the return-time budget
   proves the cursor can get back in time (`time_left > return_time + 520 ms`).
   `gameplay_must_own = !choreography_allowed`; if a decorative owner is ever set
   while `gameplay_must_own`, `diagnostics.gameplay_owner_violations` fires.

### Immutable target points

`target_point(index)` selects one interior point per object and caches it in a
write-once shadow (`m_hit_point_shadow`). The returned value always comes from the
shadow; any divergence increments `target_point_mutations` (expected 0). Points do
not regenerate on replan, timing refresh, lookahead, UI frame, or state change.

### Replan invariants

Every replan starts from the **actual current** position/velocity/acceleration:
`ensure_object_trajectory` sets `p0 = m_motion.position`, `v0 = m_motion.velocity`,
`a0 = m_motion.acceleration`. A trajectory is only rebuilt when the target index or
the immutable scheduled arrival time changes. The scheduled arrival (not a transient
recovery deadline) is the trajectory identity key, so late recovery does not replan
every frame.

**Owner-transition re-anchor** (fixes the stale primed-trajectory jump): a
slider/spinner/choreography frame drives the cursor by means other than the gameplay
quintic, so a quintic left over from priming holds a stale `p0`/start time. On the
first gameplay frame after such an owner, the trajectory is invalidated and replanned
from the current state (`diagnostics.owner_transition_replans`).

### External cursor resync (`sync_external_cursor`)

The autobot emits absolute cursor positions and remembers the last one. A single
stale/delayed `GetCursorPos` sample must not tear down a valid trajectory:
resync requires the observed cursor to differ from the last emitted point by
**> 6 px for 3 consecutive frames**; only then does it rebase position and inherit a
bounded observed velocity/acceleration (continuity preserved, no teleport).

### dt / clock

`real_dt` (steady_clock) is clamped to `[0.001, 0.025] s` for all motion math, so a
frame hitch or tab-out cannot snap a trajectory. `control_time =
max(game.cur_time, floor(relax.prepared_game_time()))`; `prepared_game_time` is
`cur_time + wall_elapsed*speed` capped at +30 ms. A backward jump of `> 200 ms`
(map rewind/retry) starts a new session.

### Coordinates

One projection helper (`impl/util/playfield.hxx`). `playfield_to_screen` (emit) and
`screen_to_playfield` (observe) are exact inverses (0.8·height playfield, 4:3, +17 px
y-offset). `verify_projection()` cross-checks canonical vs. `project_osu_to_window`
at (0,0)/(256,192)/(512,384) into `diagnostics.projection_self_check_error`. Do not
duplicate projection math (a duplicate once put Aim Assist ~29.96 px too high).

### Diagnostics / instrumentation

`diagnostics_t` tracks provenance (current/previous source, source-change reason,
replan reason, trajectory id, object index) and health counters (owner violations,
off-route replans, target-point mutations, internal discontinuities). A bounded
**bad-delta ring buffer** (`k_trace_capacity = 24`) captures full context whenever a
motion sample exceeds its kinematic bound; `last_bad_delta()` / `bad_delta_count()`
surface it in the UI so a live random-jump can be attributed after the fact.

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
