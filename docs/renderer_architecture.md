# Chart renderer architecture (current)

This document describes the chart pipeline **as it exists today**, after renderer
stages 1–4 (see [`renderer_implementation_stages.md`](renderer_implementation_stages.md)
for the staged plan and [`renderer_architecture_plan.md`](renderer_architecture_plan.md)
for the long-term target). It is the reference for anyone adding a **new chart
product** (S-101, S-102, another CM-style format) or a **new symbol / presentation
library** (an updated S-52 edition, an S-101 portrayal catalogue).

Read this first; it tells you which layer your change belongs in and which seam
to implement so the rest of the pipeline is untouched.

## The core idea: layers with one job each

A chart cell flows through a chain of layers. Each owns exactly one job, and the
boundaries between them are typed value objects, not calls into renderer
internals. Changing the chart format replaces the *decoder* layer; changing the
symbology replaces the *portrayal* layer; neither touches the other, nor the
drawing code.

```text
            chart files on disk
                    │
        ┌───────────┴────────────┐  LAYER 1  product decoding
        │ IChartProductDecoder    │  src/product_decoder.*
        │  └ S57ProductDecoder    │  (legacy: IChartSource plugins, src/chart_source.hpp)
        └───────────┬────────────┘
                    │  ProductFeatureSet  (normalized, product-neutral)
        ┌───────────┴────────────┐  LAYER 2  normalized feature model
        │ product_model.hpp       │  ChartFeature, GeometryStore, FeatureClassId
        └───────────┬────────────┘
                    │
        ┌───────────┴────────────┐  LAYER 3  caching
        │ prepared_chart_cache.*  │  on-disk parsed-cell cache (binary)
        │ FeatureCache (in-RAM)   │  src/feature_cache.hpp
        └───────────┬────────────┘
                    │  std::vector<Feature>   (legacy render model, via product_adapter)
        ┌───────────┴────────────┐  LAYER 4  portrayal (symbology)
        │ PortrayalEngine         │  src/portrayal_engine.*  feature → SymHit
        │  + PortrayalPackage     │  rule/colour data (from symbols.bin)
        │  + RenderResourceAtlas  │  src/render_resource_atlas.*  pixmap + draw helpers
        └───────────┬────────────┘
                    │  SymHit  (renderer-neutral presentation IR, src/portrayal_ir.hpp)
        ┌───────────┴────────────┐  LAYER 5  scene build  (proto; full retained cache = Stage 5)
        │ buildCell()             │  src/chart_view.cpp → BuiltCell
        └───────────┬────────────┘
                    │  BuiltCell  (QPainterPath / symbol / text / sector batches)
        ┌───────────┴────────────┐  LAYER 6  rendering  (QPainter today; GPU = Stage 7)
        │ ChartView::paintEvent   │  static pixmap cache + dynamic overlays
        └───────────┬────────────┘
                    │
              ChartView shell        LAYER 7  camera, input, overlays, settings
```

### What is realized vs. planned

| Layer | Status | Notes |
|------|--------|-------|
| 1 Product decoders | **realized for S-57** | `IChartProductDecoder`; plugin `IChartSource` also still supported |
| 2 Normalized model | **realized** | `ProductFeatureSet`; S-57 fills it, others can too |
| 3 Caching | **realized** | binary parsed-cell cache + RAM LRU + "Prepare Chart Cache" UI |
| 4 Portrayal | **realized** | package / engine / resource atlas split; emits `SymHit` IR |
| 5 Scene build | **proto** | `buildCell()` is the current compiler; pre-triangulated retained cache is Stage 5 |
| 6 Rendering | **QPainter** | static-pixmap cache; retained GPU backend is Stage 7 |
| 7 Shell | realized | `ChartView` |

A key transitional detail: layers 1–2 currently feed the legacy
`std::vector<Feature>` (via `product_adapter`) because the build/paint path
(layers 5–6) still consumes it. The normalized model is in place; the renderer
has not yet been moved onto it. See *Transitional state* below.

---

## Layer 1 — Product decoders

**Job:** read one chart product and emit normalized feature records. No symbol
resolution, no GPU objects.

**Files:** [`src/product_decoder.hpp`](../src/product_decoder.hpp) /
[`.cpp`](../src/product_decoder.cpp), and the older plugin seam
[`src/chart_source.hpp`](../src/chart_source.hpp).

There are **two** decoder seams today:

### a. `IChartProductDecoder` (the forward path — use this for S-101)

```cpp
class IChartProductDecoder {
    virtual ProductId productId() const = 0;
    virtual bool loadCell(const QString& cellId, ProductFeatureSet& out,
                          QString& err) = 0;
};
```

`S57ProductDecoder` is the only implementation today. It wraps the GDAL S-57
reader (`chart::loadCellFeatures`) and converts its output into the normalized
`ProductFeatureSet` via `product_adapter::fromLegacyFeatures`. It is invoked on
the built-in (non-plugin) load path in `ChartView::dispatchLoad`
([`src/chart_view.cpp`](../src/chart_view.cpp)).

### b. `IChartSource` (the older plugin seam — e.g. CM93)

```cpp
class IChartSource {
    virtual bool canHandle(const QString& root) const = 0;
    virtual bool catalog(... std::vector<ChartSourceCell>& out ...) = 0;
    virtual bool loadCell(const QString& cellId,
                          std::vector<Feature>& out, BBox&, QString&) = 0;
};
```

Registered via `ChartSourceRegistry` (held by `MainWindow`, fed through
`ICoreApi::registerChartSource`). A source that `canHandle()` a chart folder
supplies its cells directly as legacy `Feature`s, translating its native object
dictionary onto **S-57 object-class acronyms** so the existing portrayal engine
resolves them unchanged. Good enough for CM93; the wrong abstraction for S-101
(see *Adding a chart format* for why).

---

## Layer 2 — Normalized feature model

**Job:** represent any product's features in one product-neutral shape.

**File:** [`src/product_model.hpp`](../src/product_model.hpp).

```cpp
struct ProductId      { QString authority, product, edition; };   // "IHO","S-57",…
struct FeatureClassId { ProductId product; QString code; };       // "S57:DEPARE","S101:DepthArea"
struct AttributeValue { QString id; QVariant value; };            // typed, namespaced
struct GeometryRef    { GeometryKind kind; quint32 firstRing, ringCount; };  // slice into pool
struct GeometryStore  { std::vector<std::vector<Pt>> rings; };    // cell-local coord pool
struct ChartFeature {
    quint64 stableId;
    FeatureClassId classId;
    GeometryRef geometry;
    std::vector<AttributeValue> attrs;
    std::vector<AssociationRef> associations;   // S-100 info/spatial associations
    int scaleMin, displayPriorityHint;
    BBox bbox;
    LegacyRenderHints legacy;                   // TRANSITIONAL, see below
};
struct ProductFeatureSet { ProductCellInfo cell; GeometryStore geometry;
                           std::vector<ChartFeature> features; };
```

Design points that matter when adding a product:

- **Identity is namespace-qualified**, never a bare S-57 acronym. `FeatureClassId
  { product=S-101, code="S101:DepthArea" }` and an S-57 `DEPARE` are distinct
  identities that the portrayal layer can target separately.
- **Attributes are typed and namespaced** (`QVariant` + `"S101:..."` id), not the
  S-57 `(acronym,string)` pairs the legacy `Feature` used.
- **Geometry lives in a per-cell pool** (`GeometryStore`) referenced by slice, so
  rings are not copied per feature and shared S-57 edges / S-101 spatial
  primitives can converge here later.
- **`LegacyRenderHints` is transitional.** Because layers 5–6 still consume the
  legacy `Feature` (with its render-oriented `FeatureKind`, `zorder`, `depth`),
  `ChartFeature` carries those values so the adapter can reconstruct a `Feature`
  losslessly. They disappear once portrayal derives kind/priority from
  `FeatureClassId` + attributes.

**The adapter:** [`src/product_adapter.hpp`](../src/product_adapter.hpp) /
[`.cpp`](../src/product_adapter.cpp) — pure, GDAL-free, bidirectional:

```cpp
ProductFeatureSet fromLegacyFeatures(std::vector<Feature>, const QString& cellId);
std::vector<Feature> toLegacyFeatures(ProductFeatureSet, BBox& bbox);
```

The round trip is lossless by construction and is covered by
[`tools/test_product_model.cpp`](../tools/test_product_model.cpp) (CTest
`product_model_round_trip`).

---

## Layer 3 — Caching

**Job:** make cold cell loads fast and preparable up front.

**Files:** [`src/prepared_chart_cache.hpp`](../src/prepared_chart_cache.hpp) /
[`.cpp`](../src/prepared_chart_cache.cpp),
[`src/feature_cache.hpp`](../src/feature_cache.hpp),
[`src/prepare_cache_dialog.*`](../src/prepare_cache_dialog.cpp).

Two cache levels exist:

1. **On-disk parsed-cell cache** (`prepared_cache`): a versioned binary of the
   parsed `std::vector<Feature>` + `BBox`, written under
   `QStandardPaths::CacheLocation/parsed-cells/<sha1>.pcell`. Keyed by source
   path + size + mtime and by **decoder / projection / format versions** — any
   mismatch is a miss. `ChartView::dispatchLoad` reads it before GDAL and writes
   it after a successful parse. The **Settings → Charts → Prepare Chart Cache**
   action (`PrepareCacheDialog`) builds it for a whole set on a background pool.

2. **In-RAM LRU** (`FeatureCache`): keeps recently/visible parsed cells resident
   so back-and-forth panning never reloads.

> **Bump the cache version when you change parsing.** If you alter what a decoder
> emits (or projection math), bump `kDecoderVersion` / `kProjVersion` /
> `kFormatVersion` in `prepared_chart_cache.cpp` so stale `.pcell` files are
> rejected rather than trusted.

This level currently caches only the **built-in S-57 path** (real file-path cell
ids). Plugin `IChartSource` cells (opaque ids) and other products are not cached
yet.

---

## Layer 4 — Portrayal (symbology)

**Job:** turn a feature into a renderer-neutral description of what to draw.
This is where S-52 lives, and where an S-101 presentation library would plug in.

The old monolithic `SymAtlas` was split into four pieces:

| Piece | File | Owns |
|-------|------|------|
| Presentation IR | [`portrayal_ir.hpp`](../src/portrayal_ir.hpp) | `SymHit` and its parts — the neutral contract |
| Binary format | [`portrayal_binary.hpp`](../src/portrayal_binary.hpp) | on-disk record structs (`symbols.bin`) |
| Portrayal package | [`portrayal_engine.hpp`](../src/portrayal_engine.hpp) (`PortrayalPackage`) | lookup tables, conditions, colours, instruction blob |
| Portrayal engine | [`portrayal_engine.*`](../src/portrayal_engine.cpp) (`PortrayalEngine`) | scores a feature, executes instructions + CS procedures → `SymHit` |
| Resource atlas | [`render_resource_atlas.*`](../src/render_resource_atlas.cpp) | atlas pixmap, LC/AP motifs, `QPainter` draw helpers, name→index lookups |
| Facade | [`sym_atlas.*`](../src/sym_atlas.cpp) (`SymAtlas`) | loads `symbols.bin`, wires the three, preserves the old public API |

### The IR (`SymHit`) — the contract

```cpp
struct SymHit {
    std::vector<SymStamp>  symbols;   // SY() symbol stamps (atlas index + rotation)
    bool hasLine; SymLineStyle line;  // LS() simple line / boundary
    bool hasFill; SymFillStyle fill;  // AC() area wash
    int  lcIndex;                     // LC() complex line def, or -1
    int  apIndex;                     // AP() area pattern def, or -1
    std::vector<SymText>   texts;     // TX()/TE() labels
    std::vector<SymSector> sectors;   // CS(LIGHTS05) light arcs
};
```

`SymHit` names no `QPainter` type. It is the boundary between portrayal and
rendering: the scene builder (`buildCell`) consumes it today; the Stage-5 retained
batch compiler will consume the same thing.

### How evaluation works

`PortrayalEngine::evaluate(objClass, geom, attrs)`:
1. Looks up the LUP range for `(object class | geometry)` in the package.
2. Best-match scores each LUP's attribute conditions against the feature's attrs.
3. Executes the chosen LUP's S-52 instruction string: `SY/LS/AC/LC/AP/TX/TE`,
   plus `CS(...)` conditional-symbology procedures (LIGHTS05, OBSTRN04, WRECKS02,
   RESARE/RESTRN, TOPMARnn, SYMINS01) implemented in C++ in `runCS`.
4. Symbol/LC/AP names resolve to indices via the `RenderResourceAtlas`; colour
   tokens resolve via the `PortrayalPackage`.

### Where the data comes from

`symbols.bin` is **baked at build time** by
[`tools/gen_symbols.cpp`](../tools/gen_symbols.cpp) from
`data/chartsymbols.xml` (the S-52 presentation library) plus the
`rastersymbols-*.png` sprite sheet. `SymAtlas::load()` reads it, splits the
sections, and hands rules to `PortrayalPackage` and resources to
`RenderResourceAtlas`. See [`symbols.md`](symbols.md) for the bake.

> Day/dusk/night: only the `DAY_BRIGHT` palette is baked today. The colour table
> lives in `PortrayalPackage`, which is where additional palettes belong — adding
> them is a package change, not a renderer change.

---

## Layers 5–7 — Scene build, rendering, shell (current)

**Scene build** is `buildCell()` in [`src/chart_view.cpp`](../src/chart_view.cpp):
it clips/simplifies geometry, calls `SymAtlas::symbolForFeature` (→ `SymHit`) for
symbol-bearing features, and produces a `BuiltCell` of `QPainterPath` fills,
symbol stamps, text, light sectors, and soundings. Runs on a worker thread.

**Rendering** is `ChartView::paintEvent`: it composites a static chart pixmap
cache and draws the `BuiltCell` batches through the camera transform, then the
dynamic overlays (AIS, routes, ownship) in separate passes via
`RenderResourceAtlas::draw / drawLineComplex / fillAreaPattern`.

**Shell** is `ChartView` itself: camera/zoom, touch input, follow-ownship,
settings, overlay registration, picking.

These three are the parts the long-term plan still reshapes (retained GPU batches
in Stages 5–7). They are described here only so you can see where the IR lands.

---

## Recipe: adding a new chart format (e.g. S-101)

The model and decoder interface for this exist; the dispatch wiring does not yet.
A complete S-101 addition has these parts.

1. **Implement a decoder.** New `S101ProductDecoder : IChartProductDecoder`
   ([`src/product_decoder.hpp`](../src/product_decoder.hpp)). Parse the S-100
   dataset (feature + portrayal catalogues, GML/ISO 8211 as applicable) and emit
   a `ProductFeatureSet`:
   - `FeatureClassId.code = "S101:<FeatureClass>"`, `product = {IHO, S-101, ed}`.
   - Native S-101 attributes as `AttributeValue` (`"S101:<attr>"` + typed value).
   - Geometry into the `GeometryStore`; set `GeometryRef` slices.
   - Populate `associations`, `scaleMin`, `bbox`. Synthesize a `stableId`.
   - **Do not** translate to S-57 acronyms. That is the whole point of layer 2.

2. **Select the decoder.** Today `dispatchLoad` hard-wires `S57ProductDecoder`
   for the built-in path. Add a small decoder registry (mirroring
   `ChartSourceRegistry`) or extend `dispatchLoad` to pick a decoder by product —
   typically detected during catalog/scan. The catalog path
   ([`src/chart_catalog.*`](../src/chart_catalog.hpp)) also needs an S-101 branch
   to enumerate cells + coverage.

3. **Bridge to the renderer.** Because layers 5–6 still consume legacy `Feature`,
   you have two choices:
   - *Short term:* extend `product_adapter::toLegacyFeatures` to map S-101
     classes onto the existing `FeatureKind`s and populate `LegacyRenderHints`.
     Workable for a constrained subset, but it re-introduces the very coupling
     layer 2 removes, and `FeatureKind` cannot express every S-101 class.
   - *Right way (preferred):* do this as part of Stage 5 — have the scene builder
     consume `ChartFeature` + the portrayal IR directly, so no back-adapter is
     needed. New products then never touch `Feature`.

4. **Cache + prepare.** Give the decoder a version stamp and extend the cache key
   so S-101 cells participate in the parsed-cell cache and the Prepare action.

5. **Portray it** — see the next recipe; S-101 needs its own portrayal package.

**Quicker, lower-fidelity alternative:** if you only need a CM93-style "make it
show up" result and the product maps cleanly onto S-57 semantics, implement the
older `IChartSource` plugin seam instead (translate to S-57 acronyms, emit
`Feature` directly). This reuses the entire existing portrayal + render path with
zero core changes — but it is explicitly *not* the path for a faithful S-101.

---

## Recipe: adding a symbol / presentation library (e.g. S-101 portrayal, new S-52)

The portrayal layer is designed to be replaceable data + a thin evaluator. The
boundary you must hit is: **produce a `SymHit`** from a feature, and make sure
every symbol/LC/AP name you reference exists in a `RenderResourceAtlas`.

For an **updated S-52** (new symbols, recoloured, new lookups): edit
`data/chartsymbols.xml` (and the sprite sheet), rebuild — `gen_symbols` re-bakes
`symbols.bin` and the `PortrayalPackage` / `RenderResourceAtlas` pick it up. No
C++ changes unless you add a new instruction verb or CS procedure (those live in
`PortrayalEngine::execInstruction` / `runCS`).

For an **S-101 / S-100 portrayal catalogue** (a genuinely different presentation
system):

1. **New package loader.** S-100 portrayal is XML/SVG rule sets, not the S-52
   `symbols.bin` layout. Add a loader that builds a `PortrayalPackage`-equivalent
   from it (lookup/rule tables keyed by **`FeatureClassId`**, not S-57 acronym +
   `SymGeom`). The current `PortrayalPackage` keys on `(objClass|geom)`; generalize
   that to `FeatureClassId.code` (or add an S-101 package type with its own key).

2. **New resources.** Build a `RenderResourceAtlas` (or an analogous resource
   provider) from the S-101 symbol set — the engine resolves symbol/LC/AP *names*
   through it, so as long as the names the rules reference resolve, the draw path
   is unchanged.

3. **Evaluate to the same IR.** Your engine (or an extended `PortrayalEngine`)
   must emit `SymHit`. If S-101 portrayal needs an instruction the IR can't
   express, add a field/kind to `portrayal_ir.hpp` and teach the scene builder to
   honour it — that is the one place a portrayal extension legitimately touches
   the renderer.

4. **Select by product.** Pick the portrayal package by the feature's `ProductId`
   so S-57 cells use S-52 and S-101 cells use S-101 portrayal in the same view.

Because portrayal evaluation is independent of decoding and of drawing, swapping
or adding a package touches neither the chart decoders nor the build/paint code —
that separation is the deliverable of stage 4.

---

## Transitional state and gotchas

- **The renderer is not on the normalized model yet.** Layers 1–2 produce
  `ProductFeatureSet`, but `product_adapter::toLegacyFeatures` immediately
  converts back to `std::vector<Feature>` for layers 3–6. So today S-57 makes a
  `Feature → ProductFeatureSet → Feature` round trip. This is intentional and
  cheap (geometry is moved, not copied), and it disappears in Stage 5.
- **Only S-57 fills the model.** `FeatureClassId.code` is populated only for
  symbol-bearing kinds because the wrapped `loadCellFeatures` discards the object
  acronym for depth/land/coastline features. A native S-101 decoder would fill it
  for all features.
- **No decoder registry yet.** `dispatchLoad` hard-wires `S57ProductDecoder` for
  the built-in path; non-S-57 products currently must come in via the
  `IChartSource` plugin seam until the registry is added.
- **Portrayal keys on S-57 acronym + geometry.** Generalizing to `FeatureClassId`
  is the first step for S-101 portrayal.
- **Rendering is QPainter + a static pixmap cache**, not retained GPU geometry.
  This bounds pan/zoom performance and is what Stages 5–7 address.

---

## File reference

| File | Layer | Role |
|------|-------|------|
| `src/chart_source.hpp` | 1 | `IChartSource` plugin seam + registry (legacy, S-57-acronym) |
| `src/product_decoder.{hpp,cpp}` | 1 | `IChartProductDecoder` + `S57ProductDecoder` |
| `src/chart_loader.{hpp,cpp}` | 1 | GDAL S-57 reader (`loadCellFeatures`, coverage, basemap) |
| `src/chart_catalog.{hpp,cpp}` | 1 | cell enumeration + coverage scan → `CellRecord` |
| `src/product_model.hpp` | 2 | normalized model types |
| `src/product_adapter.{hpp,cpp}` | 2 | legacy `Feature` ↔ `ProductFeatureSet` |
| `src/prepared_chart_cache.{hpp,cpp}` | 3 | on-disk parsed-cell binary cache |
| `src/feature_cache.hpp` | 3 | in-RAM LRU of parsed cells |
| `src/prepare_cache_dialog.{hpp,cpp}` | 3 | "Prepare Chart Cache" UI |
| `src/portrayal_ir.hpp` | 4 | `SymHit` presentation IR |
| `src/portrayal_binary.hpp` | 4 | `symbols.bin` record structs |
| `src/portrayal_engine.{hpp,cpp}` | 4 | `PortrayalPackage` + `PortrayalEngine` |
| `src/render_resource_atlas.{hpp,cpp}` | 4 | resources + `QPainter` draw helpers |
| `src/sym_atlas.{hpp,cpp}` | 4 | facade: loads `symbols.bin`, wires the three |
| `tools/gen_symbols.cpp` | 4 | bakes `data/chartsymbols.xml` → `symbols.bin` |
| `src/chart_view.{hpp,cpp}` | 5–7 | `buildCell` scene compile, paint, camera, input |
| `tools/test_product_model.cpp` | test | model round-trip (CTest `product_model_round_trip`) |

## See also

- [`renderer_architecture_plan.md`](renderer_architecture_plan.md) — long-term target and rationale.
- [`renderer_implementation_stages.md`](renderer_implementation_stages.md) — staged plan; stages 1–4 done.
- [`symbols.md`](symbols.md) — the S-52 symbol bake (`gen_symbols`, `symbols.bin`).
- [`plugin_api.md`](plugin_api.md) — plugin host API, including `registerChartSource`.
