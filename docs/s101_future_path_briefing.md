# S-101 future-path briefing (session notes)

**Status: not being implemented now.** This is a "start here when we pick up
S-101" briefing that records the architecture review and decisions from the design
session on 2026-07-04, so the context is not lost. Nothing in the codebase was
changed for S-101; this is planning only.

Read alongside the detailed docs this session produced:

- `docs/rendering_pipeline.md` — ASCII pipeline diagram, GDAL → canvas, GPU + CPU.
- `docs/s101_portrayal_architecture.md` — the full S-101 portrayal analysis,
  answers, and roadmap (the primary reference; this file is the digest).
- `docs/renderer_architecture_plan.md` — the pre-existing layered plan these build on.

---

## 1. Why we looked

Goal check: is the app modular enough that S-101 (charts **and** symbols, which are
very different from S-57) can be added mostly as a plugin, without rewriting the
renderer? Then a deeper dive specifically into **portrayal**, plus a hard look at
**performance**.

---

## 2. Where the architecture stands today (good news)

The pipeline is cleanly layered with real, named seams (verified in code):

- **Render backend** — `IChartRenderer` (`chart_renderer.hpp`), `RenderBackend`
  (`render_backend.hpp`). CPU painter (`ChartView`) vs retained GPU
  (`GpuChartView`, `QRhiWidget` → D3D11/Metal/GL). Overlays are backend-agnostic
  (`IChartOverlay`).
- **Portrayal split (S-52)** — `PortrayalPackage` + `PortrayalEngine`
  (`portrayal_engine.*`) emit a renderer-neutral `SymHit` IR (`portrayal_ir.hpp`);
  `RenderResourceAtlas` (`render_resource_atlas.*`) holds resources + draw helpers.
- **Normalized product model** — `ProductFeatureSet` / `ChartFeature`
  (`product_model.hpp`), with `IChartProductDecoder` (`product_decoder.hpp`).
- **Chart-source plugin seam** — `IChartSource` (`chart_source.hpp`), proven by the
  CM93 DLL.
- **Caches** — parsed-cell (`prepared_chart_cache.*`) and prepared-render
  (`prepared_render_cache.*`, keyed by portrayal fingerprint).

Portrayal already runs once per cell and is cached to disk. This matters a lot for
performance (see §6).

## 3. The three gaps that block "S-101 as a plugin" — (a),(b),(c)

1. **(a) The normalized model is not load-bearing.** The built-in path decodes
   GDAL → `ProductFeatureSet` → **immediately** `product_adapter::toLegacyFeatures`
   back to `std::vector<Feature>` (`cell_source.cpp:48`). Everything downstream runs
   on the legacy `Feature`. The neutral model exists but is a round-trip, not the
   spine.
2. **(b) No plugin registration surface for decoders/portrayal.** `ICoreApi` has
   `registerChartSource` but no `registerProductDecoder` / `registerPortrayalEngine`.
   `S57ProductDecoder` is hard-wired in-tree.
3. **(c) Portrayal I/O is S-57-shaped.** `PortrayalEngine::evaluate(objClass, geom,
   attrs)` takes 6-char acronyms + string attrs; `symbols.bin` is S-52-shaped; the
   `SymHit` IR is a fixed struct. Output is neutral, input and package format are not.

`IChartSource` is the wrong seam for S-101: it forces translation to S-57 acronyms
and flat features, discarding exactly the complex attributes / associations /
topology S-101 portrayal needs. Right for CM93 (S-57 semantics), wrong for S-101.

## 4. What S-101 portrayal actually is (the key discovery)

Inspected the real IHO catalogue at
`C:\Users\WarrenHolybee\source\S-101_Portrayal-Catalogue` (v2.1.0-DRAFT):

- **An embedded Lua 5.1 rule engine** (S-100 Part 9), *not* a lookup table.
- **216 Lua rule scripts** (one per feature class) + runtime (`main.lua`,
  `PortrayalAPI.lua`, `PortrayalModel.lua`, `S100Scripting.lua`, `Default.lua`).
- **725 SVG symbols**, 65 SVG line styles, 25 SVG area fills, `colorProfile.xml`
  (day/dusk/night) with CSS palettes.
- Rules emit a **string instruction language**; the host receives it via
  `HostPortrayalEmit(...)` and must implement ~20 `Host*` callbacks exposing the
  **full S-100 general feature model** (simple + complex attributes, information
  types, feature + spatial associations, shared-edge topology, geometry).
- Two hard properties (both visible in `DEPARE03.lua`): portrayal is
  **context-parameter driven** (`SafetyContour`, day/dusk/night, radar overlay) and
  **topology/association driven** (walks shared boundary curves to neighbours,
  reads their depth + position-quality to pick line style).
- **Shared with S-52:** the colour tokens (CHBLK, DEPSC, DRGARE, …) are identical.
  That palette layer is reusable; almost nothing else is.

Corollary: **do not reimplement the 216 rules in C++.** They *are* the spec,
delivered as data and versioned independently. The S-101 "engine" = embed Lua +
implement the Host ABI + ship/update the catalogue unmodified. (Opposite of S-52,
which is correctly a C++ reimplementation of a small frozen instruction set.)

## 5. Decisions reached (the answers)

- **One engine or two?** → **Two**, selected by `ProductId`. They share no
  execution machinery (compiled-table interpreter vs Lua host). Unify at the seams
  *around* the engine: common input (normalized `ChartFeature`) and common neutral
  output IR. Introduce `IPortrayalEngine { product(); evaluate(feature, context,
  params) -> RenderIR; }`; `S52PortrayalEngine` and `S101PortrayalEngine` implement
  it; the scene compiler picks by `ProductId`.
- **Portrayal engine in a plugin package?** → **Yes, but it's three separable
  things:** product **decoder** (plugin DLL), the S-100 **Lua runtime** (belongs in
  *core* — reused by S-102/S-104/S-111), and the **catalogue** (installable
  versioned data). The "two DLLs as one install" vision works (`PluginManager`
  discovers multiple DLLs); the hard rule is **only value types / IR cross the DLL
  boundary — never a `lua_State*` or Qt SVG objects.** Recommended default package:
  *decoder DLL + catalogue data*, reusing the core engine.
- **Full ECDIS?** No. Target a usable subset; measure coverage by
  "rules exercised without falling back to `Default`".

## 6. Performance verdict (the top concern)

**In-process Lua does not threaten frame time**, because portrayal is upstream of
the frame path and its output already caches to disk (`prepared_render_cache`):

```
decode -> PORTRAY (S-52 or Lua) -> [DISK CACHE] -> instantiate -> GPU/pixmap -> frame loop
```

- The frame loop only touches post-cache artifacts (retained GPU buffers; static
  pixmap). Pan/zoom/restart pay **zero Lua**.
- Lua runs only in the **"Prepare charts"** batch step and **cold-cell load**, on
  worker threads — never the GUI thread. Cost is paid **once per cell per catalogue
  version**.
- Caveat to "never again": S-101 portrayal is context-dependent, so changing
  **safety contour / day-dusk-night / display mode** re-portrays affected features.
  Bounded and backgrounded: `main.lua` reports *observed* context parameters, so
  cache on `(cell, catalogue version, observed params)` and re-run only affected
  features. This is **new work, not a regression** — today's S-52 path sidesteps
  context dependence (fixed `kSafetyDepthM`; depth shading in `cell_builder` from
  raw depth), so its cache correctly needs no context key.
- Make the prepare pass fast: **LuaJIT (5.1-compat)**, one Lua state per worker,
  parallel across cells.
- Residual risks are all off the frame path: bigger on-disk cache, richer parsed
  cache format, text placement is scale-dependent (cache candidates per band),
  cold-first-view unchanged in shape.

## 7. Roadmap (condensed; full version in s101_portrayal_architecture.md §6)

1. Extend the IR and move the **S-52** engine onto `ChartFeature` + extended IR.
   No behaviour change — proves the seam.
2. Make the normalized model load-bearing **(a)** + add `ICoreApi` registration
   surfaces **(b)**. Still S-57; regression target "renders identically".
3. Grow the model for associations / complex attrs / shared-edge topology
   **(c-input)**; acceptance test = the `DEPARE03` neighbour walk.
4. Split `RenderResourceAtlas` into interface + **SVG** backer; add an SVG bake tool
   (mirror `tools/gen_symbols.cpp`). S-52 keeps the raster backer.
5. Embed **Lua 5.1 / LuaJIT**, implement the Host ABI bridge, load the catalogue,
   wire `S101PortrayalEngine` behind `IPortrayalEngine`.
6. Add the **S-101 decoder** (real dataset parse into the normalized model),
   context parameters, and observed-parameter cache keying.
7. Iterate coverage toward the usable subset.

Steps 1–2 are pure architecture hardening that pay off even without S-101. The
renderer is never rewritten — the whole point of the layered design.

## 8. New dependencies to add when starting

- A **Lua 5.1** engine (reference `lua` 5.1.5, or **LuaJIT** in 5.1-compat mode —
  recommended for prepare-pass throughput). Not in `vcpkg.json` today.
- An **SVG** rasteriser (Qt SVG, or a dedicated S-100 tiny-SVG renderer). Not in
  `vcpkg.json` today.

## 9. Key references

- **S-101 Portrayal Catalogue:** `C:\Users\WarrenHolybee\source\S-101_Portrayal-Catalogue`
  (`PortrayalCatalog/Rules`, `/Symbols`, `/LineStyles`, `/AreaFills`,
  `/ColorProfiles`, `portrayal_catalogue.xml`).
- **Specs:** S-101 ENC Product Specification Ed 2.0.0; S-101 DCEG Annex A Ed 2.0.0
  (in `~/Downloads` this session).
- **Code seams to touch:** `product_model.hpp`, `product_decoder.*`,
  `cell_source.cpp` (the round-trip at line ~48), `core_api.*`,
  `portrayal_engine.*`, `portrayal_ir.hpp`, `render_resource_atlas.*`,
  `render_scene_compiler.*`, `prepared_render_cache.*`. Untouched by design:
  `gpu_chart_view.*`, `chart_view.cpp` render path, `chart_renderer.hpp`,
  overlays, quilt.
