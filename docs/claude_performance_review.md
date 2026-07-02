# Performance review: Stage 7 retained GPU backend

Analysis of why the app burns ~25% CPU and ~25% GPU **at idle** after the Stage 7
(Direct3D 11 / QRhiWidget) change, and why pan/zoom did not get smoother. All
findings are from code inspection of the current worktree; each one cites the
mechanism and the file/line evidence.

The one-line summary: **the retained GPU layer itself is sound and event-driven,
but the ChartView integration around it creates an infinite repaint loop, and it
moved the most expensive CPU raster work (point symbology) from a
render-once-then-blit cache onto a per-frame path.** The backend is doing a
cheap GPU frame plus an expensive CPU frame, 60 times per second, forever.

---

## Finding 1 (critical): self-sustaining repaint loop in GPU mode

**This is the direct cause of the constant 25% CPU + 25% GPU at idle.**

The GPU-mode widget stack is three widgets deep:

- `ChartView` (parent, gets all input, paints nothing in GPU mode)
- `gpuLayer_` — the `QRhiWidget` chart surface, bottom of the stack
- `overlayLayer_` — a **translucent** plain `QWidget` on top
  (`WA_TranslucentBackground`, chart_view.cpp:1652-1658) that draws the dynamic
  pass via an event filter (chart_view.cpp:1622-1631)

The loop:

1. Any paint of `ChartView` in GPU mode runs this branch
   (chart_view.cpp:1534-1538):

   ```cpp
   if (useGpu_ && gpuLayer_) {
       syncGpuCamera();                             // -> gpuLayer_->update()
       if (overlayLayer_) overlayLayer_->update();  // schedule overlay repaint
       return;
   }
   ```

2. `overlayLayer_->update()` marks the overlay dirty. Because the overlay is
   **not opaque** (translucent background, no `WA_OpaquePaintEvent`), Qt's
   repaint manager must re-render everything *underneath* it to composite the
   translucency — which means the parent `ChartView::paintEvent` is invoked
   again for that region on the next backing-store sync.

3. That `paintEvent` again calls `overlayLayer_->update()` (and
   `syncGpuCamera()`), scheduling the next cycle. `update()` calls made during
   painting are deferred to the next event-loop pass, so this never converges —
   it just runs at the backing-store flush rate (~vsync, 60 Hz) forever.

So after the very first paint (window show, first GPS fix, anything), the app
repaints the full window at ~60 fps even when absolutely nothing changed. Each
cycle does:

- `syncGpuCamera()` → `GpuChartView::setCamera()` → **unconditional**
  `update()` (gpu_chart_view.cpp:51-56) → a full RHI re-record + re-render of
  every retained vertex (no change detection, no culling) → GPU load;
- a full overlay repaint → `paintDynamic()` → **the entire point-symbology
  pass** (Finding 2) in software → CPU load;
- because the overlay covers the whole window and repaints fully, the raster
  backing store for the whole window is re-uploaded to the GPU and
  re-composited with the RHI texture every frame (at 1080p that alone is
  ~8 MB × 60/s ≈ 500 MB/s of upload bandwidth) → more CPU *and* GPU load.

That is exactly the observed symptom: ~one core of CPU plus ~25% GPU with the
app sitting idle. In painter mode none of this exists — the widget only paints
when `update()` is requested, which is why idle CPU used to drop to 0-3%.

Note the repaint governor (chart_view.cpp:194-200), which was specifically
built to stop data feeds from driving the paint rate, is completely bypassed by
this loop: paints are no longer driven by data at all, they drive themselves.

### Fix

The rule being violated: **a paint handler must never schedule the next paint.**
`ChartView::paintEvent` is the wrong place to push state to the child layers.

- Make the GPU-mode branch of `paintEvent` a pure no-op (just clear the
  pending-repaint flag and return). Better still, in GPU mode set attributes so
  the parent doesn't get painted at all.
- Move `syncGpuCamera()` + `overlayLayer_->update()` into an explicit
  `refreshGpuFrame()` helper, and call it from the places that *cause* a frame:
  the pan/zoom handlers (`mouseMoveEvent`, `wheelEvent`, `zoomBy`), the settle
  timers, `requestRepaint`'s timer, cell/raster arrival, and setting changes —
  i.e. everywhere the code currently calls `update()` for the painter path.
- Add change guards to `GpuChartView::setCamera` / `setScene` /
  `setRasterLayer`: if the camera/scene is unchanged, do not call `update()`.
  This is a cheap belt-and-braces backstop that makes accidental loops
  self-extinguishing.

Expected result from this fix alone: idle CPU and GPU return to ~0 (a truly
idle window paints nothing), and data-driven updates go back through the
coalescing governor at ≤16 Hz.

---

## Finding 2 (critical): full point symbology re-rasterized every frame in GPU mode

**This is the main reason panning is not smoother than the painter path — it is
actually doing more CPU work per pan frame than before.**

In painter mode the expensive constant-screen-size chart symbology — S-52 area
patterns, complex lines (LC), soundings (`drawText` per sounding), symbol
pixmap blits, light sectors, text labels — is baked into the static pixmap
cache once on settle, and mid-gesture frames are a single cached blit
(chart_view.cpp:1551-1574, comment at 2026-2028 confirms the design).

In GPU mode, that entire pass was moved onto the **per-frame** dynamic path:
`paintDynamic()` calls `drawPointSymbology()` on every overlay repaint
(chart_view.cpp:1595-1602) — and the overlay repaints on every frame (every
mouse-move during a pan, and, with Finding 1, 60×/s at idle). There is no
`interacting_` gate in `drawPointSymbology` (the check at chart_view.cpp:2029
only tests `pointLodVisible_`); the comment block at 2017-2024 describes
gesture-skipping that the painter path achieves via the cache, but the GPU path
gets neither the cache nor the gate.

At harbour detail with a positive detail level this pass is thousands of
`QPainter::drawText` calls (CPU glyph rasterization), pattern fills, and
per-vertex `t.map()` polyline conversions for every LC path — per frame, into
an ARGB32 surface, with antialiasing on. This dwarfs the cost the GPU backend
removed (the area/line fills, which were already cheap blits from the static
cache during pans).

Net effect: GPU mode pan frame = cheap GPU uniform update **plus** the most
expensive part of the old settle-time render. Painter mode pan frame = one
pixmap blit. GPU mode loses.

### Fix (staged)

1. **Stopgap (small):** gate the symbology passes in GPU mode on
   `!interacting_`, the same trade-off the painter path makes mid-gesture
   (symbols/text pop back on settle, chart geometry keeps moving under the
   finger — that's what the current painter build ships).
2. **Proper (medium):** give the GPU path the same treatment as the painter:
   render `drawPointSymbology` into a cached pixmap/texture with an apron on
   settle, and blit/translate it mid-gesture. This can even reuse the existing
   `staticCache_` logic with the base-chart drawing skipped.
3. **Target state (the architecture plan's Stage 8+):** symbols as instanced
   textured quads from the atlas, text via a glyph atlas with screen-space
   declutter, patterns/complex lines as GPU batches. That is what makes pan
   cost truly camera-only, per the plan's "Frame loop" section.

---

## Finding 3 (high): full-window translucent overlay is an expensive composition model

Even with Findings 1-2 fixed, the dynamic pass architecture has a structural
cost: `overlayLayer_` is a full-window, per-pixel-alpha raster widget stacked
over an RHI texture widget. Every overlay repaint:

- rasterizes with QPainter into the backing store (CPU),
- forces upload of the dirtied backing-store region (full window, since
  ownship/scale bar/AIS repaints call full-widget `update()`) to a GPU texture,
- and composites backing store + RHI texture.

The painter path, by contrast, flushes one opaque surface with dirty-region
blits.

### Fix

- Short term: repaint the overlay in *dirty rectangles* (ownship glyph area,
  scale bar corner, AIS targets' union) instead of full-widget `update()`.
- Medium term: draw the dynamic pass inside the RHI frame itself. QRhi can
  render QImage-sourced textures cheaply; ownship/AIS/route overlays are a few
  hundred triangles at most. This removes the translucent raster widget — and
  with it both the per-frame full-window upload and the Finding 1 loop
  *mechanism* (an opaque QRhiWidget child never forces parent repaints).

---

## Finding 4 (high): every pan/zoom settle rebuilds and re-uploads the whole GPU scene

On **every** gesture settle, the 180 ms `aaTimer_` sets `gpuSceneDirty_ = true`
unconditionally (chart_view.cpp:177), and the next frame runs
`rebuildGpuScene()` (chart_view.cpp:1694-1742) **synchronously on the GUI
thread inside the paint cycle** (via `syncGpuCamera`, chart_view.cpp:1773).
That rebuild:

- copies and re-bases **every vertex** of the basemap + all active cells into
  fresh `std::vector`s (potentially millions of `GpuVertex` at fine bands —
  see Finding 5 on why the counts are inflated),
- destroys and recreates four `Immutable` GPU buffers and re-uploads them
  (gpu_chart_view.cpp:209-243),
- and re-runs `composeGpuRaster()` (chart_view.cpp:1744-1769): a full-viewport
  ARGB image fill + tile composite + `convertToFormat(RGBA8888)` copy
  (gpu_chart_view.cpp:66) + full texture upload — even when the raster layer
  didn't change.

So the end of every pan produces a visible hitch — the user experiences this
as "panning still isn't smooth," because the smoothness of the mid-gesture
frames is judged together with the stutter at release. The same full rebuild
also fires per arriving cell during load-in (chart_view.cpp:951) and on quilt
topology changes (chart_view.cpp:794), so zooming across band boundaries pays
it repeatedly.

The only reason to re-base at all is float32 precision. Re-centring on every
settle is far more aggressive than needed.

### Fix

- Re-base only when required: track distance between the camera and
  `gpuSceneO*`; rebuild when the float32 error at the current `ppm_` would
  exceed a fraction of a pixel (in practice, tens of kilometres at typical
  zooms). A pan across a harbour should *never* re-base.
- Better: keep **one GPU buffer set per cell**, uploaded once when the cell is
  built, each drawn with its own origin passed through the (already dynamic)
  camera uniform. Scene assembly then becomes reordering a draw list — no
  vertex copying, no re-upload, and cells can be culled individually. This also
  removes the concatenated CPU copy (Finding 6).
- `composeGpuRaster` should run only when the tile set actually changed
  (`gpuRasterDirty_` is already tracked for that — stop calling it
  unconditionally from `rebuildGpuScene`, chart_view.cpp:1739).

---

## Finding 5 (medium): GPU line batches are unsimplified and unclipped

`gpubatches::appendCellBatches` emits outline/line geometry from the **raw**
`f.rings` (gpu_batches.cpp:81-102): full-resolution, unclipped source
geometry, two vertices per segment (line list), 20 bytes each.

The painter path never draws that: `instantiateCell` clips rings to the keep
area and simplifies them per band (`simplifyToleranceM`, chart_view.cpp:102-111
— e.g. 1.4 km tolerance at overview scale). The GPU path skips both steps, so:

- vertex counts per cell are far larger than what the painter drew (an
  overview-band coastline can be hundreds of thousands of segments),
- every `rebuildGpuScene` copies all of it (Finding 4 multiplier),
- and the GPU rasterizes all of it every frame with no culling.

Fills are better (pre-triangulated in `compileScene`) but are also whole-cell,
exterior-ring-only, and drawn without any viewport culling.

### Fix

- Emit GPU line batches from the same clipped/simplified geometry the painter
  uses (the `BuiltPath` polylines), or apply `simplifyToleranceM(band)` +
  clip-box clipping inside `appendCellBatches`. Optionally keep two LODs per
  the architecture plan.
- With per-cell buffers (Finding 4 fix), cull cells against the viewport
  before drawing.

Correctness footnote (not perf): exterior-ring-only triangulation
(render_scene_compiler.cpp:51-65, "holes deferred") means lakes/holes inside
land polygons fill solid on the GPU path where the painter respected them.

---

## Finding 6 (medium): triple-buffered vertex data in RAM

Each cell's GPU batches currently exist in three places at once:

1. `BuiltCell::gpuTris/gpuLines` in `loaded_` (kept for reassembly),
2. the concatenated copies inside `GpuChartView::baseData_/triData_/...`
   (kept because `sceneDirty_` upload happens later, but never freed after
   upload — gpu_chart_view.cpp:103-106 members),
3. the GPU buffers themselves.

At fine-band vertex counts this is hundreds of MB of duplicated float data.
The per-cell-buffer design (Finding 4) collapses all of this: upload once at
build, free the CPU copy, done.

---

## Finding 7 (low): assorted smaller costs

- **`Immutable` buffer churn:** `setScene` destroys and recreates all four
  vertex buffers on every scene change (gpu_chart_view.cpp:209-243). Reuse a
  `Dynamic` buffer when the new data fits the existing allocation.
- **`setCamera`/`setScene`/`setRasterLayer` never check for no-op input**
  (gpu_chart_view.cpp:33-72). Cheap guards prevent redundant GPU frames and
  make repaint loops self-limiting (see Finding 1 fix).
- **`composeGpuRaster` on every decoded tile:** each arriving MBTiles tile sets
  `gpuRasterDirty_` (chart_view.cpp:1247), and each recomposite is a
  full-viewport CPU composite + format conversion + full texture upload.
  During initial load over a raster chart this runs dozens of times. Batch it
  behind a short single-shot timer.
- **Stale painter cache retained in GPU mode:** `staticCache_` keeps its last
  painter-mode pixmap (up to ~1.5× window × dpr²) alive while unused.
- **D3D11 line-list rendering is 1-px, non-AA** — a visual regression vs. the
  cosmetic-pen painter lines rather than a perf issue, but it will read as
  "worse" in comparisons. Triangulated wide lines are already called for in
  the architecture plan.

---

## Why this contradicted the architecture plan

The plan's frame-loop contract (docs/renderer_architecture_plan.md, "Layer 6")
is: *on pan/zoom, update the camera and redraw retained batches; when idle, do
nothing.* Stage 7 implemented the retained batches correctly, but the
integration violated the contract twice: the view repaints without a cause
(Finding 1), and the per-frame path contains prepare-time work (Finding 2).
The plan's Phase 0 telemetry ("lightweight frame timing telemetry ... paint
ms ... counts") was never added, which is why a 60 fps idle loop shipped
invisibly. Recommend adding it before the fixes so improvement is measurable:

- a per-second log/overlay of: `ChartView::paintEvent` count, overlay paint
  count, RHI frame count, `rebuildGpuScene` count + ms, symbology-pass ms;
- acceptance: with the app idle, **all counters must read 0**.

---

## Prioritized fix plan

| # | Fix | Effort | Expected effect |
|---|-----|--------|-----------------|
| 1 | Break the repaint loop: no child `update()` from `paintEvent`; drive GPU/overlay frames from input/data events; no-op guards in `setCamera`/`setScene`/`setRasterLayer` | Small | Idle CPU ~25% → ~0-3%, idle GPU ~25% → ~0. Restores the repaint governor. |
| 2 | Stop per-frame symbology: gate on `interacting_` now; cached symbology layer (pixmap with apron, blitted mid-gesture) next | Small / Medium | Pan frames drop from "full S-52 raster" to near-zero CPU; this is where pan smoothness is won. |
| 3 | Dirty-rect overlay repaints; longer term move the dynamic pass into the RHI frame | Medium | Removes full-window backing-store upload per frame. |
| 4 | Re-base the GPU scene only past a float-precision distance threshold; stop raster recomposite unless tiles changed | Small | Removes the settle hitch after every pan/zoom. |
| 5 | Per-cell GPU buffers with per-draw origin; upload at build, cull per cell, free CPU copies | Medium | Removes rebuild copies/uploads entirely; fixes memory triplication. |
| 6 | Clip + simplify GPU line batches per band (reuse painter geometry) | Small/Medium | Cuts vertex counts by 10-100× at coarse bands; faster uploads and GPU frames. |
| 7 | Telemetry counters (Phase 0 of the plan) | Small | Makes all of the above verifiable; catches future regressions. |

Items 1, 2 (stopgap), 4, and 7 are together roughly a day of work and should
recover both reported regressions: idle usage back to painter-era levels, and
pan cost strictly below the painter path (uniform update + cached blits). Items
3, 5, 6 then take the backend to the plan's actual target: pan/zoom cost that
is camera-only, with the GPU doing all static chart work.

## How to verify after fixing

1. Idle test: app open on a harbour view, no data feed — Task Manager CPU ≈ 0%,
   GPU ≈ 0%; telemetry counters at 0 paints/sec.
2. Idle with live GPS/AIS: paint rate ≤ the 60 ms governor (~16 Hz), CPU in low
   single digits (dominated by the dynamic overlay, per the plan's success
   metric).
3. Pan test: continuous drag at harbour detail — frame time well under 16 ms,
   no settle hitch on release (no `rebuildGpuScene` in the log unless the
   quilt changed).
4. Side-by-side with OpenCPN and the painter backend on the same chart set, per
   the plan's Phase 5 exit criteria.
