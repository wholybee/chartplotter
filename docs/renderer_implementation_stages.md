# Renderer implementation stages

A finer-grained, independently-shippable breakdown of the migration plan in
[`renderer_architecture_plan.md`](renderer_architecture_plan.md). Where that
document describes the target architecture and coarse Phases 0–6, this one maps
each step onto the real code seams and defines what "done" means per stage.

Each stage:

- builds on its own (`vs2022-release`) and leaves the app shippable,
- has an explicit exit criterion that can be checked before the next stage,
- changes as few layers as possible.

## Current pipeline (the seams we are moving)

```text
catalog                 ChartCatalog / IChartSource::catalog -> CellRecord
  |
dispatchLoad            chart_view.cpp:1159
  |  chart::loadCellFeatures (GDAL)  ->  std::vector<Feature>     (chart_loader.hpp:49)
  v
FeatureCache            in-memory LRU of parsed cells             (feature_cache.hpp)
  |
dispatchBuild           chart_view.cpp:1226
  |  buildCell -> BuiltCell (QPainterPath fills, lines, BuiltText, symbols via SymAtlas)
  v
paint                   staticCache_ pixmap + dynamic overlays    (chart_view.cpp:1839+)
```

The performance ceiling is that `buildCell` produces immediate-mode Qt value
objects and `paint` rasterizes them into a static pixmap. The stages below move
the expensive work to a prepared, retained representation.

## Dependency graph

```text
0 -> 1 -> 2
     1 -> 3 -> 4 -> 5 -+
               6 ------+-> 7 -> 8
```

Stages 0–2 are pure performance/UX wins on the current renderer. Stage 5 is the
architectural pivot (geometry becomes retained data). Stage 7 is the payoff.

---

## Stage 0 — Telemetry & precise invalidation

**Goal:** make the current renderer measurable and stop needless static
rebuilds, so later stages can be compared honestly. No architecture change.

- Frame-timing struct surfaced in a debug overlay / log: visible cells,
  `staticCache_` render ms, paint ms, point/symbol/text counts, cache hit/miss.
- Audit every `staticDirty_ = true` site (chart_view.cpp:791, 1101, 1140, 1297,
  1915) and tighten so routine pan-settle does not rebuild static pixels.
- Keep the static pixmap small; do not grow the apron.

**Exit:** numbers visible in a release build; pan-settle no longer forces a
static rebuild when quilt topology is unchanged. **Risk:** low.

---

## Stage 1 — Binary parsed-cell cache  *(starting point)*

**Goal:** eliminate cold GDAL parse latency. Benefits the current renderer and
is required by the final architecture (becomes the parsed-product cache level).

- New `src/prepared_chart_cache.{hpp,cpp}`: versioned binary serialize /
  deserialize of `std::vector<Feature>` + `BBox` (the exact type at
  chart_loader.hpp:49, including rings, attrs, name, objClass, scaleMin).
- Cache key: source path + file size/mtime, decoder version, projection
  version, cache-format version. A key mismatch is a miss (and rewrites).
- In `dispatchLoad` (chart_view.cpp:1159): read cache before
  `chart::loadCellFeatures`; write after a successful GDAL parse. The
  `IChartSource::loadCell` plugin path can opt in later.
- Wire cache hit/miss + parse time into Stage 0 telemetry.

**Exit:** restart over a prepared set skips GDAL for visible cells; deleting the
cache directory is safe; changed charts invalidate correctly. **Risk:** low/med.

---

## Stage 2 — "Prepare chart cache" action

**Goal:** OpenCPN-style explicit, persistent preparation.

- Add `Charts -> Prepare chart cache` in `side_menu.cpp`, styled per the menu
  conventions in CLAUDE.md.
- Background scan + parse using `nCPU-1` workers writing the Stage 1 cache;
  progress dialog with pause/cancel; never blocks pan/zoom.

**Exit:** preparing a folder populates the parsed cache for the whole set
offline. **Depends on:** Stage 1.

---

## Stage 3 — Normalized product model behind an adapter

**Goal:** stop S-57 acronyms being the universal internal type, without touching
rendering yet.

- Introduce `ProductId`, `FeatureClassId`, typed `AttributeValue`,
  `GeometryStore`, `ChartFeature`, `ProductFeatureSet` (architecture plan
  Layer 2).
- Wrap the GDAL S-57 reader as `S57ProductDecoder` emitting the normalized model.
- Compatibility adapter `ProductFeatureSet -> std::vector<Feature>` so
  `buildCell` / `SymAtlas` / paint are untouched.
- Test fixtures for feature/attribute identity and projection.

**Exit:** S-57 renders identically through the adapter; model can express
S-101-style ids/associations. **Risk:** medium (mitigated by the adapter).

---

## Stage 4 — Portrayal split + presentation IR

**Goal:** make symbol/style updates a data-package change, not code.

- Split `SymAtlas` (sym_atlas.hpp) into `RenderResourceAtlas`,
  `PortrayalPackage`, `PortrayalEngine`, and the `QPainter` draw helpers.
- Define renderer-neutral `RenderInstruction` IR (architecture plan Layer 4).
- `buildCell` consumes IR via the existing `QPainter` helpers — output must
  match pixel-for-pixel.
- Move day/dusk/night into portrayal resources.

**Exit:** S-52 output visually unchanged; swapping the atlas/package touches no
decoder or render code. **Depends on:** Stage 3. **Risk:** medium-high; gate
with screenshot diffs.

---

## Stage 5 — Prepared render cache (CPU-side retained batches)

**Goal:** precompute the expensive representation; still drawn by `QPainter`.

- Compiler: portrayal IR -> `PreparedCellRender` (architecture plan Layer 5):
  pre-triangulated area fills, line batches (CPU-tessellated joins/caps), symbol
  instances, text candidates, pick index.
- Third cache level, separately versioned and keyed (adds portrayal package
  id/version) from the parsed cache (Stage 1).
- A `QPainter` path that renders batches verifies correctness before any GL.

**Exit:** visible cells load prepared batches without recomputing portrayal;
portrayal-package change rebuilds only this cache. **Depends on:** 3–4. This is
the pivot: geometry becomes retained data, not Qt paths.

---

## Stage 6 — `IChartRenderer` seam in ChartView

**Goal:** decouple the shell from the backend so two renderers can coexist.

- Extract `IChartRenderer` (architecture plan Layer 7); refactor current code
  into `PainterChartRenderer`.
- `ChartView` keeps camera/input/overlays/settings; overlays (AIS/route/ownship)
  stay as separate dynamic passes via a stable API.

**Exit:** app runs unchanged on `PainterChartRenderer`; overlays don't know the
backend. **Depends on:** can proceed in parallel with Stage 5.

---

## Stage 7 — Retained OpenGL backend

**Goal:** transform-only pan/zoom.

- `PreparedGpuChartRenderer` (QOpenGLWidget) behind `IChartRenderer`: upload
  Stage 5 batches to GPU buffers; camera uniform pan/zoom; display-priority draw
  order; instanced symbols from the atlas texture; screen-space text declutter +
  glyph atlas; CPU pick index for hit-testing; DPR + palette swap.
- Frame loop per architecture plan Layer 6; no giant static pixmap.

**Exit:** pan/zoom of loaded cells rebuilds no geometry; CPU is mostly culling +
labels; side-by-side perf vs OpenCPN and the painter backend. **Depends on:**
5–6. **Risk:** highest; keep `PainterChartRenderer` as fallback.

---

## Stage 8 — S-101 as a product + portrayal package

**Goal:** prove the architecture: add a product without renderer changes.

- `S101ProductDecoder` + feature-catalogue loader -> normalized model (Stage 3);
  S-101 portrayal catalogue -> `PortrayalPackage` (Stage 4); parsed + prepared
  caches; sample/test data.

**Exit:** S-101 cells catalog, prepare, and render through the same retained
renderer as S-57; portrayal changes need no renderer edits unless the IR lacks
an instruction class.
