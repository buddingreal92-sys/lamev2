# Known Issues

## Resolved

- **Autobot occasional mystery movement (travels to an unrelated location, then
  resumes).** The old controller carried several overlapping state machines
  (`movement_state` × `destination_owner` × `destination_source` × replan reasons)
  plus a decorative choreography system that picked a **random staging point far from
  the object** and drove the cursor there whenever a gap exceeded a threshold. Any
  miscoordination between those machines — a stale primed trajectory, a decorative
  owner briefly winning, a far staging point during an ordinary slow section — read as
  a jump to nowhere. Rewritten around **one MotionState + one MovementPlan + one output
  path** (`core/autobot/motion_core.hxx` + `autobot.hxx`):
  - exactly one plan owns movement each frame; plans are only rebuilt on a real
    identity change (never per frame), always from the current MotionState;
  - decorative motion is **local and bounded** (short hops around the cursor's current
    position), so it can never be a far destination and can never own the cursor while
    gameplay acquisition is required;
  - continuity is enforced by a kinematic budget and any violation is recorded in a
    flight recorder (`diagnostics.unexpected_discontinuities`, expected 0).
  Verified off-line across 16 deterministic scenarios (`tests/motion_tests.cpp`, 81
  checks): finite positions, zero discontinuities, bounded per-frame displacement,
  correct plan transitions, and decorative never owning during acquisition. **Live
  osu! confirmation still pending** (osu! not available in this environment).

- **Slider / spinner entry snap.** The old follower set `desired ≈ ball` and the
  spinner snapped to the orbit radius on the first active frame, producing a
  one-frame snap proportional to the approach error. Now the slider follower blends an
  `entry_offset` to zero (first follow frame == incoming position) and the spinner
  integrator starts from the actual incoming radius/angle with the ellipse blended in
  by the entry factor. Off-line: zero discontinuities on circle→slider, slider→circle,
  circle→spinner, spinner→circle.

- **Target Accuracy had little effect on real maps.** (Unchanged in this rewrite —
  the debt-based controller in `core/relax/relax.hxx` is preserved.) The old
  probability controller assumed all-circles and overshot toward 100% on slider maps;
  the debt-based integral controller accounts for object-type dilution exactly and
  spends controlled 100s only on isolated circles. Offline convergence within 0.08% of
  target across pure-circle → 65%-slider maps.

## Monitoring (instrumented, needs live confirmation)

- **Live movement confirmation.** The rewrite proves the movement invariants off-line
  but osu! is not available here. When testing live, watch the Autobot diagnostics
  panel: `Discontinuities` must stay 0, and the flight recorder's `Last event`
  attributes any abnormal move to its exact plan (type, object, displacement vs.
  bound, dt, reanchor). If a live jump appears, that line names the responsible plan.

- **Transient playfield-rect reads.** `get_playfield_rect` can momentarily return an
  inconsistent rect during window state changes (alt-tab, resolution/fullscreen
  toggle). The adapter now holds the frame (no emit) when a screen delta dwarfs the
  playfield delta, and records a `geometry` flight event; the engine's motion stays
  continuous regardless because it works purely in playfield space. Environmental —
  confirm the guard behaves on a real fullscreen toggle.
