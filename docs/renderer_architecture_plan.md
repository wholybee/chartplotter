# High-performance chart renderer architecture plan

This document proposes the long-term renderer architecture for matching
OpenCPN-class pan/zoom performance while keeping the application open to new chart
formats and new presentation systems, especially S-101.

The current renderer should be treated as the legacy / compatibility backend. It
has useful pieces worth keeping (catalog scan, coverage quilting, feature cache,
symbol atlas work, overlays, settings, plugins), but the renderer core is still an
immediate-mode `QPainterPath` pipeline. That is the wrong center of gravity for
OpenCPN-level performance.

The target architecture is:

```text
              chart files / chart services
                         |
                 Product decoders
       S-57 ENC | S-101 | CM93 | MBTiles | future S-1xx
                         |
              normalized product feature model
                         |
                  prepared chart cache
       parsed features + coverage + associations + LOD geometry
                         |
                 portrayal engines/packages
            S-52, S-52 Annex A:100, S-101, app overlays
                         |
                  render scene compiler
       triangles, line batches, symbol instances, text candidates
                         |
                    retained renderer
             GPU batches + screen-space label/declutter pass
                         |
                    ChartView shell
       camera, input, overlays, settings, route/AIS/ownship
```

The important split is not "S-57 vs S-101". It is:

- product decoding
- normalized chart feature model
- portrayal rule evaluation
- prepared render batches
- retained GPU drawing

Each layer owns one job. Changing symbols or adding a chart product should replace
one layer, not force edits through the full renderer.

## Why the current path will not reach OpenCPN performance

The current renderer's fundamental unit is a `BuiltCell` full of Qt value objects:
`QPainterPath`, `QColor`, `QPixmap` symbols, and device-space text/symbol passes.
Fixes 1-3 improved the symptoms by coalescing repaints and caching a static
pixmap, but the performance ceiling remains low:

- Any static-cache rebuild still walks and rasterizes the full visible chart with
  `QPainter`.
- Larger pixmap aprons multiply cache rebuild cost by area.
- `QPainterPath` area fills are CPU raster work; they are not retained geometry.
- Symbols and text are still per-frame/per-cache-render immediate draw calls.
- Geometry is rebuilt as presentation-ready Qt paths instead of retained render
  buffers.
- The current `IChartSource` seam requires non-S-57 products to translate into
  S-57 object acronyms and S-52-friendly attributes. That is serviceable for CM93,
  but it is the wrong abstraction for S-101 and future S-1xx products.

OpenCPN is fast because its expensive work is prepared, retained, and cached. It
does not ask the CPU painter to rebuild and rasterize every navigational feature
on every view update.

## Current standards context

IHO's current standards page lists:

- S-101 ENC Product Specification Edition 2.0.0, December 2024.
- S-100 Universal Hydrographic Data Model Edition 5.2.1, December 2025.
- S-98 S-100 ECDIS and Interoperability Specification Edition 2.0.0, October
  2025.
- S-52 Annex A:100, the S-100 ECDIS Presentation Library for S-57 ENC, Edition
  5.0.0, October 2025.
- S-65 Annex B and Annex C conversion guidance between S-57 and S-101.

The implication: S-101 should not be treated as "S-57 with different symbol
names". It is an S-100 product with its own feature catalogue, portrayal
catalogue, dataset/update/security rules, and future interoperability concerns.
The renderer should be product-model neutral.

References:

- IHO Standards and Specifications: https://iho.int/en/standards-and-specifications
- IHO S-100 Universal Hydrographic Data Model page: https://iho.int/en/s-100-universal-hydrographic-data-model
- IHO Geospatial Information Registry: https://registry.iho.int/

## Design goals

1. **Pan and zoom must be transform-only for already-loaded chart content.**
   Existing visible cells should stay resident as GPU buffers. A pan changes the
   camera matrix. It should not trigger CPU path rasterization.

2. **Preparation must be explicit and persistent.**
   The app should have an OpenCPN-style "Prepare charts" step that builds an
   on-disk cache. Cold startup should read prepared binaries, not parse ISO 8211,
   S-100, XML, GML, or SVG rule sets on the navigation path.

3. **Chart products are plugins or modules, not renderer branches.**
   S-57, S-101, CM93, raster MBTiles, and future products decode into a normalized
   feature model. The renderer never knows how a chart file was encoded.

4. **Portrayal is a replaceable package.**
   S-52, S-52 Annex A:100, and S-101 portrayal should all compile into a common
   presentation IR. Updating a symbol set or portrayal catalogue should not
   require changes to cell loading, GPU buffers, AIS overlays, or panning code.

5. **Retained rendering is the primary backend.**
   The long-term renderer should render retained batches: triangles, line
   segments, symbol instances, and text candidates. `QPainter` can remain for
   fallback/debug, not the performance path.

6. **Dynamic overlays stay separate.**
   Ownship, AIS, CPA, routes, and UI overlays draw in their own passes. They
   should not invalidate static chart batches.

7. **Non-goal: ECDIS certification.**
   The architecture should follow the same data/portrayal separation as S-100
   systems, but this is not a certified ECDIS project. Avoid claiming compliance
   unless the full validation/security/display requirements are intentionally met.

## Layer 1: Product decoders

### Responsibility

A product decoder reads a chart product and emits normalized feature records plus
coverage metadata. It does not resolve final symbols and it does not create GPU
objects.

Examples:

- `S57ProductDecoder`
- `S101ProductDecoder`
- `Cm93ProductDecoder`
- `MbtilesRasterProductDecoder`
- future S-102/S-104/S-111 style products

### Interface sketch

```cpp
struct ProductId {
    QString authority;     // "IHO", "CMAP", app-private, etc.
    QString product;       // "S-57", "S-101", "CM93", "S-102"
    QString edition;       // product-spec edition, if known
};

struct ProductCellInfo {
    QString cellId;        // opaque stable identity
    ProductId product;
    int scaleBand;         // normalized scale tier for selection/quilt
    double nativeScale = 0.0;
    BBox bbox;
    std::vector<std::vector<Pt>> coverage;
    QString edition;
    QString updateNumber;
    QFileInfo sourceFile;
};

class IChartProductDecoder {
public:
    virtual ~IChartProductDecoder() = default;
    virtual ProductId productId() const = 0;
    virtual bool canHandle(const QString& root) const = 0;
    virtual bool catalog(const QString& root,
                         std::vector<ProductCellInfo>& out,
                         ProgressSink& progress,
                         QString& err) = 0;
    virtual bool loadCellRaw(const ProductCellInfo& cell,
                             ProductFeatureSet& out,
                             QString& err) = 0;
};
```

### S-57 path

The current GDAL S-57 path can be wrapped behind this interface. It should stop
being the model for all products. It becomes one decoder that emits normalized
features.

### S-101 path

The S-101 decoder should emit native S-101 feature identifiers and attributes,
not translate everything into S-57 acronyms. It needs a feature catalogue loader
and a dataset/update/security path appropriate to S-100 products.

Early implementation can support a constrained S-101 subset, but the model should
already include:

- feature catalogue namespace and version
- information associations / spatial associations
- feature identifiers stable enough for updates and deduplication
- geometry primitives with shared coordinate arrays where possible
- quality / scale / metadata fields needed by portrayal and declutter
- coverage and no-coverage metadata

## Layer 2: Normalized feature model

The current `Feature` type is convenient but too S-57-shaped:

```cpp
std::string objClass;
std::vector<std::pair<std::string, std::string>> attrs;
FeatureKind kind;
```

For S-101, use namespace-qualified feature and attribute identifiers:

```cpp
struct FeatureClassId {
    ProductId product;
    QString code;          // e.g. "S57:BOYLAT" or "S101:BeaconCardinal"
};

struct AttributeValue {
    QString id;            // namespace-qualified attribute id
    QVariant value;        // enum, int, double, string, list, date, etc.
};

struct ChartFeature {
    quint64 stableId;
    FeatureClassId classId;
    GeometryRef geometry;
    std::vector<AttributeValue> attrs;
    std::vector<AssociationRef> associations;
    int scaleMin = 0;
    int displayPriorityHint = 0;
    BBox bbox;
};

struct ProductFeatureSet {
    ProductCellInfo cell;
    GeometryStore geometry;
    std::vector<ChartFeature> features;
};
```

### GeometryStore

Do not copy rings into every feature if the product has shared nodes/edges. Use a
cell-local geometry store:

- coordinate arrays
- edge arrays
- ring descriptors
- line descriptors
- point descriptors
- optional triangulation products

This is where S-57 edge/node topology and S-101 spatial primitives can converge.

## Layer 3: Prepared chart cache

This is the first big win and should be implemented before the retained GPU
renderer is complete.

### Cache levels

Use three cache levels, each separately versioned:

1. **Catalog cache**
   - cell identity, product id, source file metadata, bbox, coverage polygons,
     native scale, edition/update metadata
   - current JSON cache can evolve or be replaced by binary

2. **Parsed product cache**
   - decoded and projected features
   - updates merged
   - feature catalogue ids resolved
   - geometry store serialized
   - no final portrayal baked in

3. **Prepared render cache**
    - portrayal-resolved render commands
    - fill style metadata; avoid raw full-cell triangulation in this layer
    - line batches / line LODs
    - symbol instance data
    - text candidates
    - display priority and viewing group tags

The parsed product cache survives portrayal changes. The prepared render cache is
invalidated when the portrayal package, style settings, or renderer format
version changes.

### Cache key

Each cached artifact should be keyed by:

- product id and product specification edition
- source path or chart identity
- source file size / mtime / checksum where feasible
- edition and update number
- decoder version
- projection version
- feature catalogue version
- portrayal package id/version (for prepared render cache only)
- render-cache format version

### Prepare all charts

Add a menu action:

```text
Charts -> Prepare chart cache
```

Behavior:

- scan selected chart folders
- build catalog cache
- decode and cache parsed product cells
- compile render cache for likely display modes
- use `nCPU - 1` workers
- show progress with cancel/pause
- safe to delete cache directory
- never block pan/zoom UI

This is the closest equivalent to OpenCPN's "Prepare all ENC charts".

## Layer 4: Portrayal packages

### Requirement

Portrayal must be data-driven and replaceable.

The current `SymAtlas` combines:

- S-52 lookup matching
- instruction execution
- runtime drawing helpers
- `QPainter`-specific symbol rendering

That was a good pragmatic step, but it should be split for the next renderer.

### Proposed split

```text
PortrayalPackage
  - metadata: product compatibility, edition, style sets
  - feature/attribute ids required
  - lookup/rule bytecode or compiled rule tables
  - symbol resources
  - line/pattern resources
  - text formatting rules

PortrayalEngine
  - evaluates package rules against ChartFeature
  - emits RenderInstruction IR

RenderResourceAtlas
  - GPU texture atlas
  - vector symbol tessellation cache
  - line/pattern GPU resources
```

### Presentation IR

Portrayal should produce renderer-neutral instructions:

```cpp
enum class RenderInstructionKind {
    AreaFill,
    AreaPattern,
    LineStroke,
    LinePattern,
    Symbol,
    Text,
    Conditional,
};

struct RenderInstruction {
    RenderInstructionKind kind;
    int displayPriority;
    int drawingGroup;
    int viewingGroup;
    int radarPriority;
    StyleRef style;
    GeometryRef geometry;
    TextSpec text;
    SymbolSpec symbol;
    int scaleMin = 0;
};
```

The GPU renderer consumes the IR. A debug `QPainter` renderer can consume the
same IR. That keeps portrayal independent from rendering.

### S-52 and S-101 coexistence

The architecture should support:

- S-52 portrayal for S-57 ENC
- S-52 Annex A:100 / S-100 Presentation Library for S-57 in S-100 ECDIS context
- S-101 portrayal for S-101 ENC
- future product-specific portrayal for S-102, S-104, S-111, etc.

Do not encode "S-52 object class acronym" as the universal symbol key. Use
namespace-qualified feature ids and portrayal package compatibility rules.

## Layer 5: Render scene compiler

The render scene compiler converts portrayal IR into retained batches.

### Output batches

```cpp
struct PreparedCellRender {
    ProductCellInfo cell;
    CoverageMesh coverage;
    std::vector<AreaBatch> areaBatches;
    std::vector<LineBatch> lineBatches;
    std::vector<SymbolInstance> symbols;
    std::vector<TextCandidate> text;
    std::vector<PickRecord> pickIndex;
    BBox sceneBounds;
};
```

### Area fills

Do not pre-triangulate raw, full-cell polygons in the view-independent prepared
render cache. Profiles showed `geomtess::triangulate` dominating worker-thread
CPU when this layer tessellated unclipped ENC and basemap geometry. Instead,
build GPU fill triangles from the instantiated `BuiltCell` geometry after the
view clip and simplification steps have reduced the vertex budget.

Store:

- fill style id
- display priority
- feature id / pick id

The current ear-clipping tessellator is acceptable only after clipping and
simplification. Long term use a robust tessellator that handles holes and
complex polygons deterministically.

### Lines

Prepare line geometry into vertex buffers. For the first retained renderer,
CPU-tessellate line joins/caps into triangles instead of relying on wide GL
lines.

Store multiple LODs where appropriate:

- full shoreline/coastline detail
- conservative simplified linework for zoomed-out display
- never use aggressive simplification for safety-critical shoreline geometry
  while zoomed in

### Symbols

Use instanced textured quads:

- symbol atlas texture
- source rectangle
- pivot
- rotation
- scale
- display priority
- viewing group
- feature id

The current S-101-derived raster atlas is a good starting resource. The new
renderer should make it a `RenderResourceAtlas`, not a `QPixmap` helper.

### Text

Text should be a screen-space pass:

- collect candidates from visible cells
- evaluate scale / viewing group / SCAMIN
- declutter in screen space
- shape/rasterize with glyph atlas
- draw instanced glyph quads or cached label textures

Do not bake all text into static map pixels. Labels need display-scale and
declutter decisions.

### Picking

Build a pick index from prepared cells:

- feature id
- feature class
- bbox
- geometry reference
- display priority
- source product/cell

Picking should not depend on rendered pixels. It should use geometry and spatial
indexes.

## Layer 6: Retained renderer backend

### First backend: OpenGL through QOpenGLWidget

Use `QOpenGLWidget` initially because it fits the current Qt app and deployment
model. Keep backend seams clean enough that Qt RHI, Vulkan, Metal, or Direct3D
can replace it later.

Renderer responsibilities:

- maintain GPU buffers for visible prepared cells
- update camera uniforms for pan/zoom
- draw by display priority
- draw area fills, lines, symbols, text, overlays
- support hit-testing via CPU pick index
- handle device pixel ratio
- handle day/dusk/night palette changes by swapping style resources

### Frame loop

```text
on pan/zoom frame:
  update camera matrix
  cull visible prepared cell batches
  draw retained area/line/symbol batches
  run screen-space label pass
  draw dynamic overlays

on cell enters view:
  request prepared cell from cache/load queue
  upload buffers when ready
  no blocking UI

on portrayal/style change:
  invalidate prepared render cache for affected products
  rebuild asynchronously
  keep old style visible until new batches are ready
```

### No giant static pixmap

The new backend should not use a giant static chart pixmap as the primary
performance mechanism. It can still use small internal caches:

- glyph atlas
- symbol texture atlas
- raster tile textures
- prepared cell buffers
- optional screenshot cache for route previews or thumbnails

But pan and zoom should be retained geometry, not shifted bitmap hacks.

## Layer 7: ChartView integration

Keep `ChartView` as the UI shell:

- mouse/touch interaction
- camera state
- follow-ownship behavior
- settings
- status signals
- overlay registration
- route/AIS/ownship dynamic passes

Extract the chart renderer behind an interface:

```cpp
class IChartRenderer {
public:
    virtual ~IChartRenderer() = default;
    virtual QWidget* widget() = 0;
    virtual void setCamera(const ChartCamera& camera) = 0;
    virtual void setChartSet(const ChartSetRef& charts) = 0;
    virtual void setDisplaySettings(const ChartDisplaySettings& settings) = 0;
    virtual void requestRepaint(RepaintReason reason) = 0;
    virtual PickResult pick(const QPointF& screenPos) const = 0;
};
```

Backends:

- `PainterChartRenderer` - current implementation, compatibility/debug
- `PreparedGpuChartRenderer` - new retained backend

Do not require every plugin overlay to know which backend is active. Preserve a
simple overlay API for dynamic application overlays.

## Interoperability and mixed products

S-100-era systems are not just "draw one chart layer". They may need S-98-style
interoperability rules between products.

The architecture should therefore keep:

- product id
- feature id
- viewing group
- display plane
- display priority
- interoperability category
- source metadata

available through the prepared render pipeline.

Initial implementation can render one base chart product plus overlays, but the
model should not make that a permanent assumption.

## Migration plan

### Phase 0: Stabilize current renderer

Goal: keep the app usable while building the new path.

Tasks:

- Keep the repaint governor.
- Keep the small static pixmap cache only as a compatibility renderer feature.
- Avoid larger cache aprons; they multiply cache-render cost.
- Make `staticDirty_` precise so routine pan-settle does not rebuild static pixels.
- Add lightweight frame timing telemetry:
  - visible cells
  - static cache render ms
  - paint ms
  - point/symbol/text counts
  - cache hit/miss counts

Exit criteria:

- current renderer is stable enough to compare against the new backend
- performance numbers are visible in logs or a debug overlay

### Phase 1: Prepared parsed-cell cache

Goal: eliminate cold-cell parse latency.

Tasks:

- Add binary parsed-cell cache format for current S-57 `Feature` data.
- Key it by source file metadata, decoder version, projection version, and cache
  format version.
- Modify `dispatchLoad` to read cache before GDAL.
- Write cache after GDAL parse.
- Add "Prepare selected charts" action.

This phase benefits both the current renderer and the future renderer.

Exit criteria:

- restarting the app over a prepared chart set avoids GDAL cell parses for visible
  cells
- deleting the cache is safe
- changed charts invalidate correctly

### Phase 2: Normalize product model

Goal: stop making S-57 acronyms the universal internal model.

Tasks:

- Introduce `ProductId`, `FeatureClassId`, typed attributes, `GeometryStore`.
- Adapt S-57 decoder to emit normalized features.
- Keep a compatibility adapter from normalized S-57 features to current
  `Feature`/`SymAtlas` path.
- Add test fixtures for feature/attribute identity and geometry projection.

Exit criteria:

- S-57 renders unchanged through compatibility adapter
- normalized model can represent S-101-style feature ids and associations

### Phase 3: Portrayal package abstraction

Goal: make symbol/presentation updates data-package changes.

Tasks:

- Split `SymAtlas` into resource atlas, portrayal package, portrayal evaluator,
  and painter draw helpers.
- Define presentation IR.
- Compile existing S-52 data into the new package format.
- Keep `QPainter` renderer consuming the IR initially.
- Move style mode (day/dusk/night) into portrayal resources.

Exit criteria:

- S-52 output visually matches current renderer
- updating the symbol atlas or portrayal package does not touch chart decoders or
  renderer batches

### Phase 4: Prepared render cache

Goal: precompute the expensive render representation.

Tasks:

- Convert portrayal IR to prepared render objects.
- Cache fill styles, but leave fill tessellation to the clipped/simplified
  `BuiltCell` GPU build.
- Prebuild line batches and symbol/text candidates.
- Serialize prepared render cache to disk.
- Keep prepared cache invalidation separate from parsed cache invalidation.

Exit criteria:

- visible cells can load prepared render data without recomputing portrayal
- prepared cache rebuilds when portrayal package changes

### Phase 5: Retained OpenGL renderer

Goal: make pan/zoom smooth by design.

Tasks:

- Add `PreparedGpuChartRenderer` behind `IChartRenderer`.
- Upload prepared area/line/symbol batches to GPU buffers.
- Implement camera uniform pan/zoom.
- Implement display-priority draw order.
- Implement symbol atlas texture and instanced symbols.
- Implement text candidate collection, declutter, glyph atlas, and text draw.
- Keep existing AIS/route/ownship overlays as separate dynamic passes.

Exit criteria:

- pan/zoom of already-loaded cells does not rebuild chart geometry
- CPU cost is mostly culling and label placement
- no static pixmap cache needed for normal panning
- performance is compared side-by-side with OpenCPN and current backend

### Phase 6: S-101 decoder and portrayal

Goal: add S-101 without rewriting renderer architecture.

Tasks:

- Add `S101ProductDecoder`.
- Load S-101 feature catalogue and map features/attributes into normalized model.
- Load S-101 portrayal catalogue into `PortrayalPackage`.
- Add S-101 parsed cache and prepared render cache.
- Add sample/test data pipeline.
- Add S-101-specific validation tests where possible.

Exit criteria:

- S-101 cells can catalog, prepare, and render through the same retained renderer
  as S-57
- S-101 portrayal package changes do not require renderer changes unless the
  presentation IR lacks an instruction class

## What to avoid

- Do not keep growing the static pixmap cache. It hides one bottleneck by creating
  a larger one.
- Do not make S-101 decode into fake S-57 acronyms as the long-term model.
  Temporary adapters are fine; architecture should not depend on that.
- Do not tie portrayal evaluation to `QPainter`.
- Do not make chart decoders produce GPU buffers directly.
- Do not require plugin chart sources to know about renderer internals.
- Do not build a "S-101 renderer" separate from the "S-57 renderer". Build one
  renderer fed by different product and portrayal packages.

## Success metrics

Measure in release builds on target hardware:

- idle CPU with live GPS/AIS feed
- pan frame time at harbour detail
- zoom-settle latency
- visible-cell upload time
- cold first-view latency before and after prepare
- prepared-cache hit rate
- GPU memory per visible cell
- text-label candidate and accepted counts
- CPU time spent in label declutter

Performance targets:

- panning already-loaded charts should be visually continuous at the display
  refresh rate
- static chart CPU time during pan should be near zero except culling/labels
- idle live-feed CPU should be dominated by dynamic overlays, not chart rendering
- prepared cold view should be bounded by disk read and GPU upload, not GDAL or
  XML/GML parsing

## Recommended next concrete step

Do not start with the full GPU renderer. Start with the prepared parsed-cell cache
because it is useful immediately and is also required by the final architecture.

Concrete first implementation:

1. Create `src/prepared_chart_cache.{hpp,cpp}`.
2. Serialize current `std::vector<Feature>` plus bbox into a versioned binary.
3. Read it in `dispatchLoad` before `chart::loadCellFeatures`.
4. Write it after successful GDAL parse.
5. Add a "Prepare chart cache" dialog/action.
6. Add timing logs for cache hit/miss and parse time.

Then add the normalized product model and portrayal IR. Those are the seams that
make S-101 a product-package addition instead of a renderer rewrite.
