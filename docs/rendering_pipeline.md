# Chart rendering pipeline

ASCII block diagram of the chart rendering pipeline, from the GDAL decoder to
the pixels on the canvas. It shows both the CPU (`QPainter`) and GPU (retained
`QRhiWidget`) draw paths, the offline symbol bake, and how S-52 symbology is
processed and drawn in both backends. Each block names the file and/or C++
class that does the work. Stage numbers `[n]` map to the layers in
`docs/renderer_architecture_plan.md`.

```text
================================================================================
  OFFLINE  (build time, once)                              symbol resource bake
================================================================================

   data/chartsymbols.xml            IHO S-101 SVG symbol library
   (S-52 LUP rules, colour table,   (symbol artwork, re-rasterised)
    HPGL line/area programs)                 |
            |                                |
            +----------------+---------------+
                             v
                   tools/gen_symbols.cpp          (host-side build executable)
                             |
                             v
        data/symbols.bin  +  data/rastersymbols-{day,dusk,dark}.png


================================================================================
  LOAD TIME  (startup, once, GUI thread)          portrayal resources resident
================================================================================

                   SymAtlas::load()                sym_atlas.cpp  (thin facade)
                             |
             +---------------+-----------------------+
             v                                       v
     PortrayalPackage                        RenderResourceAtlas
     portrayal_engine.cpp                    render_resource_atlas.cpp
     - per-class LUP match tables            - point-symbol sprite atlas (rects,
     - attribute conditions                    pivots, name -> index)
     - S-52 colour table                     - LC line-complex HPGL motifs
     - instruction blob (SY/LS/AC/TX/CS)     - AP area-pattern tiles
        the swappable RULE/STYLE data          - QPainter draw helpers
             |                                       |
             |  (rule half)                          |  (resource half)
             +------------------+--------------------+
                                |
                                v   used by [4] PORTRAY and by the canvas


================================================================================
  PER-CELL PIPELINE  (worker threads via QtConcurrent pool)
================================================================================

  [1] DECODE  -- product decoders (Layer 1)
  ..............................................................................
     BUILT-IN ENC / S-57                     PLUGIN CHART SOURCE
     chart_loader.cpp                        chart_source.hpp : IChartSource
     chart::loadCellFeatures                 (e.g. CM93 -- runtime DLL, GPL repo)
     (GDAL / OGR "S57" driver)               registered via
            |                                ICoreApi::registerChartSource()
            v                                loadCell() -> Feature[]  (must emit
     S57ProductDecoder                       S-57 acronyms + projected geometry)
     product_decoder.cpp                              |
            |                                         |
            v                                         |
     ProductFeatureSet  (Layer 2, normalized)         |
     product_model.hpp                                |
     - FeatureClassId  "S57:DEPARE" ...               |
       (ready for "S101:DepthArea")                   |
     - typed AttributeValue[]                         |
     - GeometryStore (shared rings)                   |
     - AssociationRef[]  (S-100 assoc.)               |
            |                                          |
            |  product_adapter::toLegacyFeatures       |
            |  (TRANSITIONAL round-trip back to        |
            |   the legacy Feature spine)              |
            +---------------------+--------------------+
                                  v
                       std::vector<Feature>           <-- the real spine today
                       cellsource::parseCell           cell_source.cpp
                                  |
                                  v
                       parsed-cell cache (versioned binary, on disk)
                       prepared_chart_cache.cpp        (GDAL parse skipped on hit)

  [4] PORTRAY  -- run S-52 rules once per feature (Layer 4)
  ..............................................................................
                       scene::compileScene            render_scene_compiler.cpp
                                  |
                                  |  PortrayalEngine::evaluate()  portrayal_engine.cpp
                                  |  reads PortrayalPackage + RenderResourceAtlas
                                  v
                       PreparedCellRender             prepared_render.hpp
                         one SymHit per feature       portrayal_ir.hpp
                         (renderer-NEUTRAL IR):       - symbol stamps  SY()
                         symbols / line / fill /      - line style     LS()
                         LC / AP / text / sectors     - fill wash      AC()
                                  |                    - LC / AP refs
                                  v                    - text          TX()/TE()
                       prepared-render cache           - light sectors CS(LIGHTS)
                       prepared_render_cache.cpp       keyed by portrayal
                                  |                     fingerprint
                                  v
  [5] INSTANTIATE  -- clip + simplify to the current view (Layer 5)
  ..............................................................................
                       cellbuilder::instantiateCell   cell_builder.cpp
                         clip       geom_clip.hpp
                         simplify   (vertex-merge, zoom tolerance)
                         tessellate geom_tessellate.hpp
                                  |
                                  v
                       BuiltCell   built_cell.hpp   (Qt value object, per view)
                         BuiltPath[]  fills + lines (QPainterPath + style)
                         Sounding[]   BuiltSymbol[]  BuiltText[]
                         BuiltLightSector[]
                         + GpuVertex tris/lines/contours   <-- only when GPU on
                           gpu_batches.cpp
                           appendBuiltCellFills / appendCellBatches
                                  |
                                  |
   ===========================  [6/7] RENDER  ==================================
       IChartRenderer seam (chart_renderer.hpp) + RenderBackend (render_backend.hpp)
       ChartView picks the backend; overlays never learn which one is active.
   ============================================================================
                                  |
              +-------------------+---------------------------+
              v                                               v
    CPU / PAINTER BACKEND                          GPU BACKEND  (retained)
    ChartView            chart_view.cpp            GpuChartView   gpu_chart_view.cpp
    (implements IChartRenderer)                    QRhiWidget -> D3D11 / Metal / GL
              |                                     shaders: src/shaders/*.vert,*.frag
    paintEvent()                                              |
      |                                            setCell(): ONE retained vertex-
      v                                              buffer set per cell (tris /
    renderStaticCache()  -> offscreen QPixmap         lines / contours), uploaded
      draws, in scene coords:                         once; CPU copy freed
        - basemap fills                            setCamera(): pan/zoom is a
        - raster MBTiles tiles  (drawRasterCharts)   UNIFORM update -- no vertex
        - BuiltCell.paths       (QPainter fills/     copy, no rebuild, no re-raster
          lines)                                   setRasterTiles(): textured quads
        - CONSTANT-SIZE S-52 SYMBOLOGY  (A)          per MBTiles tile
      |                                            per-cell viewport culling
      v                                                       |
    blitStaticCache()  (shift/scale while panning) retained fills+lines+contours
      |                                              + raster live on the GPU
      v                                                       |
    paintDynamic()  <------- shared dynamic pass ----------> overlayLayer_ (QWidget,
      - ownship glyph        (same code, same camera)          transparent, on top
      - scale bar                                     |        of the RHI surface)
      - plugin IChartOverlay[]  (device coords,       |      paintDynamic() draws:
        backend-agnostic; ChartViewport helper)       |        - CONSTANT-SIZE S-52
                                  |                    |          SYMBOLOGY  (A)
                                  |                    |        - ownship / scale bar
                                  |                    |        - plugin overlays
                                  |                    |      (symbology served from
                                  |                    |       a transparent-bg apron
                                  |                    |       cache; blitted while
                                  |                    |       panning, same as CPU)
                                  |                    |
    (A) CONSTANT-SIZE S-52 SYMBOLOGY  --  QPainter in BOTH backends
    ............................................................................
        Reads BuiltCell + PreparedCellRender SymHits; drawn at fixed on-screen
        size (never baked into GPU geometry) via:
          RenderResourceAtlas::draw()             point symbols  (sprite atlas)
          RenderResourceAtlas::drawLineComplex()  LC motifs stamped along lines
          RenderResourceAtlas::fillAreaPattern()  AP tiled area patterns
          + ChartView draws Sounding / BuiltText / BuiltLightSector
        Difference between backends: the CPU cache also holds fills/lines/raster;
        the GPU path keeps only this symbology in the painter apron (fills, lines,
        contours and raster are retained GPU geometry instead).


  RASTER (MBTiles) SIDE LANE  -- feeds both backends
  ..............................................................................
     MbtilesService (own thread)   mbtiles_service.cpp
     MbtilesReader (SQLite)        mbtiles_reader.cpp
              |
              +--> CPU: ChartView::drawRasterCharts()   (QPainter image blits)
              +--> GPU: GpuChartView::setRasterTiles()   (textured quads)
```

## Notes

- **One seam per job.** Decode (`IChartProductDecoder` / `IChartSource`),
  normalized model (`product_model.hpp`), portrayal (`PortrayalPackage` +
  `PortrayalEngine` -> `SymHit` IR), scene compile, view build (`BuiltCell`),
  and render backend (`IChartRenderer`) are separate stages. The portrayal `SymHit`
  IR (`portrayal_ir.hpp`) is renderer-neutral, so the CPU and GPU backends and any
  plugin overlay consume the same result and never learn how a cell was encoded.

- **Transitional spine.** The normalized `ProductFeatureSet` is produced on the
  built-in path but immediately converted back to `std::vector<Feature>`
  (`cell_source.cpp`), which is what portrayal, caching, build and paint actually
  consume today. The normalized model is seated but not yet load-bearing — see the
  migration phases in `renderer_architecture_plan.md`.

- **GPU pan/zoom is transform-only.** Retained per-cell vertex buffers stay
  resident; a pan/zoom is a camera-uniform update (`GpuChartView::setCamera`), not
  a geometry rebuild. Constant-size S-52 symbology is the one part still rasterised
  by `QPainter` in both backends (served from a blitted apron cache while panning).

- **Backend selection** (`render_backend.hpp`): `Cpu` / `Gpu` / `Auto`. `Auto`
  currently resolves to CPU until the retained GPU backend clears its
  performance/parity gates; a real RHI-availability probe (`GpuChartView::isAvailable`)
  guarantees fallback to the painter so a broken driver can never blank the chart.
```
