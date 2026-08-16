# Known Issues

## Resolved

- **Target Accuracy had little effect on real maps.** The old probability controller
  used a feed-forward of `(100 - target)·0.015` that assumed every object was a circle
  and a weak clamped feedback term. On maps with sliders/spinners (guaranteed 300s) it
  systematically converged **above** target (offline: 95% target → 96.9%, 98.5% → 99.0%
  on a 45%-slider map). Replaced with a debt-based integral controller that accounts for
  object-type dilution exactly and only spends controlled 100s on isolated circles.
  Offline convergence now within 0.08% of target across pure-circle → 65%-slider maps.
  (`core/relax/relax.hxx`)

- **Autobot jump on slider/spinner/choreography exit.** When motion returned to the
  gameplay quintic after a frame driven by the slider follower, spinner orbit, or
  decorative choreography, a quintic left over from priming still held a stale start
  point/time, so the first gameplay frame evaluated it away from the current cursor.
  Fixed with an owner-transition re-anchor that replans from the actual current state
  (`diagnostics.owner_transition_replans`).

- **Decorative wandering during ordinary slow sections.** The break-motion gate treated
  any gap `> 1800 ms` as a break and let the cursor wander to a decorative staging point
  mid-map. Raised to `> 2600 ms` with a larger return-budget margin so only genuine
  breaks trigger decorative motion.

## Monitoring (instrumented, not yet reproduced live)

- **Residual random jumps during active gameplay.** Could not be reproduced offline
  (osu! not available in this environment). A bounded bad-delta trace buffer now captures
  the full provenance (owner, previous owner, source, object index, dt, displacement vs.
  kinematic bound, resync state) of any motion sample that exceeds its kinematic bound,
  surfaced in the Autobot diagnostics panel (`Off-trajectory events` / `Last jump`).
  When a live jump occurs, read that panel: it attributes the jump to a category
  (owner transition, external resync, dt spike, slider/spinner entry snap, or gameplay
  trajectory) so the exact remaining cause can be fixed with certainty.

- **Slider/spinner entry snap (candidate, not yet changed).** `update_slider` sets
  `desired ≈ ball` and `update_spinner` snaps to the orbit radius on the first active
  frame; if the approach left the cursor far from the head/center this produces a
  one-frame snap proportional to the approach error. Left unchanged pending confirmation
  from the trace buffer, because the slider/spinner follow + tail path is approval-gated
  and a well-timed approach keeps the snap sub-pixel. If the trace attributes live jumps
  to `SLIDER`/`SPINNER` entry, smooth the first N ms of the follow.

- **Transient playfield-rect reads.** `get_playfield_rect` can momentarily return an
  inconsistent rect during window state changes (alt-tab, resolution/fullscreen toggle),
  which would map a continuous internal position to a different screen point for one
  frame. Environmental; the trace buffer's `dt` / owner fields help distinguish this from
  an internal cause.
