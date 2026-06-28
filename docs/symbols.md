# ENC Symbology

How the chartplotter draws S-57 chart features as recognisable nautical symbols
without implementing the full S-52 standard.

S-57 is the IHO data format for Electronic Navigational Charts (ENCs). S-52 is
the companion standard that says *how* to draw each feature — the colours,
boundary patterns, lighted-buoy flares, restricted-area glyphs. S-52 is
expensive to license and complex to implement end-to-end. This app takes a
pragmatic shortcut: it borrows the **S-52 lookup tables, colour palette, and
HPGL line/pattern definitions from OpenCPN's `chartsymbols.xml`** and implements
just enough of the S-52 look-up-table (LUP) selection algorithm to render
correct symbols, line styles, area fills, and rotations. The **sprite atlas
those rules point at is generated from the non-GPL IHO S-101 symbol library** —
a drop-in replacement for OpenCPN's GPL atlas (see
[Atlas provenance](#atlas-provenance-generated-from-s-101)).

The result isn't ECDIS-compliant — but visually it matches OpenCPN on the same
charts for the great majority of features. The instruction set now covers
symbols (`SY`), line styles (`LS`), area washes (`AC`), **complex lines (`LC`)**,
**area patterns (`AP`)**, **text labels (`TX`/`TE`)**, and the **conditional
symbology procedures (`CS`)** that reach ENC chart features — see
[S-52 instruction support](#s-52-instruction-support).

```
data/                         build tree                          runtime
+--------------------+   gen   +------------+   load   +--------------+
| chartsymbols.xml   | ------> | symbols.bin| -------> |   SymAtlas   |
| rules: OpenCPN GPL |         | (packed    |          |  LUP engine  |
| 1015 sym defs +    |         |  binary    |          +--------------+
| 2k+ lookups        |         |  ~150 KB)  |                 ^
+--------------------+         +------------+          per-feature query
| rastersymbols-     |    copy                         (objClass, geom,
|   day.png          | --------------------------+     attribute list)
| S-101, non-GPL     |                           |
+--------------------+                           v
                                            +---------+
                                            | Painter |
                                            |  blits  |
                                            +---------+
```

## The two source files

Both live under `data/`, but they no longer share one origin — which is the
whole point of the licensing work in [Atlas provenance](#atlas-provenance-generated-from-s-101):

| File | Role | Origin |
|------|------|--------|
| `data/chartsymbols.xml` | Lookup tables, colour palette, HPGL line/pattern + vector defs, and the `<bitmap>` atlas coordinates. Read at build time only. | OpenCPN (GPL v2); only the `<bitmap>` coordinates are regenerated to match the repacked atlas. |
| `data/rastersymbols-day.png` | Sprite atlas — every symbol pre-rasterised into one PNG. Read at runtime, kept resident as a `QPixmap`. | Generated from the IHO S-101 SVG library. |
| `data/rastersymbols-dusk.png`, `data/rastersymbols-dark.png` | Same atlas at dusk/night colour temperatures. Bundled but not yet switched at runtime. | Same S-101 build, dusk/night palettes. |

The atlas is consumed as **pre-baked tiles**: faster at runtime (one
`drawPixmap` per symbol, no path construction) and the colours are baked in per
scheme. `gen_symbols` still reads the XML's lookup/colour/HPGL content at build
time; only the bitmap coordinates moved when the atlas was repacked.

## Atlas provenance: generated from S-101

The three `rastersymbols-*.png` atlases used to be OpenCPN's own GPL-v2 sprite
sheets, shipped verbatim. They are now **regenerated from the official IHO S-101
SVG symbol library** by a separate build pipeline (the S-57→S-101 project, run
via its `build/build_all.sh`) and dropped into `data/` as a **drop-in
replacement** — `gen_symbols`, `symbols.bin`, `SymAtlas`, the LUP matcher, and
all runtime code are untouched. The driver is licensing: OpenCPN's atlas is
GPL-v2, incompatible with Apple App Store distribution; IHO S-101 is not GPL.

The pipeline reproduces every legacy bitmap name, pivot, and on-screen size, so
the renderer can't tell the difference. Of the **1091 bitmap tiles** (1023
unique names) it emits:

* **837 are rendered from S-101 vector art** — a same-named S-101 SVG, or a
  family *recipe* that recolours/composes S-101 shapes (buoys, beacons, towers,
  topmarks, soundings, lights, daymarks). Each is rasterised per colour scheme
  through the matching `*SvgStyle.css` and scaled to the legacy pixel size.
* **254 are carried over** as the exact legacy pixels — families with no S-101
  equivalent (inland notice marks, app/UI overlay marks, a few edge-case
  dangers) plus the HPGL `<pattern>` tiles.

The atlas stays **1500×1200** with one shared layout across all three schemes,
and `chartsymbols.xml` is edited surgically: only `<bitmap>` `width`/`height`,
`<pivot>`, and `<graphics-location>` change — lookups, colour tables,
line-styles, patterns, and vectors are byte-identical. The recipe step also
corrects places where S-101 reused an S-52 name for a *different* symbol (e.g. a
buoy colour that would otherwise render wrong), so those collisions never reach
the chart.

> **Licensing status (snapshot).** The shipped atlas pixels are now ~77% non-GPL
> (837/1091); the remaining 254 carry-overs are the GPL content still to be
> re-sourced. Separately, **`chartsymbols.xml` itself is still OpenCPN GPL** —
> its lookup rules, colour table, and HPGL vector/line/pattern programs (all
> baked into `symbols.bin`) are a distinct App Store exposure that the atlas
> work does not address.

## Build-time tool: `gen_symbols`

`tools/gen_symbols.cpp` parses the XML and emits a compact binary lookup table
(`symbols.bin`). CMake builds it as a host-side executable and runs it once per
build, with the Qt bin dir prepended to `PATH` so `Qt6Core.dll` is loadable.

The tool's output is reproducible from the same XML — there's no state in the
build dir beyond `symbols.bin`. A typical run prints:

```
gen_symbols: 1015 syms, 1937 lookups, 2608 conds, 113 attrs, 63 colors,
             57 lines(LC), 30 patterns(AP), 96947 str bytes
gen_symbols: wrote symbols.bin (269783 bytes)
```

Unlike earlier generations, the tool no longer pre-resolves a single
`SY`/`LS`/`AC` per lookup or synthesises CS fallbacks. Instead it stores the
**raw instruction string** for every lookup (in a trailing string blob) plus the
full DAY_BRIGHT **colour table** and the HPGL **line-complex** (`LC`) and
**area-pattern** (`AP`) definitions. The runtime parses and *executes* each
instruction — including CS procedures — so multi-symbol output, text, complex
lines, patterns, and conditional symbology all become possible.

## Binary format: `symbols.bin`

A single packed binary, little-endian, magic `"SYM\x06"`. Layout:

```
Header (36 bytes)
  magic[4]    "SYM\x06"
  symCount    number of SymRecord
  lupCount    number of LupRecord
  condCount   number of CondRecord (condition pool)
  attrCount   number of AttrRecord (relevant-attribute acronyms)
  colorCount  number of ColorRecord (DAY_BRIGHT colour table)
  lcCount     number of LcDefRecord (LC line-complex HPGL defs)
  apCount     number of ApDefRecord (AP area-pattern defs)
  strBytes    size of the trailing string blob

SymRecord   x symCount    (36 bytes each) -- atlas tiles
LupRecord   x lupCount    (24 bytes each) -- one lookup (+ instruction ref)
CondRecord  x condCount   (32 bytes each) -- pooled conditions
AttrRecord  x attrCount   (8  bytes each) -- attribute acronyms
ColorRecord x colorCount  (12 bytes each) -- token -> RGBA
LcDefRecord x lcCount     (48 bytes each) -- LC() HPGL line definitions
ApDefRecord x apCount     (64 bytes each) -- AP() HPGL/raster pattern defs
string blob x strBytes                    -- instruction + HPGL text
```

Key per-record shapes:

```
SymRecord    name[24], atlas_x, atlas_y, width, height, pivot_x, pivot_y
LupRecord    objClass[8], geomType, dispCat, nConds, condStart,
             instrOff, instrLen          (instr in the string blob)
CondRecord   attr[8], value[24]
ColorRecord  token[8], r, g, b, a
LcDefRecord  name[24], rgba, vecW/H, pivot, origin, hpglOff, hpglLen
ApDefRecord  name[24], rgba, vecW/H, pivot, origin, minDist/maxDist,
             fillType, spacing, hasBitmap, bmp{X,Y,W,H}, hpglOff, hpglLen
```

`LupRecord`s are grouped: every lookup for a `(objClass, geomType)` is
contiguous, which lets the runtime build a single hash from class+geom to a
`[first, count)` range with no per-lookup index entry.

Conditions are pooled and referenced by `(condStart, nConds)`. All variable-
length text — the lookup instructions and the HPGL programs for `LC`/`AP` — lives
in the trailing **string blob**, referenced by `(offset, length)`. The colour
table is the whole DAY_BRIGHT palette so the runtime can resolve any `LS`/`AC`/
`TX`/`CS` colour token by name. Raster `AP` patterns (no HPGL form, e.g. the
foul-ground hatch) carry an atlas tile rect instead of an HPGL reference.

## Runtime: `SymAtlas` + matcher

`src/sym_atlas.{hpp,cpp}` owns everything related to symbols at runtime.

**Lifecycle:**

1. `ChartView` constructs and calls `symAtlas_.load("symbols.bin",
   "rastersymbols-day.png")` — synchronous, on the GUI thread, exactly once.
2. The PNG goes into a `QPixmap` (GPU-backed on the Qt raster/GL backends).
3. The binary is read into immutable vectors and a `QHash`.
4. After load the data is **never mutated** — every read is safe to call from
   any worker thread.
5. `ChartView` hands the relevant-attribute set to `chart::setSymbologyAttrs`,
   so the loader knows exactly which S-57 attributes it must read into each
   feature.

**Thread model:**

```
GUI thread                          worker thread (cell load)
+---------------+                   +-------------------------+
| SymAtlas      |                   | chart_loader            |
|  load() once  |   <-- queries --  |  reads f.attrs from GDAL|
|  immutable    |                   |  symbolForFeature(...)  |
+---------------+                   |  → SymHit               |
        ^                           +-------------------------+
        | drawPixmap (paintEvent)            |
        |                                    v
+---------------+                   +-------------------------+
| QPainter blit |                   | BuiltCell (built path,  |
+---------------+                   |  symbols, fills, lines) |
                                    +-------------------------+
```

The matcher (`symbolForFeature`) takes:

```cpp
SymHit symbolForFeature(const QByteArray& objClass,
                        SymGeom geom,              // Point, Line, Area
                        const AttrList& attrs);    // (acronym, value) pairs
```

and returns the *result of executing the chosen lookup's instruction*:

```cpp
struct SymHit {
    std::vector<SymStamp> symbols;     // zero or more SY() stamps (+ rotation)
    bool          hasLine;  SymLineStyle line;   // LS() pen / boundary
    bool          hasFill;  SymFillStyle fill;   // AC() colour wash
    int           lcIndex;             // LC() complex line, or -1
    int           apIndex;             // AP() area pattern, or -1
    std::vector<SymText>  texts;       // TX()/TE() labels (text + placement)
};
```

A single hit can carry **any combination** of these — a TSS-zone area
(`TSEZNE`) gets just a fill; a TSS-lane area gets two rotated arrows, a dashed
line, and a restriction glyph from `CS(RESTRN01)`; a light gets a coloured flare
plus a `Fl(2)R 10s15M` character label; a cable-area boundary gets a complex
line. The matcher selects the best lookup, then `execInstruction` parses its raw
instruction string and fills the `SymHit` (running any `CS` procedure inline).

### The best-match selection algorithm

For each `(objClass, geomType)`, the binary holds one or more `LupRecord`s.
Each lookup has zero or more attribute conditions. The runtime picks one:

1. Walk every lookup for `(objClass, geomType)`.
2. A lookup **matches** when every one of its conditions is satisfied by the
   feature's attributes.
3. Among matched lookups, the **most-specific one wins** (highest `nConds`).
4. If no conditional lookup matches, the class's no-condition default wins.
5. If there's no default either, return `kNoSymbol` (renders as a dot).

A condition can be:

| Form | Encoding | Meaning |
|------|----------|---------|
| `BOYSHP4` | `attr="BOYSHP", value="4"` | The attribute equals this value. |
| `COLOUR3,4,3` | `attr="COLOUR", value="3,4,3"` | The attribute equals this comma-joined multi-value. |
| `ORIENT ` (acronym alone) | `attr="ORIENT", value="*"` | The attribute is *present* on the feature, any value. |

The `"*"` presence sentinel is critical. The XML uses it for lookups like
`<attrib-code>ORIENT </attrib-code>` (note the trailing space — acronym only,
no value). Without it, every TSS-lane arrow was filtered out because nothing
"equals empty string"; with it, those lookups match exactly when the feature
has an ORIENT attribute.

## S-52 instruction support

Each lookup's `<instruction>` is an S-52 mini-language: a semicolon-separated
list of drawing primitives. The whole instruction is stored verbatim and
**executed at runtime** by `SymAtlas::execInstruction`, which fills a `SymHit`.

| Instruction | Meaning | Support |
|-------------|---------|---------|
| `SY(NAME)` | Stamp a symbol from the atlas at the feature point/centroid. | ✅ |
| `SY(NAME, ORIENT)` | …rotated by the feature's ORIENT attribute. | ✅ |
| `SY(NAME, <numeric>)` | …fixed rotation. | ✅ |
| `SY(NAME, OBJNAM)` | …with text-label rotation. | No rotation (drawn upright). |
| `LS(pattern, width, colour-token)` | Solid/dashed/dotted line. | ✅ |
| `LC(line-name)` | "Line complex" — a motif stamped along the path. | ✅ (HPGL) |
| `AC(colour-token[, τ])` | Area-colour wash, optional S-52 transparency. | ✅ |
| `AP(pattern-name)` | Area-pattern fill (hatching, stipple, symbol tiles). | ✅ (HPGL + raster) |
| `TX(attr, …)` | Text label from an attribute. | ✅ |
| `TE('fmt', attr, …)` | Formatted text label. | ✅ |
| `CS(procedure)` | Conditional symbology procedure. | ✅ for the ENC chart procedures (see below) |

Multiple `SY()` in one instruction all stamp (e.g. a TSS lane area draws a
rotated lane arrow *and* a route point); the first `LS`/`AC`/`LC`/`AP` of each
kind wins. Colour tokens are resolved at runtime from the baked DAY_BRIGHT
table.

### Rotation (`SY(..., ORIENT)`)

When a `SY()`'s second argument is `ORIENT`, the runtime reads the feature's
`ORIENT` attribute (degrees CW from true north) into the stamp's rotation; a
numeric second argument is used as a fixed rotation. The painter draws via:

```cpp
t.translate(d.x(), d.y());
t.rotate(rotationDeg);
t.translate(-pivot.x(), -pivot.y());
p.drawPixmap(QPointF(0,0), atlas, src);
```

Our scene is north-up Mercator with Y flipped, so `QPainter::rotate(orient)`
maps an "ORIENT 240°" feature to a symbol whose own "up" points at compass
bearing 240° — directly matching S-57 conventions.

### Line styles (`LS()`)

`LS(pattern, width, colour-token)` resolves the colour token against the
**DAY_BRIGHT** table and sets `SymHit::line`:

| Pattern | Maps to |
|---------|---------|
| `SOLD` | `Qt::SolidLine` |
| `DASH` | `Qt::DashLine` |
| `DOTT` | `Qt::DotLine` |

For a **Line** feature it colours the geometry; for an **Area** feature it
colours the boundary outline.

### Complex lines (`LC()`)

`LC(line-name)` references one of the HPGL **line-style** definitions baked from
chartsymbols.xml's `<line-styles>` (cable squiggles, anchorage-boundary ticks,
restricted-area boundaries, …). At load time each definition's HPGL program is
compiled into vector strokes; at paint time `SymAtlas::drawLineComplex` walks the
feature's device-space polyline and stamps the motif end-to-end, rotated to the
local tangent, at constant on-screen size. A faint guide line is kept underneath
so a sparse motif still reads as a boundary. (Two referenced names —
`ARCSLN01`, `NEWOBJ01` — have no `<line-style>` and render as the guide only.)

### Area fills (`AC()`)

`AC(colour-token[, τ])` sets `SymHit::fill`. The S-52 transparency factor
(τ in 0..4) becomes an 8-bit alpha:

```
alpha = 255 * (1 - τ / 4)
τ = 0 → 255 (opaque)   τ = 3 → 63 (the common "soft tint")   τ = 4 → 0
```

When the matched lookup has a fill (or an `AP` pattern), the `OtherArea` branch
switches from polyline clipping to ring clipping and gives the `BuiltPath` a fill
brush and outline pen.

### Area patterns (`AP()`)

`AP(pattern-name)` references an HPGL or raster **pattern** baked from
`<patterns>`. At load time each pattern becomes a small tile `QImage` — the HPGL
motif rendered to its bounding box, or (for raster-only patterns such as the
foul-ground/cross hatches) a tile copied straight out of the atlas. At paint time
`SymAtlas::fillAreaPattern` clips to the area and tiles the motif in device space
at constant on-screen size, honouring the pattern's `<distance>` spacing and
staggering alternate rows for staggered (`filltype S`) patterns. The `AC` wash (if
any) draws underneath. 30 patterns are available — airport areas, dredged-quality
hatches, marine-farm/fish-haven stipples, the under-safety-contour diamond, etc.

### Text labels (`TX()` / `TE()`)

`TX(attr, hjust, vjust, space, 'chars', xoffs, yoffs, colour, disp)` draws an
attribute's value; `TE('fmt', 'attr,…', …)` formats one or more attribute values
through a C-style format string (`%s`, `%d`, `%4.1lf`, …). The runtime resolves
the source attribute(s), applies the format, and records the text with its S-52
placement (`hjust`/`vjust`/offsets), colour, and body size. A `TE` whose
referenced attribute is absent is suppressed, matching S-52. At paint time labels
draw at constant on-screen size with a light halo, justified per the instruction.
This surfaces light characters (`Fl(2)R 10s15M`), vertical clearances
(`clr 12.5`), berth numbers, object names, and more. The bare-`OBJNAM` fallback
still labels any named object whose instruction carried no `TX`/`TE`.

### Conditional symbology procedures (`CS()`)

`CS(proc)` runs a procedure in C++ (`SymAtlas::runCS`) that inspects the
feature's attributes and appends symbols / text / line styles to the `SymHit`
— the same machinery OpenCPN's presentation library uses, ported to the data
available per feature. The procedures that actually reach the symbol engine for
ENC chart objects are implemented:

| Procedure | Classes | What it does |
|-----------|---------|--------------|
| `LIGHTS05` | `LIGHTS` | Coloured flare by `COLOUR` + a compact light-character label from `LITCHR`/`SIGGRP`/`SIGPER`/`VALNMR`. |
| `TOPMAR01` | `TOPMAR`, `DAYMAR` | Topmark/daymark symbol by `TOPSHP` (incl. cardinal marks). |
| `OBSTRN04` | `OBSTRN`, `UWTROC` | Obstruction/rock glyph by `WATLEV`/`VALSOU` + sounding label. |
| `WRECKS02` | `WRECKS` | Dangerous vs non-dangerous wreck by depth/`CATWRK`/`WATLEV`. |
| `RESTRN01` | TSS/area classes | Restriction glyph from the `RESTRN` value list. |
| `RESARE01/02` | `RESARE`, `ACHARE` | Dashed boundary + restriction/anchorage glyph. |
| `SYMINS01` | `NEWOBJ` | Question-mark glyph (+ dashed boundary for areas). |

Procedures for depth areas, depth contours, soundings, coastlines and data
coverage (`DEPARE01/02`, `DEPCNT02`, `SOUNDG02`, `SLCONS03`, `QUAPOS01`,
`DATCVR01`) are intentionally **not** routed through `CS`: the chart loader
classifies those features into kinds (`DepthArea`, `DepthContour`, `Sounding`,
`Coastline`, skipped `M_*`) that the chartplotter renders with its own depth- and
unit-aware code, so they never reach `runCS`. Route/own-ship procedures
(`LEGLIN`, `PASTRK`, `OWNSHP`, `VESSEL`, …) are overlay features handled
elsewhere. Any unrecognised `CS` is a no-op; the rest of the instruction (direct
`SY`/`LS`/`AC`/text) still renders.

#### Known CS gaps

- **Light sectors.** `LIGHTS05` draws the flare and character label but not the
  sector arcs/legs (these need per-feature geometry beyond the point).
- **Isolated-danger isolation test.** `OBSTRN04`/`WRECKS02` flag a danger by
  depth vs a fixed safety depth (20 m) rather than testing whether the danger is
  surrounded by deeper water.
- **Floating vs rigid topmarks.** `TOPMAR01` uses the buoy topmark variants
  (it can't resolve the parent object's class without S-57 relationships).

## Attributes: what the loader reads

The chart loader reads a **fixed set** of S-57 attributes from each feature —
the union of those referenced by any lookup condition, any `TX`/`TE` text
source, and any implemented `CS` procedure (e.g. `COLOUR`, `LITCHR`, `VALSOU`,
`WATLEV`, `TOPSHP`, `RESTRN`, `CATWRK`, …). The set is baked into `symbols.bin`
and exposed via `SymAtlas::relevantAttrs()`, which `ChartView` hands to
`chart::setSymbologyAttrs()` at startup:

```cpp
if (symAtlas_.isLoaded())
    chart::setSymbologyAttrs(symAtlas_.relevantAttrs());
```

A typical build pulls in ~80 attributes: `BOYSHP`, `CATLAM`, `COLOUR`,
`COLPAT`, `CATCAR`, `CATSPM`, `WATLEV`, `CATWRK`, `ORIENT`, `TRAFIC`,
`RESTRN`, etc. The loader resolves each layer's field indices **once per
layer** and reads only those fields per feature.

### Field-type normalisation

A subtle GDAL pitfall: the S-57 driver returns multi-valued S-57 attributes
(`COLOUR`, `COLPAT`, `NATSUR`, …) as `OFTStringList`. Calling
`OGR_F_GetFieldAsString` on a StringList returns a *formatted* string like
`"(1:4)"` — not the bare value `"4"` that the lookup condition expects.

`normalizedFieldValue()` in `chart_loader.cpp` handles every field type
explicitly:

```
OFTInteger          → "4"
OFTInteger64        → "4"
OFTReal             → "4"          (cast to long long)
OFTIntegerList      → "4" or "1,4" (joined)
OFTRealList         → "4" or "1,4"
OFTStringList       → "4" or "1,4" (walked element-by-element)
default / OFTString → strip spaces from OGR_F_GetFieldAsString
```

Without the StringList case, **every COLOUR-conditioned lookup silently
fails** — green buoys default to the uncoloured BOYGEN03 outline, green
lights to magenta. The visual symptom is "buoys look unfilled and lights are
all wrong colour"; the root cause is GDAL handing back a list-formatted
string the matcher can't parse.

## Table selection

S-52 ships its symbology in multiple tables per geometry type:

| Geometry | Tables | Preferred |
|----------|--------|-----------|
| Point | `Paper`, `Simplified` | `Paper` (realistic buoy/beacon shapes) |
| Line | `Lines` | `Lines` |
| Area | `Plain`, `Symbolized` | `Symbolized` (modern S-52 default) |

`preferredTable()` in `gen_symbols.cpp` picks the preferred one when present
and falls back to the alternate if not. Only lookups in the chosen table for
each class are kept; the others are dropped from the binary.

## Colour resolution

The `<color-tables>` section of `chartsymbols.xml` defines named colour
tokens (`CHGRD`, `TRFCD`, `LANDA`, `CHMGF`, …) in four palettes:

| Table | When |
|-------|------|
| `DAY_BRIGHT` | Daylight, normal contrast |
| `DAY_BLACKBACK` | Daylight, inverted background |
| `DAY_WHITEBACK` | Daylight, white background |
| `DUSK`, `NIGHT` | Reduced brightness for night vision |

`gen_symbols` parses the **`DAY_BRIGHT`** table only. The token-to-RGB map is
held in memory during the build and used to resolve every colour reference
in `LS()` and `AC()` instructions. The RGB is baked into the binary —
runtime knows nothing about the tokens.

Switching to dusk/night would mean either baking the alternate palettes into
the binary as parallel tables, or keeping the token names in the records and
resolving at draw time. Neither is wired up today.

## The rendering pipeline

How a feature becomes pixels:

```
1. chart_loader reads the ENC cell:
     - feature kind from layer name (DEPARE → DepthArea, LIGHTS → Point, ...)
     - objClass copied for symbol-bearing kinds (Point, OtherArea, OtherLine)
     - relevant S-57 attributes read into f.attrs

2. chart_view::buildCell() runs on a worker thread:
     - For each feature, query atlas->symbolForFeature(objClass, geom, attrs)
     - Apply the SymHit:
         hit.symIdx        → BuiltSymbol at point or centroid
         hit.rotationDeg   → on BuiltSymbol
         hit.line          → BuiltPath pen colour/width/style
         hit.fill          → BuiltPath brush colour (and switch to ring clip)
     - Sort BuiltPaths by z (areas under, lines over, contour-aware)

3. chart_view::paintEvent() on the GUI thread:
     - Walk BuiltPath, set pen+brush, drawPath
     - Walk BuiltSymbol, atlas.draw(p, idx, screenPoint, rotation)
         (one drawPixmap with the source rect → cheap GPU blit)
```

## Source layout

```
data/
  chartsymbols.xml          OpenCPN GPL rules + regenerated bitmap coords (build time only)
  rastersymbols-day.png     atlas — generated from S-101, non-GPL (runtime)
  rastersymbols-dusk.png
  rastersymbols-dark.png

tools/
  gen_symbols.cpp           build-time tool: XML → symbols.bin

src/
  sym_atlas.hpp/.cpp        runtime atlas + LUP matcher + instruction executor
                            (SY/LS/AC/LC/AP/TX/TE) + CS procedures + HPGL renderer
  chart_loader.hpp/.cpp     S-57 reader; normalizedFieldValue()
  chart_view.hpp/.cpp       buildCell() applies SymHit; paints AP/LC + text

CMakeLists.txt              gen_symbols target, custom command,
                            data-file copy POST_BUILD
```

## Known limitations

| Gap | Effect | Notes |
|-----|--------|-------|
| Light sectors | `CS(LIGHTS05)` draws the flare + character label but not the sector arcs/legs. | Needs per-feature geometry beyond the point. |
| Isolated-danger isolation test | `OBSTRN04`/`WRECKS02` flag a danger by depth vs a fixed 20 m safety depth, not by surrounding depth. | Over- or under-flags a few dangers. |
| Floating vs rigid topmarks | `TOPMAR01` always uses the buoy topmark variants. | Parent-object class needs S-57 relationships. |
| `LC`/`AP` motif scale | HPGL motifs render at a fixed nominal pixel scale (≈ atlas-bitmap size); `AP` patterns don't follow the symbol-size slider. | Visually close; not pixel-exact to OpenCPN. |
| Complex lines absent for 2 names | `LC(ARCSLN01)` / `LC(NEWOBJ01)` have no `<line-style>` def. | Render as the faint guide line only. |
| Day-mode palette only | Atlas PNGs for dusk/night are bundled but colours resolve from `DAY_BRIGHT`. | The colour table is now baked whole, so a runtime palette switch is a smaller step than before. |
| Multi-colour attribute conditions are exact-string | A buoy with `COLOUR=1,4` matches a `COLOUR=1,4` rule but not a `COLOUR=1` rule. | Matches the XML's encoding; the same way OpenCPN's tables work. |

None of these are blockers for general navigation viewing — the app reads real
ENC charts and renders the great majority of features recognisably, now
including complex lines, area patterns, text labels, and the conditional
symbology for lights, topmarks, obstructions, wrecks and restricted areas.
