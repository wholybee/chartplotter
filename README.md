# Marine Chart Viewer (Qt 6)

A streamlined ENC (S-57) chart viewer built with **Qt 6** and **GDAL**. Point it
at the root of a directory tree containing hundreds of ENC cells; it catalogs
them, then loads only the cells visible in the current view, on background
threads, as you pan and zoom.

**CM93** (C-Map CM93 v2) vector charts are also supported, via a separate
dynamically-loaded plugin. Because its decoding derives from OpenCPN, that plugin
is **GPL-2.0** and is maintained in its **own repository** (`chartplotter-cm93`),
built against the plugin-SDK headers in `src/` (`chart\_source.hpp` et al.); this
app stays LGPL-2.1 and links none of it. Build that plugin and drop its
`chartplotter\_cm93\_plugin.dll` into `plugins/` (with `Qt6Concurrent.dll` next to
the exe), then point the app at a CM93 dataset root (the folder with
`CM93OBJ.DIC`). See [docs/cm93.md](docs/cm93.md).

## 

## Installing

The latest release build is here:
https://github.com/wholybee/chartplotter/releases

### 

### You will need to download charts. NOAA ENC charts can be downloaded from

https://charts.noaa.gov/ENCs/ENCs.shtml
I recommend just downloading the whole set "ALL" but if space is a concern you can browse and download only what you need.

MBTiles charts are available from multiple sources, but Bruce Balan's Chart Locker is has a great selection:
https://chartlocker.brucebalan.com/

### 

### You will need to download the GSHHG Basemap

https://www.soest.hawaii.edu/pwessel/gshhg/
Scroll down near the bottom, the zip file you need is the ESRI shapefiles



### Windows

Run HMV.Chartplotter-x.x.x-win64.exe installer from the latest release build.

### 

### rpi/Linux

HMV Chartplotter requires qt6 version 6.8 or newer. 6.8 is the version that ships with Debian Trixie. This means that you will have troubles getting it to work with Bookworm.

#### 

#### First, install the dependencies:

sudo apt update
sudo apt upgrade
sudo apt install qt6-base-dev
sudo apt install libgdal36
sudo apt install qt6-websockets-dev
sudo apt install qt6-base-dev-tools

#### Extract the archive and run the application:

tar -xf wholybee-chartplotter-v0.5.4-debian-trixie-arm64.tar.gz
cd wholybee-chartplotter-v0.5.4-debian-trixie-arm64
./hmvchartplotter

### 

### MacOS

I have compiled on MacOS, and it does work. However, there is minimal testing and no automatic build. It will be supported in the future.

## 

## First Run

When opened for the first time, you will need to point it at your unzipped charts.

#### 

#### Basemap Folder

Click the Gear, scroll to Basemap folder, and select the folder where the GSHHG file was unzipped.

#### 

#### Charts

Click the Gear, scroll to chart sets. Add a set of charts for each set you wish to create. At a minimum, you probably want all your NOAA ENC charts in one set, and MBTiles in probably several sets based on area/satellite/Chart.

## 

## How it works

### 1\. Catalog the tree (cheap, cached)

When you open a folder, a background scan walks the tree for ENC base cells
(`\*.000`) and records, for each cell:

* its **footprint** (bounding box), read cheaply from the small `M\_COVR`
coverage layer rather than the full geometry, and
* its **usage band** (navigational purpose: 1 = overview … 6 = berthing), which
comes for free from the cell name — the band digit is the 3rd character of an
ENC filename (e.g. `US`**`5`**`FL14M.000` is a harbour cell).

Footprints are cached to disk (keyed by file path + size + modified-time, one
cache file per root), so subsequent launches skip re-reading the cells.

### 2\. Select by viewport + zoom (with gap-fill quilting)

On every pan/zoom (debounced), the view:

* computes the visible world rectangle and the zoom-appropriate *target* band,
* selects every available band from overview up to that target (`1..maxBand`)
whose footprint intersects the viewport, and
* draws them **band-major**: coarser bands underneath, finer bands on top. A
finer cell's opaque area fills occlude the coarser cell within its footprint,
while anywhere the finer band has no coverage, the next coarser available band
shows through. That is the gap fill — missing bands are simply skipped, so a
gap is filled by whatever the next *available* coarser band is.

If an area has no coverage at or below the target band (only finer data exists),
it falls back to the coarsest band finer than the target so the screen isn't
blank.

### 3\. Load asynchronously

Newly-visible cells are loaded on a `QThreadPool` — each worker opens its own
GDAL handle (the thread-safe usage pattern) and returns parsed geometry, which
the UI thread turns into scene items. Cells that scroll well outside the view
are unloaded to bound memory (their parse stays in the cache; see below). A
hysteresis margin — load just beyond the viewport edge, unload only well past it
— keeps panning from thrashing; the exact margins are described in §4.

Rendering itself is unchanged from the single-folder version: `QGraphicsView`
with one item per feature, cosmetic pens for constant line width, depth-shaded
fills, and sounding labels that appear when zoomed in.

### 4\. Cache parsed cells (LRU) and clip per region

Two layers keep panning and zooming fast:

**In-memory LRU cache** (`FeatureCache`). Parsing a cell — GDAL open, S-57 layer
walk, projection — is the expensive step, and its output is just plain vectors
of points. We keep that parsed result in an LRU keyed by file path, so revisiting
a cell that scrolled off rebuilds its scene items straight from memory with no
disk or GDAL round-trip. The cache is bounded by a soft byte budget and entry
count (256 MB / 256 cells by default); when a load pushes it over, the
least-recently-used cells are evicted. Cells currently on screen are *pinned* and
never evicted, so a tight budget can never drop visible geometry — it just holds
the live set and trims everything else.

**Per-region clipping** (`geom\_clip`). The cached value is the cell's *full*
parse, independent of any viewport. Scene items are built by clipping that parse
to a region a little larger than the view (Sutherland–Hodgman for area rings,
Cohen–Sutherland for contour/coastline polylines, a rect test for soundings and
points). This matters most for gap-fill: a coarse cell that contributes only a
sliver in a gap would otherwise drag a basin-spanning polygon into the scene, and
Qt would traverse and rasterize all of it every frame. Clipped, it carries only
roughly screen-sized geometry, so per-frame cost stays low. Caching the full
parse (rather than clipped output) means a later pan re-clips the same cached
cell to a new region for free — no reload.

The clip region is the same box used for unload hysteresis, and it is always
larger than the visible viewport, so the straight edges clipping introduces fall
off-screen. Concretely, the load/re-clip trigger sits half a viewport-width
beyond each edge while the clip (and unload) box sits one and a half
viewport-widths beyond — a full viewport-width of margin. A cell is re-clipped
only once the view has moved far enough that its stored clip box no longer covers
that inner trigger box, which is still a full viewport-width short of the old clip
edge. So the visible area never reaches a clip boundary and **no blank slivers
ever appear**.

## Controls

* **Drag** — pan. **Scroll wheel** — zoom (centred on cursor).
* **Fit** — frame the whole catalog (all cells, not just loaded ones).
* Status bar: root folder + scan summary (left), band / cells shown (middle),
cursor lat/long (right).

## Building

Requires Qt 6 (Widgets **and Concurrent**, both part of Qt Base), GDAL with the
S-57 driver, CMake ≥ 3.16, C++17.

### Linux

```bash
sudo apt install cmake g++ qt6-base-dev libgdal-dev
cmake -S . -B build \&\& cmake --build build
./build/hmvchartplotter
```

### Windows

See `BUILDING_WINDOWS.md` for the Visual Studio (MSVC) path. Remember `GDAL_DATA`
must point at GDAL's data folder so the S-57 driver finds `s57objectclasses.csv`
/ `s57attributes.csv`.

### macOS

The steps below assume a **clean Mac** with no developer tools installed yet.

1. **Command Line Tools** (clang, git, make):

   ```bash
   xcode-select --install
   ```

   This is enough to build from the command line. To open the project in the
   Xcode IDE (optional, see below), also install the full **Xcode** from the App
   Store.

2. **Homebrew** — the package manager ([brew.sh](https://brew.sh)):

   ```bash
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

   Follow its final instructions to add `brew` to your `PATH`.

3. **Dependencies** — Qt 6, GDAL, the build tools, and `dylibbundler` (used to
   make the `.app` self-contained; see below):

   ```bash
   brew install cmake ninja qt gdal dylibbundler
   ```

4. **Configure and build:**

   ```bash
   cmake -S . -B build/macos -G Ninja \
     -DCMAKE_PREFIX_PATH="$(brew --prefix qt)" \
     -DCMAKE_BUILD_TYPE=Release
   cmake --build build/macos
   open build/macos/hmvchartplotter.app
   ```

GDAL's data files and the Qt/GDAL libraries are bundled into the `.app`
automatically, so the result runs on a Mac that has neither Qt nor Homebrew
installed.

#### Building in Xcode

CMake can generate an Xcode project (this needs the full **Xcode**, not just the
Command Line Tools):

```bash
cmake -S . -B build/xcode -G Xcode -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
open build/xcode/marine_chart_viewer_qt.xcodeproj
```

It is a CMake-generated project — edit `CMakeLists.txt` (not the project) and
re-run CMake. Like the Visual Studio generator it is multi-config: pick
Debug/Release in the scheme. Build the **macdeploy** target (or `ALL_BUILD`) to
produce the fully bundled app — the plotter's own plugins are not dependencies of
the `chartviewer` target, so building only that scheme skips them.

#### Self-contained bundle

On macOS the build runs a deploy step (`cmake/macdeploy.cmake`) that copies the Qt
frameworks/plugins (via `macdeployqt`) and GDAL's full dependency closure — GEOS,
PROJ, SQLite, … (via `dylibbundler`) — into the `.app` and rewrites their load
paths, so it launches with neither Qt nor Homebrew present. It runs automatically
as part of a normal build. If `dylibbundler` is not installed the build still
succeeds, but the `.app` will depend on Homebrew's libraries at runtime. Turn the
step off with `-DCHARTPLOTTER_MACOS_DEPLOY=OFF` for a faster inner-loop build that
runs against the libraries in place.

## Test data

Free ENC cells for US waters come from NOAA ("NOAA ENC direct download"). Unzip
the exchange set(s) anywhere under one root and point the app at that root.

## Layout

```
src/projection.hpp     Mercator <-> lon/lat helpers
src/chart\_loader.\*     chart:: free functions — per-cell load + cheap extent (GDAL)
src/geom\_clip.hpp      pure polygon/polyline/point clipping math (no Qt/GDAL)
src/feature\_cache.hpp  FeatureCache — LRU of parsed cells, keyed by path
src/chart\_catalog.\*    ChartCatalog — async tree scan, footprints, band, disk cache
src/chart\_view.\*       QGraphicsView — viewport/zoom selection, async load/unload
src/main\_window.\*      QMainWindow — toolbar, status bar, folder dialog, QSettings
src/main.cpp           QApplication entry point
```

`geom\_clip.hpp` and `feature\_cache.hpp` are deliberately free of Qt and GDAL so
they can be unit-tested in isolation — see `test\_clip.cpp`.

## Limitations / next steps

* **No load cancellation.** In-flight loads for cells that scrolled away aren't
cancelled; their results are simply discarded on arrival (but still cached, so
the work isn't wasted if the cell is revisited).
* First scan of a very large tree opens every cell once to read its footprint
(then caches it). Subsequent launches are instant.
* Symbology is approximate, not S-52, and not for navigation. ENC update files
(`.001`, …) are not applied.

