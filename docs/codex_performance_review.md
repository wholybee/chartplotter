# Codex performance review

Date: 2026-07-02

Scope: static code review of the current Stage 7 renderer merge at `43fd873`.
I did not run a profiler in this pass, so timings below are evidence-based
hypotheses rather than measured flamegraph percentages. The code paths line up
strongly with the reported symptoms: CPU rose from about 3 percent to about 25
percent, GPU also sits around 25 percent, and panning is still not smooth.

## Executive summary

The current GPU path is very likely slower because it is not yet a replacement
for the CPU renderer. It is an additional child render layer bolted onto
`ChartView`, while the old CPU scene construction and much of the old QPainter
chart drawing are still active.

The highest-impact problems are:

1. GPU mode is effectively enabled by default on machines where D3D11 works,
   even though comments say it should be opt-in while incomplete.
2. GPU batches are generated from full raw feature lists, not the clipped and
   simplified `BuiltCell` geometry. This can draw whole cells, hidden quilt
   regions, depth contours, and especially the full GSHHG basemap tier.
3. GPU mode still draws constant-size S-52 chart symbology with QPainter on a
   translucent overlay every repaint. The CPU painter path caches this work in
   `staticCache_`; the GPU path does not.
4. Each `ChartView` paint in GPU mode asks both child layers to repaint, and
   `GpuChartView::setCamera()` schedules a GPU render even when the camera did
   not change.
5. AIS CPA bookkeeping emits `targetUpdated` every second for every target even
   when the derived values have not materially changed, guaranteeing idle chart
   repaints on a live AIS/ownship feed.

The fastest way to validate this diagnosis is to force the CPU renderer again
and compare idle CPU/GPU and pan smoothness. The fastest safe mitigation is to
make CPU the default/fallback until the retained renderer avoids the additive
work described here.

## Architecture drift

`docs/renderer_architecture_plan.md` correctly says pan/zoom should be
transform-only for already-loaded content, static chart CPU time during pan
should be near zero, and the retained backend should draw GPU batches plus a
screen-space label pass.

The deleted previous staging document, `docs/renderer_implementation_stages.md`
from commit `0c1551c`, also said each stage must remain shippable, measurable in
`vs2022-release`, and that Stage 7 should be a retained renderer behind the
renderer seam.

Current code diverges in two important ways:

- The current tree no longer contains `docs/renderer_implementation_stages.md`,
  even though this review request referenced it.
- Stage 7 did not create an independent retained renderer behind
  `IChartRenderer`. Instead, `ChartView` now owns a `GpuChartView` child plus a
  translucent QPainter overlay while still owning the painter cache, cell
  building, overlays, raster tile logic, and invalidation.

That integration shape explains most of the regression: the app pays the old
renderer costs and new GPU costs together.

## Finding 1: GPU is enabled by default despite comments saying it is opt-in

Evidence:

- `src/settings.hpp:244` says to default to the CPU painter while GPU parity is
  incomplete, and the member initializer is `RenderBackend::Cpu`.
- `src/settings.cpp:99` overwrites that initializer using
  `chartrender::fromKey(..., RenderBackend::Auto)`. On a fresh install or
  missing setting, the default becomes `Auto`.
- `src/render_backend.hpp:17` says the retained GPU backend is not wired yet and
  Auto/Gpu currently resolve to CPU, but `src/render_backend.hpp:45` actually
  returns `gpuAvailable` for Auto and Gpu.
- `src/side_menu.cpp:345` shows the "Use GPU acceleration" toggle checked for
  every backend except `Cpu`.

Impact:

Any Windows system where `GpuChartView::isAvailable()` can create D3D11 will use
the new path by default. This is exactly the case that should have been opt-in
while the renderer is incomplete and unmeasured.

Recommendation:

Use `RenderBackend::Cpu` as the default in `Settings::Settings`, and consider
making Auto resolve to CPU until the GPU path passes parity/performance gates.
The GPU toggle can remain for manual testing.

## Finding 2: GPU mode adds CPU build work instead of replacing it

Evidence:

- `src/chart_view.cpp:927` loads or compiles a `PreparedCellRender`.
- `src/chart_view.cpp:930` still calls `cellbuilder::instantiateCell(...)`,
  which creates the CPU `BuiltCell` with `QPainterPath`, soundings, symbols,
  light sectors, and texts.
- Only after that, in GPU mode, `src/chart_view.cpp:938` calls
  `gpubatches::appendCellBatches(...)` and stores extra GPU vectors in the same
  `BuiltCell`.
- Basemap build does the same pattern at `src/chart_view.cpp:1136` through
  `src/chart_view.cpp:1144`.

Impact:

GPU mode does not remove the CPU path's retained Qt objects. It creates them and
then creates additional GPU data. Cell loads, pan-settle rebuilds, backend
switches, and basemap rebuilds all get more memory traffic and more worker work
than the old path.

Recommendation:

Split CPU and GPU scene construction. A true GPU backend should not instantiate
`QPainterPath` objects for chart fills/lines just to throw most of that work away
at draw time. If the current hybrid must remain temporarily, treat it as an
experimental backend and keep the CPU path default.

## Finding 3: GPU batches ignore clipping, simplification, quilting, and view options

Evidence:

- `src/gpu_batches.cpp:46` takes raw `std::vector<Feature>` plus
  `PreparedCellRender`.
- It emits fills from `prep.fills` and line vertices by walking every ring in
  every feature. There is no `clipBox`, no simplified `BuiltPath`, no
  `drawClip_`, no viewport cull, and no option filtering.
- In contrast, `src/cell_builder.cpp` clips to `clipBox`, simplifies by band,
  suppresses out-of-clip points, and builds the CPU painter scene.
- `src/chart_view.cpp:1709` only rebases the already-expanded GPU vectors into a
  common scene. It does not apply the quilt clip or discard hidden geometry.

Impact:

The GPU path can draw far more geometry than the CPU painter path:

- Whole chart cells are emitted rather than the clipped 1.5x keep-area geometry.
- Coarser cell geometry hidden by finer quilt coverage can still be in the GPU
  buffers.
- Depth contours are emitted even when `showDepthContours_` is false.
- Basemap GPU batches are especially risky: `maybeBuildBasemap()` builds a
  clipped/simplified `BuiltCell`, but `appendCellBatches()` ignores that and
  walks the full basemap feature set. At high-resolution GSHHG tiers, this can
  mean drawing much of the world every GPU frame.

This is a prime suspect for the reported 25 percent GPU usage and unchanged pan
smoothness.

Recommendation:

Do not generate GPU batches directly from raw full-cell features for the active
view. Short term, add a clipped/simplified GPU path that mirrors
`instantiateCell()` decisions and honors `showDepthContours_`, `vectorOverlay_`,
and quilt clips. Long term, make prepared render cache store proper LOD line
batches and clipped/cullable cell-level buffers.

## Finding 4: GPU mode redraws S-52 point/text/pattern symbology with QPainter every repaint

Evidence:

- In the CPU painter path, `drawPointSymbology()` is called from
  `renderStatic()` at `src/chart_view.cpp:1928`, so it is baked into
  `staticCache_` and reused during normal idle/pan frames.
- In GPU mode, `paintDynamic()` calls `drawPointSymbology()` at
  `src/chart_view.cpp:1599` on the translucent overlay layer.
- That pass walks loaded cells, builds device clips, draws AP patterns, LC line
  complexes, soundings, symbols, light sectors, and text. Text still draws a
  four-pass halo plus ink for every accepted label.

Impact:

This defeats one of the biggest wins of the previous CPU optimization. During a
pan, the CPU painter backend can blit the cached static chart. The GPU backend
updates the GPU camera, then repaints expensive chart symbology through
QPainter on the overlay. So panning is no longer "GPU transform only"; it is
GPU geometry plus live CPU chart-label painting.

Recommendation:

Until there is a real GPU symbol/text pipeline, keep constant-size chart
symbology cached in GPU mode too. Options:

- Temporarily keep using the CPU `staticCache_` for point/text/pattern
  symbology above the GPU fill/line layer.
- Cache the overlay symbology to a pixmap/texture and translate it during pan,
  just like the CPU static cache.
- Complete the intended retained symbol instances and glyph atlas before
  enabling GPU mode by default.

## Finding 5: GPU repaint scheduling is too eager

Evidence:

- `ChartView::paintEvent()` in GPU mode calls `syncGpuCamera()` and then
  `overlayLayer_->update()` every time the parent is asked to paint
  (`src/chart_view.cpp:1535` and `src/chart_view.cpp:1536`).
- `syncGpuCamera()` calls `GpuChartView::setCamera()` at
  `src/chart_view.cpp:1776`.
- `GpuChartView::setCamera()` always calls `update()` at
  `src/gpu_chart_view.cpp:55`, even if center and zoom are unchanged.
- `GpuChartView::setScene()` and `setRasterLayer()` also call `update()` after
  every scene/raster assignment.

Impact:

Any data-driven repaint now tends to schedule both a GPU frame and an overlay
QPainter frame. If the camera did not change, the GPU frame is redundant.

Recommendation:

Add equality/epsilon guards to `GpuChartView::setCamera()` and only update the
overlay when the camera or dynamic overlay content changed. Track repaint
reasons so AIS bookkeeping does not automatically push a camera update.

## Finding 6: AIS CPA bookkeeping creates idle repaint pressure

Evidence:

- `CpaCalculator` runs every second and also recomputes on every ownship change
  (`src/cpa_calculator.cpp:22` and `src/cpa_calculator.cpp:25`).
- Every recompute loops targets and calls `setRangeMeters()` and `setCpaTcpa()`
  (`src/cpa_calculator.cpp:92`, `src/cpa_calculator.cpp:97`,
  `src/cpa_calculator.cpp:114`, `src/cpa_calculator.cpp:132`).
- `AisTargetStore::setRangeMeters()` and `setCpaTcpa()` emit `targetUpdated`
  unconditionally (`src/ais_target_store.cpp:189` through
  `src/ais_target_store.cpp:206`).
- `MainWindow` maps `targetUpdated` to `view_->requestRepaint()`
  (`src/main_window.cpp:247`).

Impact:

With a valid ownship fix and AIS targets, the app is never truly idle. The
coalescing repaint timer prevents target-count explosions, but it still
guarantees regular chart repaint work. In CPU mode the static cache made that
mostly acceptable. In GPU mode each repaint can trigger live QPainter symbology
plus a GPU frame.

Recommendation:

Make `setRangeMeters()` and `setCpaTcpa()` change-detect before emitting. Use a
tolerance for floating point values, or batch all derived CPA/range updates and
emit one lightweight "derived target data changed" signal only when visible
state actually changes.

## Finding 7: Raster charts are composited on CPU and uploaded as a full texture

Evidence:

- `composeGpuRaster()` creates a full viewport-sized `QImage`, draws raster
  charts into it with QPainter, and passes it to the GPU layer
  (`src/chart_view.cpp:1744` through `src/chart_view.cpp:1768`).
- `GpuChartView::render()` uploads that image with `uploadTexture()` when
  `rasterDirty_` is set (`src/gpu_chart_view.cpp:246` through
  `src/gpu_chart_view.cpp:261`).
- Every raster tile reply marks `gpuRasterDirty_` and invalidates the chart
  (`src/chart_view.cpp:1234` through `src/chart_view.cpp:1247`).

Impact:

Initial raster loading or panning into missing MBTiles can cause repeated
full-viewport CPU recomposites and full texture uploads, one tile reply at a
time. This is not a retained tile renderer. It may be fine as an interim bridge,
but it is not expected to outperform the existing painter cache during tile
churn.

Recommendation:

Short term, coalesce raster tile replies before recomposition. Longer term,
retain raster tiles as individual textures/atlased textures and draw tile quads,
or keep the CPU painter cache path for rasters until the tile renderer exists.

## Finding 8: GPU scene rebuilds and uploads are coarse

Evidence:

- `aaTimer_` marks `gpuSceneDirty_ = true` after every interaction settle in GPU
  mode (`src/chart_view.cpp:177`).
- `rebuildGpuScene()` concatenates all active cell and basemap GPU vectors into
  new vectors and calls `setScene()` (`src/chart_view.cpp:1695` through
  `src/chart_view.cpp:1740`).
- `GpuChartView::render()` recreates immutable vertex buffers and uploads all
  current vertex data whenever `sceneDirty_` is true
  (`src/gpu_chart_view.cpp:205` through `src/gpu_chart_view.cpp:242`).

Impact:

Pan frames may avoid geometry rebuilds, but every settle can still concatenate
and upload the whole active scene. With the current unbounded full-cell/full-
basemap batches, this can create a visible hitch at the end of each gesture.

Recommendation:

Retain per-cell GPU buffers and update only cells entering/leaving the view.
Rebasing for float precision should not require rebuilding and uploading every
cell if the pan stayed within a local origin tolerance.

## Finding 9: Telemetry is still not sufficient for this regression

The architecture plan called for lightweight frame timing telemetry:
visible cells, static cache render ms, paint ms, point/symbol/text counts,
cache hit/miss counts, upload time, and prepared-cache hit rate.

There are useful pieces, such as parsed cache logging in `cell_source.cpp`, but
there is no comprehensive release-build frame telemetry for:

- parent `ChartView` paint count and reason
- GPU child render count
- overlay paint count
- `drawPointSymbology()` time and counts
- GPU vertex counts by base/cell/fill/line
- `rebuildGpuScene()` time
- raster recomposition and texture upload count
- CPA/AIS repaint trigger count

Without those numbers, it is easy to "optimize" by moving work between CPU and
GPU and accidentally making total work larger.

Recommendation:

Before additional renderer work, add a low-noise logging category or debug
overlay with these counters. Measure in `vs2022-release`, with CPU and GPU
backends side by side over the same chart set and same AIS/GPS feed.

## Suggested triage order

1. Force CPU default again. Change the constructor fallback from Auto to Cpu, or
   make `resolveUseGpu()` return false for Auto until the GPU backend passes
   performance gates.
2. Add telemetry listed above and capture one idle minute plus a standard pan
   gesture in both backends.
3. Change-detect AIS CPA/range setters to stop idle repaints when values are
   unchanged or visually equivalent.
4. Stop live QPainter chart symbology in GPU mode. Cache it or complete the GPU
   symbol/text path before using GPU by default.
5. Fix GPU batch generation so it is clipped, simplified, quilt-aware, and
   option-aware. Treat the basemap as the first test case because it is likely
   the largest vertex explosion.
6. Retain per-cell GPU buffers instead of rebuilding/uploading one aggregate
   scene after each settle.
7. Convert raster GPU handling from full-viewport recomposite/upload to retained
   tile textures, or keep rasters on the proven painter cache until then.

## Bottom line

The regression is not evidence that GPU rendering cannot help this application.
It is evidence that the current Stage 7 path is doing the old renderer's work
plus new GPU work, while losing the static-cache advantage for expensive chart
symbology. The GPU path should remain experimental until it draws clipped,
retained, option-aware batches and no longer repaints S-52 text/symbol/pattern
passes through QPainter on every overlay update.
