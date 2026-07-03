# Performance fix plan — Stage 7 GPU backend regression

Date: 2026-07-02

Status: Phases 0-3 implemented 2026-07-02. Telemetry logs on the
`chart.telemetry` category (enable/disable with
`QT_LOGGING_RULES="chart.telemetry.debug=true|false"`); per-cell vertex counts
are logged at each cell push, and the per-second dump reports drawn vertices
after culling.

Phase 2 notes: Step 2.1's gate was skipped in favour of going straight to Step
2.2 (the symbology cache reuses `staticCache_` with a transparent background in
GPU mode, so symbols stay visible mid-gesture as a translated blit); raster
composites centre on the settled camera with a quarter-viewport apron.

Phase 3 notes: Step 3.1 — line batches now come from the BuiltCell's clipped +
simplified `BuiltPath` geometry (painter parity by construction, including the
basemap); fills are triangle-bbox-filtered to the clip box; depth contours have
their own retained bucket so the toggle is a draw-list change. Step 3.2 —
`GpuChartView` retains one buffer set per cell, uploaded once at build with the
CPU copy freed immediately (fixes the memory triplication); draws use per-cell
camera uniform slices (dynamic UBO offsets), so pan/zoom is camera-only and the
absolute-camera design removes the global scene origin — Step 2.3's re-base
threshold became obsolete and was deleted; cells are culled individually per
frame; device loss re-pushes via `GpuChartView::deviceLost`. Deferred from
Phase 3: Step 3.3 (dirty-rect overlay repaints) — needs per-overlay dirty
tracking or the RHI-integrated dynamic pass, better done with Stage 8 (Step
4.3). Quilt draw-clips are still approximated by coarse-first overprint on the
GPU (needs stencil to be exact).

Phase 4 notes (Steps 4.1, 4.2, 4.4 implemented; 4.3 and 4.5 open): Step 4.1 —
in GPU mode the build worker drops every BuiltPath without AP/LC symbology
after batch extraction (the GPU draws those from retained buffers; only the
constant-size AP/LC pass still strokes paths with QPainter), and basemap
builds drop all paths. Step 4.2 — the full-viewport raster composite is gone:
tile selection (native zoom, ancestor fallback, requests, eviction) was
factored into `ChartView::selectRasterTiles`, shared by the painter (blits)
and the GPU path (`pushGpuRasterTiles`), which retains one texture per tile in
`GpuChartView` and draws textured quads; only never-seen tiles convert and
upload, and an unchanged selection is a no-op. Step 4.4 — fills are hole-aware
(`geomtess::mergeHoles`, earcut-style bridging; detached outer rings of
multi-polygon features fill independently; islands nested inside holes remain
best-effort); `kPreparedRenderFormat` bumped to 2, so on-disk prepared-render
caches regenerate on first load. Still open: Step 4.3 (GPU symbol/text
pipeline — the Stage 8 project; the settle-time symbology cache from Step 2.2
covers the perf need until then), Step 3.3, exact quilt clipping (stencil),
and Step 4.5 — flipping `Auto` back to GPU-when-available once the acceptance
gates in this plan pass on the reference machine.

This plan consolidates `docs/codex_performance_review.md` and
`docs/claude_performance_review.md` into one ordered implementation sequence.
Both reviews were verified against the current worktree; every finding cited
below was confirmed in code.

## Where the reviews agree (verified)

| Problem | Codex | Claude | Evidence |
|---|---|---|---|
| GPU enabled by default despite being incomplete | F1 | — | `settings.cpp:99` falls back to `Auto`; `render_backend.hpp` resolves Auto→GPU when D3D11 probes OK |
| Self-sustaining repaint loop in GPU mode | (F5, partial) | F1 (critical) | `chart_view.cpp:1534-1538`: `paintEvent` schedules `overlayLayer_->update()`; translucent overlay forces parent repaint → loop at ~60 Hz |
| S-52 point symbology re-rasterized per frame in GPU mode | F4 | F2 (critical) | `chart_view.cpp:1595-1602`: `paintDynamic` calls `drawPointSymbology` on every overlay repaint; painter path bakes it into `staticCache_` |
| No no-op guards on GPU setters | F5 | F7 | `gpu_chart_view.cpp:33-72`: `setCamera`/`setScene`/`setRasterLayer` always `update()` |
| Full scene rebuild + re-upload on every settle | F8 | F4 | `chart_view.cpp:177` sets `gpuSceneDirty_` unconditionally; `rebuildGpuScene()` (1694-1742) copies every vertex and recreates all buffers |
| GPU batches unclipped/unsimplified/option-blind | F3 | F5 | `gpu_batches.cpp` walks raw `f.rings`; ignores clip box, band simplification, quilt clip, `showDepthContours_` |
| Raster = full-viewport CPU composite + full texture upload | F7 | F7 | `composeGpuRaster()` (chart_view.cpp:1744-1769); called unconditionally from `rebuildGpuScene` (1739); per-tile dirtying |
| AIS CPA/range setters emit unconditionally → idle repaints | F6 | — | `ais_target_store.cpp:189-207`; `CpaCalculator` runs every second per target |
| Vertex data triplicated in RAM | — | F6 | `BuiltCell::gpuTris` + `GpuChartView::baseData_/triData_` + GPU buffers |
| No frame telemetry to catch any of this | F9 | yes | none exists in release builds |
| GPU mode still runs full CPU cell build | F2 | — | `chart_view.cpp:927-938`: `instantiateCell()` then `appendCellBatches()` on top |

---

## Phase 0 — Safety + measurement (do first, before any renderer fix)

### Step 0.1: Restore CPU as the effective default
- `src/settings.cpp:99` — change the `fromKey` fallback from
  `RenderBackend::Auto` to `RenderBackend::Cpu` (matches the stated intent at
  `settings.hpp:244`).
- `src/render_backend.hpp:45` — make `resolveUseGpu()` return `false` for
  `Auto` until the GPU backend passes the Phase 4 gates. `Gpu` (explicit
  user choice) still resolves to GPU so the toggle remains usable for testing.
- Check `side_menu.cpp:345` — the "Use GPU acceleration" toggle should read
  checked only for explicit `Gpu`, not for `Auto`.
- **Acceptance:** fresh install (no QSettings key) runs the painter path;
  idle CPU back to ~0-3%.

### Step 0.2: Frame telemetry (Phase 0 of the architecture plan)
Add a lightweight counter struct in `ChartView`, dumped once per second via a
`QLoggingCategory` (enabled in release), or an optional on-screen debug overlay:
- `paintEvent` count (+ trigger reason where cheap to tag)
- overlay paint count, RHI frame count (`GpuChartView::render` entries)
- `rebuildGpuScene` count + ms; `composeGpuRaster` count + ms; texture upload count
- `drawPointSymbology` ms + label/symbol/sounding counts
- GPU vertex counts by bucket (baseTris/baseLines/cellTris/cellLines)
- repaint-request counts by source (AIS, ownship, nav, plugin)
- **Acceptance:** with the app idle and no data feed, all per-second counters
  read 0. Capture a baseline: one idle minute + one standard pan gesture, in
  both backends, `vs2022-release`, same chart set + same recorded feed.

---

## Phase 1 — Break the idle loops (small diffs, biggest win)

### Step 1.1: Paint handlers must not schedule paints
- Make the GPU branch of `ChartView::paintEvent` (`chart_view.cpp:1534-1538`)
  a pure no-op: clear the pending-repaint bookkeeping and return. Nothing in a
  paint handler may call `update()` on any widget.
- Add a `refreshGpuFrame()` helper that does what the branch does today
  (`syncGpuCamera()`; `overlayLayer_->update()`), and call it from the places
  that *cause* a frame: `mouseMoveEvent`/`wheelEvent`/`zoomBy` (pan/zoom),
  `aaTimer_` settle, `repaintTimer_` timeout, cell/basemap/raster arrival,
  visibility/setting changes, resize. Rule of thumb: everywhere the painter
  path calls `update()`, GPU mode calls `refreshGpuFrame()`.
- **Acceptance:** telemetry shows 0 paints/sec at true idle in GPU mode; with a
  live feed, paint rate ≤ the 60 ms governor (~16 Hz).

### Step 1.2: No-op guards in GpuChartView
- `setCamera` (`gpu_chart_view.cpp:51`): early-return if `centerX/centerY/ppm`
  unchanged (epsilon compare).
- `setScene` / `setRasterLayer`: skip `update()` when inputs are identical.
- These make any future accidental loop self-extinguishing.

### Step 1.3: Change-detect AIS derived-data setters
- `AisTargetStore::setRangeMeters` / `setCpaTcpa`
  (`ais_target_store.cpp:189-207`): compare against stored values with a
  visible-significance tolerance (e.g. range Δ < 1 m, CPA Δ < 1 m, TCPA Δ < 1 s
  — values that cannot change a rendered pixel or info panel digit) and return
  without emitting when unchanged.
- Alternative if per-field tolerance gets fiddly: batch the whole
  `CpaCalculator` sweep and emit one `derivedDataChanged` signal only when
  something material changed; `MainWindow` (`main_window.cpp:247`) maps that to
  a single coalesced repaint.
- **Acceptance:** with live GPS + AIS but a stationary view, repaint requests
  from CPA bookkeeping drop to near 0 when values are stable.

---

## Phase 2 — Pan smoothness + settle hitch

### Step 2.1 (stopgap): gate symbology during gestures
- In the GPU branch of `paintDynamic` (`chart_view.cpp:1595-1602`), skip
  `drawPointSymbology` while `interacting_` is true — the same trade-off the
  painter path makes mid-gesture (symbols/text pop back on settle).

### Step 2.2 (proper): cached symbology layer in GPU mode
- Render `drawPointSymbology` output into an apron pixmap (transparent
  background) on settle — reuse the `staticCache_` machinery/geometry
  (`renderStaticCache`, apron = W/4, H/4) with base-chart drawing skipped —
  and blit/translate it in `paintDynamic` mid-gesture, exactly like the
  painter path blits `staticCache_`.
- This replaces the Step 2.1 gate: symbols stay visible during pans as a
  translated blit instead of disappearing.
- Also free the stale painter-mode `staticCache_` pixmap when switching to GPU
  mode (Claude F7).
- **Acceptance:** pan frame in GPU mode = camera uniform update + two blits;
  `drawPointSymbology` ms appears in telemetry only on settle.

### Step 2.3: re-base the GPU scene only when float precision requires it
- `aaTimer_` (`chart_view.cpp:177`): stop setting `gpuSceneDirty_`
  unconditionally. Instead, in `syncGpuCamera()` compute the camera distance
  from `gpuSceneOX_/OY_` and set the dirty flag only when float32 error at the
  current `ppm_` would exceed a fraction of a pixel (threshold ≈
  `(1 << 23) / ppm_` scaled with a safety factor; in practice tens of km at
  navigation zooms). Cell-set/quilt changes still trigger a rebuild.
- Remove the unconditional `composeGpuRaster()` call from `rebuildGpuScene()`
  (`chart_view.cpp:1739`); recomposite only when `gpuRasterDirty_` (tile set
  changed) or when a re-base actually moved the raster origin.
- **Acceptance:** pan across a harbour produces zero `rebuildGpuScene` entries
  in telemetry; no visible hitch on gesture release.

### Step 2.4: coalesce raster tile replies
- Tile replies currently set `gpuRasterDirty_` per tile
  (`chart_view.cpp:1234-1247`). Route them through a short single-shot timer
  (~50-100 ms) so a burst of decoded tiles produces one recomposite + one
  texture upload instead of dozens.

---

## Phase 3 — Make the GPU scene correctly sized (medium effort)

### Step 3.1: clipped, simplified, option-aware GPU batches
- Change `gpubatches::appendCellBatches` (`src/gpu_batches.cpp`) to emit line
  geometry from the same clipped/simplified geometry the painter uses (the
  `BuiltPath` polylines produced by `instantiateCell`), not raw `f.rings`.
  This inherits keep-area clipping and per-band `simplifyToleranceM` for free.
- Honor view options at batch build or draw time: `showDepthContours_`,
  `vectorOverlay_`, and the quilt clip (`drawClip_`) so hidden coarse-band
  geometry is not drawn under finer coverage.
- Treat the basemap as the first test case (largest vertex explosion —
  full-resolution GSHHG): `maybeBuildBasemap` already produces a
  clipped/simplified `BuiltCell`; batch from that, not the raw feature set.
- **Acceptance:** telemetry vertex counts drop 10-100× at coarse bands; GPU %
  at overview zoom drops accordingly.

### Step 3.2: per-cell GPU buffers, upload once, cull per cell
- Replace the concatenated `setScene(...)` model with one retained buffer set
  per cell (and per basemap tile), uploaded once when the cell is built, each
  drawn with a per-draw origin offset through the existing camera uniform.
- Scene changes become draw-list edits (order by band, viewport cull per
  cell); no vertex copying, no re-upload, no re-base of untouched cells.
- Free the CPU-side vertex vectors (`BuiltCell::gpuTris/gpuLines` and
  `GpuChartView::baseData_/triData_/...`) after upload — fixes the memory
  triplication (Claude F6). Keep buffer reuse in mind: prefer `Dynamic`
  buffers reused in place when new data fits (Claude F7).
- This supersedes Step 2.3's threshold logic for the scene (re-base becomes a
  per-cell uniform, not a rebuild); keep the raster-origin logic.

### Step 3.3: overlay composition cost
- Short term: repaint the overlay in dirty rects (ownship glyph, scale bar
  corner, union of AIS target rects) instead of full-widget `update()`.
- Medium term: draw the dynamic pass inside the RHI frame (textured quads /
  small triangle batches) and delete the translucent overlay widget entirely.
  This removes the full-window backing-store upload per frame and removes the
  Finding-1 loop *mechanism* structurally.

---

## Phase 4 — Architecture completion + re-enabling GPU by default

### Step 4.1: split CPU and GPU scene construction (Codex F2)
- GPU mode currently pays `instantiateCell()` (QPainterPath fills, painter
  scene) *plus* GPU batch generation (`chart_view.cpp:927-938`, basemap at
  1136-1144). Once Steps 2.2 + 3.1 land, GPU mode still needs `BuiltPath`
  geometry and symbology inputs but not the QPainter fill paths — split the
  builder so each backend constructs only what it draws.

### Step 4.2: retained raster tiles
- Replace the full-viewport raster composite with retained per-tile textures
  (or a tile atlas) drawn as quads. Until then, the coalescing from Step 2.4
  plus Step 2.3's dirty-gating keeps the bridge acceptable.

### Step 4.3: GPU symbol/text pipeline (Stage 8 of the architecture plan)
- Symbols as instanced textured quads from an atlas; text via a glyph atlas
  with screen-space declutter; patterns/complex lines as GPU batches. This is
  what makes pan cost truly camera-only and retires the Step 2.2 pixmap cache.

### Step 4.4: correctness item to fix alongside 3.1
- Polygon holes: `render_scene_compiler.cpp` triangulates exterior rings only,
  so lakes/holes fill solid on the GPU path (Claude F5 footnote). Fix when
  reworking batch generation.

### Step 4.5: gate, then flip the default
- Re-enable `Auto` → GPU only when, on the reference machine in
  `vs2022-release`: idle CPU/GPU ≈ painter-path levels; pan frame time < 16 ms
  with no settle hitch; telemetry parity run (same chart set, same recorded
  feed) shows GPU ≤ CPU backend on every counter; visual parity spot-checks
  pass (including line AA — D3D11 1-px non-AA lines, Claude F7 — and hole
  rendering).

---

## Verification protocol (after each phase)

1. **True idle:** harbour view, no data feed — Task Manager CPU ≈ 0%, GPU ≈ 0%,
   all telemetry counters 0/sec. (Phase 1 exit)
2. **Idle with live GPS/AIS:** paint rate ≤ ~16 Hz governor, CPU low single
   digits. (Phase 1 exit)
3. **Pan test:** continuous drag at harbour detail — frame time < 16 ms, no
   release hitch, no `rebuildGpuScene` unless the quilt changed. (Phase 2 exit)
4. **Vertex budget:** telemetry vertex counts at overview zoom comparable to
   what the painter draws. (Phase 3 exit)
5. **Side-by-side:** CPU vs GPU backend, same chart set + recorded feed, plus
   OpenCPN comparison per the architecture plan's exit criteria. (Phase 4 exit)

## Sequencing notes

- Phase 0 + Phase 1 + Steps 2.1/2.3/2.4 are all small diffs (roughly a day
  combined) and recover both reported regressions: idle load returns to
  painter-era levels and the settle hitch disappears. Users are protected from
  day one by Step 0.1 regardless of how long the rest takes.
- Step 2.2 (cached symbology) is the single biggest pan-smoothness win and
  should land before any Phase 3 work is judged.
- Phase 3 items are independent of each other and can be done in any order;
  3.1 first (it shrinks the data every other step handles).
- Every step keeps the app shippable; the GPU backend stays opt-in (explicit
  `Gpu` setting) until Step 4.5's gates pass.
