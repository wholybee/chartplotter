# Performance: findings and plan (2026 refresh)

This supersedes the earlier pan/zoom-only notes. It was written after re-reading
the current rendering path end-to-end against the reported symptoms:

- **Very high CPU usage even when idle** (no pan/zoom, no user input).
- **Pan and zoom are not smooth.**

The headline conclusion: the single biggest problem is **architectural, not
algorithmic** — there is no separation between the *static* chart and the
*dynamic* overlays, and there is no cached raster of the static chart. Every
data update (a GPS fix, an AIS message, the 1 Hz CPA recompute) re-rasterizes
the **entire** chart from scratch, and every pan frame does the same. Fix that
separation and both symptoms collapse together.

All line references are to `src/chart_view.cpp` and `src/main_window.cpp` unless
noted. Companion doc: `opencpn_optimizations.md` (pipeline/disk-cache ideas).

---

## TL;DR — priorities

| # | Change | Fixes | Effort | Payoff |
|---|--------|-------|--------|--------|
| 1 | **Coalesce repaints** behind one ~15–20 Hz timer; stop calling `update()` directly from data signals | idle CPU | low | high |
| 2 | **Cache the static chart to a `QPixmap`**; composite moving overlays on top each frame | idle CPU **and** pan/zoom | medium | very high |
| 3 | **Hoist per-frame work** — mostly absorbed by Fix 2; text `QFontMetrics` per-label churn *done* | cache-render cost | low | medium |
| 4 | **Zoom-aware geometry simplification** — *done* (`tol = 0.5/ppm`, rebuild on ±1.6× zoom) | pan/zoom, high detail | medium | high |
| 5 | **GPU via `QOpenGLWidget` backing** — only after 1–3; measure first | all rendering | high | medium |
| 6 | **SENC-style on-disk parse cache + "Prepare all charts"** | cold-cell latency | medium | high (cold) |

Do 1 → 3 first. They are small, low-risk, and target the idle-CPU complaint
directly. 2 is the structural win that also makes panning smooth. 4–6 are the
longer plays.

---

## Finding 1 — idle CPU is driven by full-chart repaints on every data update

**This is the cause of "high CPU with no interaction."** With a live GPS/AIS
feed the app is never idle internally: it repaints the whole chart several to
many times per second while sitting at anchor.

Each of these calls `view_->update()`, which schedules a **full** `paintEvent`:

- `NavDataStore::navigationChanged → view_->update()` — fires on every COG / SOG
  / heading / depth message (`main_window.cpp:272`).
- `AisTargetStore::targetUpdated → view_->update()` — fires per target update
  (`main_window.cpp:243`), and `targetExpired` likewise (`:246`).
- `ChartView::setOwnship → update()` on every position fix (`chart_view.cpp:2060`).
- `CpaCalculator::recompute()` runs **every 1 s** (timer) *and* on every
  `ownshipChanged`, looping all targets and calling `setRangeMeters` +
  `setCpaTcpa`, each emitting `targetUpdated` (`cpa_calculator.cpp:34`,
  `ais_target_store.cpp:170/206`).
- `AisTargetStore::tick()` at 1 Hz emits `targetUpdated` for aging targets
  (`ais_target_store.cpp:212`).
- Route store / nav changes also repaint (`main_window.cpp:257,268,269`).

Qt coalesces multiple `update()` calls *within one event-loop turn* into a
single paint, so the per-target emissions inside one `recompute()` collapse to
one repaint — but `recompute` itself fires on every fix, `navigationChanged`
fires independently, and the 1 Hz timers fire on their own turns. Net result:
**N full chart re-rasterizations per second** where N tracks the incoming data
rate, not anything the user did.

And a full repaint is **not cheap** (see Finding 3): it re-sorts every loaded
cell, walks every path with a bbox test, rebuilds the sounding-declutter spatial
hash from scratch, and re-lays-out every text label constructing a
`QFontMetricsF` per label. On a detailed harbour chart that is tens of
milliseconds per frame. A handful of fixes per second is enough to peg a core.

### Fix 1a — one coalescing repaint timer (do this first; ~1 hour)

Replace every direct `view_->update()` driven by a *data* signal with a request
into a single coalescing timer:

```cpp
// ChartView: a repaint governor
void ChartView::requestRepaint() {          // call this from data-driven slots
    if (!repaintPending_) {
        repaintPending_ = true;
        repaintTimer_->start();              // single-shot, ~50–66 ms (15–20 Hz)
    }
}
// repaintTimer_ timeout: repaintPending_ = false; update();
```

Point the nav/AIS/route/ownship signals at `requestRepaint()` instead of
`update()`. This caps data-driven repaints at the timer rate regardless of how
fast NMEA/AIS arrives, and it costs essentially nothing when nothing changes.
Direct `update()` stays for genuinely interactive paths (pan/zoom already have
their own throttling via `interacting_`).

This alone should take idle CPU from "saturated" to "negligible" — but each
repaint is still a full re-rasterization, which Finding 2 removes.

---

## Finding 2 — no static/dynamic layer separation; the static chart is re-rasterized every frame

`paintEvent` (`chart_view.cpp:1726`) rebuilds the **whole** image every call:
`fillRect` the background, draw the basemap, draw raster tiles, draw all ENC
cells (z-sorted), draw soundings/symbols/text, then the *dynamic* overlays
(ownship, AIS, routes, scale bar). The widget is `WA_OpaquePaintEvent` with no
backing cache, so a moving boat icon forces the entire static chart — which has
not changed — to be redrawn from `QPainterPath`s.

This is why **both** symptoms exist: idle data updates and pan frames both pay
the full static cost when only the overlay (or only the camera offset) changed.

### Fix 2a — cache the static chart in a `QPixmap`

Render the static layers (background + basemap + raster + ENC cells +
soundings/symbols/text) **once** into an offscreen `QPixmap chartCache_` sized to
the widget, keyed by a `viewEpoch_` that bumps whenever anything static changes
(pan settle, zoom, cell built/removed, detail level, layer toggles, theme).
`paintEvent` then becomes:

```cpp
if (chartCacheDirty_) renderStaticInto(chartCache_);   // expensive, but rare
p.drawPixmap(0, 0, chartCache_);                        // cheap blit
drawOwnship(p, cam);                                    // dynamic, every frame
drawDynamicOverlays(p, cam);                            // AIS, routes, scale bar
```

Now an AIS/GPS update repaints only the overlays over a cached bitmap — a blit
plus a few dozen vector glyphs — instead of thousands of paths. Combined with
Fix 1a, idle CPU becomes trivial.

### Fix 2b — shifted-bitmap panning (smooth pans)

During a drag the static content does not change, only the camera offset. Blit
`chartCache_` translated by the accumulated pan delta and draw overlays on top;
only re-render the static cache when the gesture settles (the existing
`aaTimer_`/`interacting_` machinery is the natural hook). Newly exposed edges
show the 1.5× keep-area margin already built beyond the viewport, or briefly the
background until settle — same tradeoff already documented for deferred cell
loading. This is OpenCPN's "shifted `pDIB`" trick (`opencpn_optimizations.md`,
borrow #3) and makes panning a blit instead of a full re-raster.

Zoom can't reuse the bitmap directly (scale changes), but a *scaled* blit of the
old cache as a placeholder until the re-render lands keeps zoom visually smooth.

**Caveat:** keep the cache at device-pixel resolution (handle `devicePixelRatio`)
and re-render on resize (`resizeEvent`, `chart_view.cpp:2396`). The pixmap is
~widthxheightx4 bytes — trivial.

---

## Finding 3 — work that ran every frame (mostly absorbed by Fix 2; text hoist done)

This was written before the pixmap cache. **Fix 2 already removed the per-*frame*
aspect**: the cell sort, the sounding declutter, and the text layout all live in
`renderStatic`, which now runs only when the cache is (re)rendered — on settle, a
zoom-rebuild, or a cell arrival — not on idle/overlay frames. So items 1–2 below
no longer run "every frame"; they run once per cache render, which is the right
granularity, and the payoff of hoisting them further is small.

1. **Cell z-order sort** — `std::sort` over the loaded cells, now once per cache
   render over a handful of cells. Negligible, and a cached `order_` would have to
   be kept in sync with `loaded_` across every mutation (z-order bug risk) for a
   microsecond saving — **not worth it; left as is.**

2. **Sounding declutter** — the greedy minimum-gap spatial hash depends on the
   camera and the kept sounding set, so it is inherently per-view; it now runs
   once per cache render rather than per paint. Caching it across same-camera
   re-renders (consecutive cell arrivals) is possible but adds keyed state for a
   modest gain. **Left as is for now.**

3. **Text layout** — *done.* A `QFontMetricsF` was constructed **per label**, with
   `ascent`/`descent`/`height`/`averageCharWidth` re-queried per glyph even though
   they depend only on the font size. These are now computed once per point-size
   change and reused; only `horizontalAdvance` (string-dependent) stays per label.
   At thousands of labels with few distinct sizes this removes nearly all the
   metrics work from the static render. The 4-pass halo was left intact — it's
   what keeps labels legible over busy fill, and reducing it changes appearance.

Net: after Fix 2, Finding 3's substance was the per-label metrics churn, which is
now hoisted. The sort and declutter are correctly per-cache-render and not worth
the sync/state complexity to optimise further.

---

## Finding 4 — vector geometry simplified to the zoom, not the band — **implemented**

*Status: done.* Previously `simplifyToleranceM(band)` keyed the vertex-merge
tolerance to the cell's ENC usage band, not the actual viewing scale, so at a
positive Detail Level a fine-band cell carried harbour-grade vertex density at a
coastal on-screen scale — far more vertices than the screen can resolve.

**What was tried first, and rejected.** The original plan here was to bias the
band tolerance by `pow(4, detailLevel)`. That has a real regression: it keys the
tolerance to the *detail setting* rather than the *on-screen scale*, so in any
region lacking the finest bands (most regions) the finest available chart gets
over-simplified into visibly blocky coastlines whenever detail is raised or you
zoom in at high detail. The coarsening is only correct when the loaded band
happens to equal the detail-biased target band.

**What shipped instead — zoom-aware tolerance.** `dispatchBuild` now sets
`tol = 0.5 / ppm_` — about half a logical pixel in scene metres at the *current
zoom* — exactly mirroring what the basemap already did (`maybeBuildBasemap`).
Geometry is simplified to what the screen resolves regardless of band or detail
level: a fine-band cell pulled in by +detail while zoomed out is simplified as
coarsely as the view warrants, and the same cell zoomed in keeps full detail —
no regression in either direction. `simplifyToleranceM` is gone.

Because tolerance now tracks zoom, the loaded set must re-simplify when the zoom
drifts. `updateVisibleCells` rebuilds the loaded cells when `ppm_` moves past
±1.6×/0.6× of `cellsBuiltPpm_` (the same band the basemap uses), reusing the
existing in-place re-clip path (`storeCell` replaces each cell when its worker
finishes, so nothing blanks) and the pinned parse cache (no GDAL re-read). It
composes with the gesture LOD and the Fix 2 cache: a zoom gesture scales the
cached pixmap, and the rebuild happens once on settle. Detail-level changes no
longer need a tolerance rebuild — they only change which bands load; the cells
that remain keep their (zoom-correct) tolerance.

---

## Answers to the specific questions

### Q1. Should the GPU be used, and how?

Today rendering is pure CPU raster: plain `QApplication` (`main.cpp`), QWidget +
`QPainter`, no `QOpenGLWidget`, no `QSurfaceFormat`, no RHI. So yes, there is
headroom — **but GPU is not the first move.** Do Findings 1–3 first; they remove
most of the CPU load without new failure modes, and on weak marine/embedded GPUs
a poorly-fed GL path can be *slower* than tuned raster.

When you do reach for it, in rough order of cost/benefit:

1. **`QPixmap` caching (Finding 2) is the cheap "GPU-lite" win.** Qt's raster
   pixmap blits are already fast, and on most platforms a cached pixmap can live
   in graphics memory. This captures the majority of the benefit (don't re-raster
   the static scene) with none of the GL porting risk.

2. **Back the widget with `QOpenGLWidget` and keep using `QPainter`.** Qt routes
   `QPainter` through its GL paint engine, so `drawPath`/`drawPixmap`/`drawText`
   keep working with little code change, and compositing the cached chart +
   overlays happens on the GPU. Biggest realistic GPU win for the least rewrite.
   Watch: `drawText` glyph upload, threaded `QImage`→texture handoff, and driver
   quality on target hardware.

3. **Full retained-mode GL (VBOs, pre-tessellated polygons, shader symbols).**
   This is the OpenCPN model and the only way to get true large-chart GPU
   throughput, but it means pre-triangulating area fills, building line-vertex
   buffers, and a symbol-texture atlas pipeline — a major effort
   (`opencpn_optimizations.md`). Only justified if 1–2 plus geometry
   simplification still leave you GPU-bound. Note `sym_atlas` already bakes the
   S-52 sprite sheet, which is the head start for GL symbol batching.

**Recommendation:** ship Findings 1–3 (+2's pixmap cache), measure, and only then
evaluate `QOpenGLWidget` (option 2). Don't start with retained-mode GL.

### Q2. Can vector objects be simplified while zooming?

Yes — that's Finding 4 (zoom/detail-aware build tolerance) plus a cheaper moving
frame:

- Build-time tolerance biased by detail level → fewer vertices per `drawPath`.
- During an active gesture, you may draw an even coarser cached simplification
  (a second, aggressively-simplified `QPainterPath` per cell, built once) and
  swap to the full one on settle — the same idea as dropping antialiasing and
  point overlays mid-gesture, applied to geometry. With Fix 2b (shifted bitmap)
  this may be unnecessary, since pans stop re-rasterizing geometry entirely;
  measure before adding it.

### Q3. Decluttering / reducing objects when zoomed out?

Partly done, with room to push further:

- **Already:** point-LOD gate hides soundings/symbols/text below a zoom
  threshold (`pointLodVisible_`, `updatePointLOD()` `chart_view.cpp:1181`); SCAMIN
  per-feature suppression (`scaminPasses`); greedy sounding spacing
  (`soundingMinSpacing`); coverage-subtraction quilting so only the finest band
  draws per region.
- **Add:** cap how many extra bands the Detail-Level bias can pull in (clamp
  `maxBand ≤ target + N`) so `+2` over a dense harbour can't load an unbounded
  cell pile (`opencpn_optimizations`/`performance` lever 3). Extend the greedy
  min-gap declutter from soundings to **symbols and text** (same spatial hash),
  which are currently only SCAMIN-gated, not spaced. Optionally collapse dense
  same-class symbol clusters into a count badge when zoomed out.
- **Spatial index for path culling:** `drawPaths` bbox-tests every path in every
  cell each frame (`chart_view.cpp:1761–1763`). A per-cell grid / R-tree of path
  bounds lets the paint skip whole cells. Lower priority — only worth it if
  profiling shows the *cull loop* (not `drawPath`) is hot.

### Q4. One-time chart processing cached to disk?

Yes — this is the biggest fix for **cold-cell latency** (first view after launch,
or after LRU eviction), fully specified in `opencpn_optimizations.md` (borrow #1).
Today every cold cell pays a full GDAL ISO-8211 parse + ENC update merge +
Mercator projection on a worker thread; the in-memory `FeatureCache` only makes
*warm* cells free. OpenCPN pays that once per chart edition by serializing a SENC.

**Plan:** serialize the output of `loadCellFeatures()` (the projected
`std::vector<Feature>`) to a compact binary side file per cell, keyed by the
cell's size+mtime (the catalog already tracks both) plus a format version.
`dispatchLoad` becomes "read the side file if fresh (bulk `fread`), else parse via
GDAL and write it." Add a **"Prepare all charts"** action that walks the catalog
through the existing `QThreadPool` with a progress dialog — same UX as OpenCPN's
"Prepare all ENC charts". Deleting the cache directory must always be safe.
Expect cold-view latency to drop from "GDAL parse" to "disk read".

Note this is orthogonal to the idle-CPU and pan-smoothness problems above — it
won't help a chart that's already on screen, but it removes the hitches when new
cells scroll into view.

---

## Interaction-LOD model (still the right hook for gesture work)

Unchanged from before; new optimisations should hang off this rather than
inventing a parallel mechanism.

| Name | Interval | Role |
|------|----------|------|
| `interacting_` | — | true while a pan/zoom gesture is in flight |
| `aaTimer_` | 180 ms single-shot | "gesture settled": clears `interacting_`, repaints, runs deferred cell work |
| `updateTimer_` | 120 ms single-shot | debounced cell-set recompute (`updateVisibleCells`) |

Already in place: antialiasing off during interaction (`paintEvent` line 1738);
point overlays optionally skipped mid-gesture (`hideSymbolsWhilePanning_`); cell
management deferred during interaction and caught up on settle. The proposed
repaint governor (Fix 1a) and static-cache (Fix 2) sit alongside this cleanly:
during `interacting_` you blit/shift the cache; on `aaTimer_` you re-render it.

---

## Where to measure first

Before any refactor, confirm the split with a cheap frame timer
(`QElapsedTimer`) around the blocks in `paintEvent`, and a counter of repaints
per second:

1. **Repaints/sec while idle with a live feed** — should be near 0 after Fix 1a.
   If it's high, a data signal is still bypassing the governor.
2. **`renderStatic` time** vs **overlay time** per frame — confirms Fix 2's
   premise (static dominates) and shows Finding 3's hoisting payoff.
3. Within the static render: the **vector `drawPaths` loop** vs the
   **point-overlay block** (soundings/symbols/text). If vector dominates at
   detail `+2`, prioritise Finding 4; if the cull loop dominates over `drawPath`
   itself, the spatial index (Q3) moves up.

Optimise against numbers, not guesses — the layering refactor (Fix 2) is the one
worth proving with a before/after frame-time capture.
