# S-101 portrayal: architecture and roadmap

How S-101 portrayal actually works, why it cannot share an engine with S-52, how
it should be packaged, and a concrete roadmap to a usable (non-ECDIS-certified)
subset. Companion to `docs/renderer_architecture_plan.md` (the layered plan) and
`docs/rendering_pipeline.md` (the current pipeline).

Evidence base: the IHO **S-101 Portrayal Catalogue** at
`C:\Users\WarrenHolybee\source\S-101_Portrayal-Catalogue` (version 2.1.0-DRAFT),
the S-101 ENC Product Specification Ed 2.0.0, and the S-101 DCEG Annex A.

---

## 1. What S-101 portrayal actually is

S-101 portrayal is an **S-100 Part 9 rule engine**: an embedded **Lua 5.1**
interpreter that runs a catalogue of scripts against a feature model and calls
back into the host to emit drawing instructions. It is not a lookup table.

What the catalogue contains:

| Part | Count | Form | S-52 analogue |
|---|---|---|---|
| Feature rules | 216 | Lua scripts, one per feature class (`DepthArea`, `CardinalBuoy`, …) | LUP records + CS procedures |
| Runtime | 5 | `main.lua`, `PortrayalAPI.lua`, `PortrayalModel.lua`, `S100Scripting.lua`, `Default.lua` | the C++ `PortrayalEngine` |
| Symbols | 725 | **SVG** (S-100 tiny profile) + 3 CSS palettes | `rastersymbols-*.png` sprite atlas |
| Line styles | 65 | **SVG** | HPGL `LC` motifs in `symbols.bin` |
| Area fills | 25 | **SVG** | HPGL/raster `AP` patterns |
| Colour profile | 1 | `colorProfile.xml`, day/dusk/night | S-52 colour table |
| Master catalogue | 1 | `portrayal_catalogue.xml` (450 KB) | — |

### Execution model

`main.lua` defines `PortrayalMain(featureIDs)`. Per feature it does
`require(feature.Code)` then calls `_G[feature.Code](feature, featurePortrayal,
contextParameters)` — dynamic dispatch to that class's rule. The rule appends
**drawing instructions** (a string mini-language), and the host receives them
through a callback:

```
HostPortrayalEmit(featureReference, "instr;instr;instr", observedContextParams)
```

The host must implement a **feature-model bridge** the Lua calls into. The full
callback ABI in this catalogue:

```
HostGetFeatureIDs / HostGetFeatureTypeCodes / HostGetFeatureTypeInfo
HostFeatureGetCode / HostGetSimpleAttribute / HostGetSimpleAttributeTypeInfo
HostGetComplexAttributeCount / HostGetComplexAttributeTypeCodes / …TypeInfo
HostGetInformationTypeInfo / HostFeatureGetAssociatedInformationIDs
HostGetFeatureAssociationTypeCodes / HostFeatureGetAssociatedFeatureIDs
HostFeatureGetSpatialAssociations / HostSpatialGetAssociatedFeatureIDs
HostGetSpatial            (geometry)
HostPortrayalEmit         (output)
HostDebuggerEntry         (debug)
```

That list is the point: the engine needs the **whole S-100 general feature model**
— simple *and* complex attributes, information types, feature associations,
spatial (topology) associations, and shared-edge relationships — not a flat
feature with string attributes.

### Two properties that break the S-52 assumptions

Both are visible in the real `DEPARE03.lua` (depth-area) rule:

1. **Context-parameter driven.** Symbology reads `contextParameters.SafetyContour`
   and `contextParameters.RadarOverlay`. The safety contour is a *mariner setting*;
   the same cell re-portrays differently when it changes. The engine reports which
   parameters it *observed* per feature (`observedContextParams`) so the host can
   cache and invalidate precisely.

2. **Topology/association driven.** The rule walks
   `feature:GetFlattenedSpatialAssociations()` to find features sharing a boundary
   curve, decides whether that boundary *is* the safety contour, reads a neighbour's
   `depthRangeMinimumValue`, and pulls an information association for
   `qualityOfHorizontalMeasurement` to pick a dashed vs solid line. None of this is
   expressible over a flat, per-feature S-57 acronym list.

### S-52 vs S-101 at a glance

| | S-52 (current) | S-101 (S-100 Part 9) |
|---|---|---|
| Engine | C++ interpreter of compiled tables (`PortrayalEngine`) | Embedded **Lua 5.1** script host |
| Rules | LUP match + fixed CS procedures | 216 general-purpose scripts |
| Input | object-class acronym + `(acronym, string)` attrs | full GFM: complex attrs, info types, associations, topology |
| Symbols | raster sprite atlas + HPGL | **SVG** + CSS palettes |
| Colours | S-52 tokens | **same tokens** (CHBLK, DEPSC, DRGARE, …) — shared vocabulary |
| Output | drawing calls | drawing-instruction strings via `HostPortrayalEmit` |
| Dynamics | some CS depend on safety contour | pervasive context parameters + observed-param feedback |

The colour tokens being identical is the one big gift: the palette layer is
largely reusable.

### Do not reimplement S-101 rules in C++

The 216 scripts **are** the specification's portrayal, delivered as data and
updated on their own cadence (this copy is already 2.1.0-DRAFT). Reimplementing
them in C++ would mean forking the standard and re-syncing forever. The S-101
"engine" is therefore: *embed Lua + implement the Host ABI + ship/update the
catalogue unmodified.* This is the opposite of the S-52 engine (which is correctly
a C++ reimplementation of a small, frozen instruction set).

---

## 2. Q1 — One engine, or two selected by chart set?

**Two engines, selected by the chart's `ProductId`.** A single engine "supporting
both" would just be two implementations behind an `if` — they share no execution
machinery (compiled-table interpreter vs Lua script host). Forcing them together
couples unrelated code and makes each harder to evolve.

Unify at the **two interfaces around** the engine, not inside it:

- **Input:** both engines read the normalized `ChartFeature` (`product_model.hpp`),
  not S-57 acronyms.
- **Output:** both emit the **same renderer-neutral IR** (today's `SymHit` in
  `portrayal_ir.hpp`, extended — see §4c). Everything downstream (scene compiler,
  cell builder, GPU/CPU backends, caches) stays format-agnostic and unchanged.

Introduce a seam:

```cpp
class IPortrayalEngine {
public:
    virtual ~IPortrayalEngine() = default;
    virtual ProductId product() const = 0;          // which product it portrays
    virtual RenderIR evaluate(const ChartFeature& f,
                              const FeatureContext& ctx,   // neighbours/associations
                              const PortrayalParams& params) const = 0; // safety contour, mode
};
```

`S52PortrayalEngine` (wrapping today's `PortrayalEngine`) and `S101PortrayalEngine`
(the Lua host) both implement it. The scene compiler picks the engine whose
`product()` matches the cell's `ProductId`. This is exactly the "portrayal package
compatibility" the architecture plan anticipated — now with a hard requirement
that the engine internals differ.

---

## 3. Q2 — Portrayal engine as part of a plugin package?

**Yes — but recognise there are three separable things, not two,** and keep the
Lua interpreter off the plugin ABI.

| Concern | What it is | Where it should live |
|---|---|---|
| **Product decoder** | reads S-101 dataset → normalized features + topology | plugin DLL (per product) |
| **Portrayal engine runtime** | the S-100 Lua host + Host ABI bridge | **core** (generic; reused by S-102/S-104/S-111) |
| **Portrayal catalogue** | the 216 Lua rules + SVG + colour profile | installable **data** (versioned, updates independently) |

The S-100 Lua host is not S-101-specific — every S-10x product portrays through
the same Part 9 mechanism. So the reusable runtime belongs in the core (or a
core-shipped "S-100 portrayal" module), and each product ships a **decoder + a
catalogue data package**. The engine is then configured by pointing it at a
catalogue, not by shipping a new interpreter each time.

Your "two DLLs installed as one package" vision still works and the plugin host
already supports it — `PluginManager::loadFromDirectory()` discovers multiple
DLLs, and an installer can drop several DLLs plus a data folder. The important
constraints:

- **Keep the ABI at value-type / IR granularity.** What crosses the DLL boundary
  is *normalized features in, `RenderIR` out* — never Lua state, never QObject
  graphs. This is the same discipline `IChartSource` already follows (it returns
  plain value types). A `lua_State*` or a Qt SVG renderer must never appear in an
  exported signature.
- **Package = {decoder, [engine or reuse core], catalogue+resources}.** Whether
  that is 1, 2, or 3 DLLs plus a data folder is a packaging choice, not an
  architectural one. Recommended default: **decoder DLL + catalogue data**, reusing
  the core S-100 engine. Offer a separate portrayal-engine DLL only if a product
  needs a non-standard engine.
- **Register both via `ICoreApi`** (see §4b), selected by `ProductId`, so the host
  wires decoder→engine→renderer without special-casing.

Why *not* the existing `IChartSource` seam for S-101: it requires translating the
product onto S-57 acronyms and emitting flat `Feature`s. That throws away exactly
the complex attributes, information types, and shared-edge topology that
`DEPARE03` and its peers depend on. `IChartSource` is right for "a new file format
with S-57 semantics" (CM93); it is the wrong seam for S-101.

---

## 4. Q3 — Roadmap: closing (a)–(c) and landing S-101

Recap of the three gaps from the earlier architecture review:

- **(a)** make the normalized product model the load-bearing spine (today it
  round-trips straight back to `Feature` in `cell_source.cpp`);
- **(b)** add plugin-registration surfaces on `ICoreApi`
  (`registerProductDecoder`, `registerPortrayalEngine`), selected by `ProductId`;
- **(c)** generalize portrayal input/output off S-57 acronyms.

S-101 is the forcing function that turns these from cleanups into requirements.

### (a) Make the normalized model load-bearing

- Route the build/portrayal path off `std::vector<Feature>` and onto
  `ProductFeatureSet` / `ChartFeature`. Keep the S-57→`Feature` back-adapter only
  as a fallback for the legacy S-52 engine until it reads `ChartFeature` directly.
- **Grow the model for S-100 semantics** (the real work): populate
  `AssociationRef` (feature + information associations), add **complex attributes**
  and **information types**, and add **shared-edge / spatial-association topology**
  to `GeometryStore` so a feature can find the features sharing a boundary curve.
  The `DEPARE03` walk is the acceptance test.

### (b) Registration surfaces on `ICoreApi`

```cpp
void registerProductDecoder(IChartProductDecoder* d);     // by ProductId
void registerPortrayalEngine(IPortrayalEngine* e);        // by ProductId
```

Host wiring by `ProductId`: catalog picks the decoder that `canHandle()` a folder;
the scene compiler picks the engine whose `product()` matches the cell. Bump the
plugin ABI version (currently 4) when these land. Overlays, camera, quilt, and
both render backends are untouched.

### (c) Generalize portrayal input/output

- **Input:** `IPortrayalEngine::evaluate(ChartFeature, FeatureContext, params)` —
  namespace-qualified ids + typed attrs + a neighbour/association accessor +
  context parameters. Not `(acronym, geom, attrs)`.
- **Output IR:** extend `SymHit` (or move to a small `RenderInstruction` list) to
  the union the S-101 instruction language needs. From the rules, at minimum add:
  area-fill reference by name, complex line style by name, **viewing group**,
  **drawing priority**, **display plane** (over/under radar), **line placement**
  (`Relative,0.5,…` — symbols/text positioned along a curve), and an **alert
  reference** channel. Symbols, simple line, fill, and text already exist.
- **Resource model:** make `RenderResourceAtlas` an interface with two backers:
  the current raster+HPGL atlas for S-52, and an **SVG** resource set for S-101
  (symbols, line styles, area fills) with CSS-driven day/dusk/night palettes.
  Colour tokens are shared, so the palette lookup is common.

### S-101-specific build-out

1. **Add dependencies.** Neither Lua nor Qt SVG is in `vcpkg.json` today. Add a
   **Lua 5.1** engine (reference `lua` 5.1.5, or LuaJIT in 5.1-compat mode for
   speed — the catalogue explicitly targets 5.1 semantics) and an SVG rasteriser
   (Qt SVG, or a dedicated tiny-SVG renderer).
2. **Embed the Lua host + Host ABI bridge.** Implement the ~20 `Host*` callbacks
   over `ChartFeature` and the geometry/topology store. Load `main.lua` + runtime +
   rules once; call `PortrayalMain` per visible feature set.
3. **SVG resource pipeline.** Either **bake offline** to an atlas (mirroring
   `tools/gen_symbols.cpp`, keeping runtime cheap and the GPU atlas path reusable)
   or **rasterise at runtime** with an LRU cache. Offline baking is recommended to
   preserve the current fast atlas draw path; runtime SVG is a fallback for
   symbols not yet baked.
4. **Context parameters + observed-parameter caching.** Feed safety contour,
   shallow/deep contours, display mode, radar overlay, etc. as `PortrayalParams`.
   Key the prepared-render cache on `(cell version, catalogue version, observed
   params)` using the engine's observed-parameter feedback, so changing the safety
   contour re-portrays only affected features. This is genuinely new work: the
   current S-52 path *sidesteps* context-dependent portrayal (fixed
   `kSafetyDepthM`; depth shading done in `cell_builder` from raw depth), so its
   cache needs no context key. S-101 is the first product that requires one — see
   §5.
5. **Text placement + viewing groups.** Wire the second-pass `TextPlacement`
   features and map viewing group / drawing priority / display plane onto the
   existing draw-order and layer toggles.
6. **Scope the subset.** For a usable non-ECDIS target, land base + standard
   display, depth/soundings, buoys/beacons/lights, land/coastline, and the common
   area/line features first; defer alerts, S-98 interoperability, and the long tail
   of CS edge cases. Track coverage as "rules exercised without falling back to
   `Default`".

### What is reused unchanged

The GPU and CPU render backends (`GpuChartView`, `ChartView` painter path), the
`IChartRenderer` seam, `IChartOverlay`, camera/quilt/catalog, the prepared/parsed
cache *infrastructure*, and the colour-token vocabulary all stay as-is. S-101
plugs in by adding a decoder, an engine, a catalogue, and IR/resource breadth —
not by touching the renderer.

---

## 5. Performance and caching — is in-process Lua affordable?

The concern: S-57 rendering was hard to make fast, and adding an interpreted Lua
engine in-process sounds like it threatens that. It does not — because **portrayal
is upstream of the frame path and its output already caches to disk**, at the exact
layer S-52 portrayal uses today.

### Portrayal is not on the frame path

```
decode -> PORTRAY (S-52 or S-101 Lua) -> [DISK CACHE] -> instantiate
       -> GPU batches / static pixmap -> frame loop
                                          ^ pan / zoom / restart live here
```

The frame loop only touches things *after* the cache: the retained GPU vertex
buffers in `GpuChartView` (pan/zoom = a camera-uniform update) and the painter
static pixmap in `ChartView`. Portrayal — S-52 or Lua — runs **before** the cache
and never during a frame. Bringing Lua in-process therefore cannot regress
pan/zoom frame time. The retained-geometry work that made S-57 fast is exactly
what protects S-101: the Lua-produced IR feeds the same GPU batches the S-57
geometry does.

### It caches at the layer S-52 already uses

The portrayal result is the **third cache level**, `prepared_render_cache`
(`prepared_render.hpp` / `prepared_render_cache.cpp`): one `SymHit`/`RenderIR` per
feature, written to disk, keyed by source identity + code versions + portrayal
fingerprint. `scene::compileScene` produces it once per cell. The S-101 Lua
engine slots in precisely where `PortrayalEngine::evaluate` sits now — so its cost
is paid **once per cell per catalogue version**, then every future load (and every
pan/zoom/restart) reads the IR back parse-free and portray-free.

### Where the Lua actually runs — never the GUI thread

Only two places, both already background/worker-thread today:

- the **"Prepare charts"** batch step (architecture plan: `nCPU-1` workers) — the
  big up-front pass, done once;
- **cold-cell load** when a cell first enters view uncached — on the build worker
  pool.

Lua never runs in a `paintEvent` or on the GUI thread.

### The one caveat to "done once and never again"

"Never again" holds **for a fixed set of display settings**. S-101 portrayal is
context-dependent (the real `DEPARE03` reads `contextParameters.SafetyContour`;
day/dusk/night and display mode also matter). When the mariner changes one of
those, affected features must re-portray. Two things make this bounded, not a
per-frame cost:

1. **The catalogue already tracks it.** `main.lua` records the *observed* context
   parameters per feature. Key the cache on `(cell, catalogue version, observed
   params)` and re-run Lua only for features that read the setting that changed —
   backgrounded, and rare (nobody changes their safety contour mid-pan).
2. **It is new work, not a regression.** The current app has no context-dependent
   portrayal to break: it uses a fixed `kSafetyDepthM = 20.0` and shades depth
   bands in `cell_builder` from raw depth (which is why feet/metres is just a
   repaint). So today's cache correctly needs no context key; S-101 is simply the
   first product that adds one.

### Making the up-front pass itself fast

The prepare pass is one-time, parallel, and amortized across all future sessions,
but still worth optimising:

- **Use LuaJIT in 5.1-compat mode** (the catalogue targets Lua 5.1 semantics).
  LuaJIT is typically 10–50× reference Lua for this workload — the difference
  between hours and minutes over a large region.
- **One Lua state per worker thread**, reused across cells (state creation is the
  expensive part; per-feature evaluation is cheap).
- Parallelise across cells on the existing worker pool.

### Residual risks (all off the frame path)

- **Disk, not CPU.** More features + richer IR grow the on-disk cache. Storage,
  not frame time.
- **Parsed-cache format grows** to hold the normalized model + topology, not flat
  `Feature`s. One-time / on-disk.
- **Text placement** is the one genuinely scale/view-dependent Lua pass
  (`TextPlacement` in `main.lua`). Cache its *candidates* per scale band; final
  screen-space declutter is cheap host-side geometry, not per-frame Lua.
- **Cold first-view** keeps the same shape it has for S-57 today: bounded by disk
  read + clip/simplify + GPU upload, not by portrayal.

**Bottom line:** in-process Lua is a prepare-time cost, cached to disk exactly like
S-52 portrayal is, and completely off the pan/zoom path. Do it up front once per
catalogue version; re-do only a bounded, backgrounded subset when a display
setting changes.

---

## 6. Recommended order of work

1. Extend the IR (`SymHit`/`RenderInstruction`) and make the S-52 engine read
   `ChartFeature` + emit the extended IR. No behaviour change; proves the seam.
2. Make the normalized model load-bearing (a) and add the `ICoreApi` registration
   surfaces (b). Still S-57 only; regression target is "renders identically".
3. Grow the model for associations / complex attrs / topology (c-input) with the
   `DEPARE03` neighbour walk as the test.
4. Split `RenderResourceAtlas` into an interface + SVG backer; add the SVG bake
   tool. S-52 still uses the raster backer.
5. Embed Lua 5.1, implement the Host ABI bridge, load the catalogue, wire
   `S101PortrayalEngine` behind `IPortrayalEngine`.
6. Add the S-101 decoder (real S-101/GML dataset parse into the normalized model),
   context parameters, and observed-parameter cache keying.
7. Iterate coverage toward the usable subset; measure fallback-to-`Default` rate.

Steps 1–2 are pure architecture hardening that pay off even without S-101. Steps
3–6 are where S-101 becomes real. The renderer never gets rewritten — which was
the whole point of the layered design.
