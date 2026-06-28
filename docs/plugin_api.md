# Plugin API

The boundary between the core application and plugins (Milestone 3 in
`ProjectSpec.md`). Plugins extend the app — communication, instruments, AIS,
routing, overlays — without touching core internals. They publish data, subscribe
to data, contribute UI, and draw overlays through controlled APIs; the core owns
the shared data models, the chart canvas, settings, and object lifetimes.

The same `IPlugin` / `ICoreApi` interfaces serve **both** built-in (in-process)
plugins and **dynamically loaded** shared libraries (`*.dll` / `*.so` /
`*.dylib`), which are indistinguishable to the rest of the host. The contract was
shaken out with built-ins first, before settling the ABI; dynamic loading,
versioning, and a platform loader are now in place (see
[Dynamic loading](#dynamic-loading)).

The **NMEA 0183** connection is itself a plugin (`Nmea0183Plugin`) — a real data
source built entirely on these interfaces (data-source registration, settings
page, persisted settings, a status dot, and its raw-data debug window), which is
the best validation that the API is sufficient for non-toy use. The **GPX**,
**Signal K**, **WMM**, and **Instruments** plugins ship as runtime-loaded DLLs.

```
+-----------+   initialize(core)   +-----------+   bridges to   +---------------+
|  plugin   | <------------------- | PluginMgr |                | core objects  |
| (IPlugin) |                      |           |   ICoreApi     | NavDataStore  |
|           | --- publish/draw --> |  CoreApi  | -------------> | ChartView     |
|           | <-- subscribe ------ |           |                | SideMenu      |
+-----------+                      +-----------+                +---------------+
```

## Lifecycle

```cpp
class IPlugin {
    virtual QString name() const = 0;
    virtual QString version() const = 0;
    virtual void initialize(ICoreApi* core) = 0;   // register contributions
    virtual void shutdown() = 0;                    // release them
};
```

`PluginManager` owns the plugins and drives them against one `ICoreApi`:

```cpp
PluginManager mgr(coreApi);
mgr.add(std::make_unique<Nmea0183Plugin>());          // built-in, in-process
mgr.loadFromDirectory(appDir + "/plugins");           // dynamic DLL/SO discovery
mgr.initializeAll();                                  // initialize(core) each, once
// ... app runs ...
mgr.shutdownAll();                                    // shutdown() each (idempotent)
```

A plugin does all its wiring in `initialize()` — grabs core handles, registers
menu items / overlays / data sources — and undoes anything that outlives it in
`shutdown()` (e.g. removing an overlay it registered).

## Core services: `ICoreApi`

The single, stable surface handed to a plugin. The core implements it (`CoreApi`)
and routes calls to the real objects.

```cpp
class ICoreApi {
    // Navigation data
    INavDataPublisher*  navPublisher();        // publish updates
    const NavDataStore* navData() const;       // read state; connect ownshipChanged()

    // AIS targets
    IAisPublisher*        aisPublisher();      // publish targets
    const AisTargetStore* aisData() const;     // read; connect targetUpdated/Expired

    // Routes & waypoints (read + write; one handle, no source arbitration)
    RouteStore* routes();                      // CRUD + routesChanged/waypointsChanged

    // Menu contributions (main menu "Plugins" section)
    void addMenuAction(QString title, std::function<void()> onTriggered);
    void addMenuToggle(QString title, bool checked, std::function<void(bool)> onToggled);

    // Per-plugin persistent settings
    IPluginSettings* pluginSettings(QString pluginId);

    // Settings pages (Settings > Plugin Settings)
    void addSettingsPage(ISettingsPageProvider* provider);
    void showSettingsPage(ISettingsPageProvider* provider);

    // Data sources (Settings > Data Connections; also joins Data Priority)
    IDataSource* registerDataSource(QString sourceId, QString name,
                                    std::function<void()> onOpenSettings);

    // Chart overlays
    void addChartOverlay(IChartOverlay* overlay);
    void removeChartOverlay(IChartOverlay* overlay);
    void requestChartRepaint();

    // Chart sources (pluggable vector-chart backends, e.g. CM93)
    void registerChartSource(IChartSource* source);
    void unregisterChartSource(IChartSource* source);

    QWidget* dialogParent();                    // parent for plugin dialogs
};
```

### Navigation data

Plugins **publish** through `INavDataPublisher` (same contract the simulator and
NMEA client use), so per-value source, timestamp, aging, and priority
arbitration all apply automatically — see `docs/nav_data_store.md`.

```cpp
NavValueMeta m;
m.source = QStringLiteral("my-plugin");
m.timestampUtc = QDateTime::currentDateTimeUtc();
core->navPublisher()->publishDepth(metres, m);
```

To **read / subscribe**, use `navData()` (a `const NavDataStore*`) and connect to
its `ownshipChanged()` signal:

```cpp
connect(core->navData(), &NavDataStore::ownshipChanged, this, [this] {
    const NavValue& d = store_->ownship().depthMeters;
    if (d.valid()) show(d.value, d.source);   // value + provenance + freshness
});
```

### Routes & waypoints

`routes()` hands back the core's `RouteStore` — the same persistent store
(`routes.db`) the chart overlay and list dialogs use — so a plugin can read,
create, edit, and delete routes and standalone waypoints. Unlike nav/AIS there is
no per-value source arbitration, so reading and writing share a single handle
rather than a split read-store / write-publisher pair.

The store keeps the whole collection in memory (loaded once at open); reads return
cached snapshots by const-ref, and writes update both the cache and the DB and
then emit a change signal. `routes()` may be `null` if the database failed to
open — check before use.

```cpp
RouteStore* rs = core->routes();
if (!rs) return;                                   // store unavailable

// Read snapshots.
for (const Route&    r : rs->routes())    use(r);
for (const Waypoint& w : rs->waypoints()) use(w);
const Route* r = rs->route(id);                    // nullptr if absent

// Subscribe to changes (e.g. to re-sync or redraw).
connect(rs, &RouteStore::routesChanged,    this, [this] { refresh(); });
connect(rs, &RouteStore::waypointsChanged, this, [this] { refresh(); });

// Create.
Waypoint w; w.name = "Anchorage"; w.lat = 51.5; w.lon = -0.1;
qint64 wid = rs->addWaypoint(w);                   // -1 on failure

Route route; route.name = rs->nextRouteName();     // suggested "Route N"
route.points = { {51.0, -1.0, "Start"}, {51.2, -0.8, "End"} };
qint64 rid = rs->addRoute(route);                  // inserts route + points

// Edit (by id) and delete.
Route edited = *rs->route(rid);
edited.points.push_back({51.3, -0.7, "Extra"});
rs->updateRoute(edited);                           // replaces the point list
rs->setWaypointVisible(wid, false);
rs->removeWaypoint(wid);
```

`Route`, `RoutePoint`, and `Waypoint` are the plain-data types in
`route_types.hpp`, shaped to map onto GPX (`<rte>`/`<rtept>`/`<wpt>`). `id == -1`
marks a record not yet persisted; the store fills it in on `addRoute`/
`addWaypoint`. `updateWaypoint`/`updateRoute` match on `id` and preserve the
original `createdUtc`. Writes are GUI-thread only (same as the rest of the API).

The **GPX Import / Export** plugin (`plugins/gpx_plugin`, a runtime-loaded DLL)
is the worked example of this surface: it reads the store to write GPX 1.1 files
and parses GPX back into `addRoute`/`addWaypoint`, all behind a touch-friendly
dialog reached from the Plugins menu.

### Menu contributions

`addMenuAction` / `addMenuToggle` append to a **Plugins** section of the main menu
(hidden until the first item is added). The toggle uses the same check-mark style
as the built-in toggles. Callbacks fire on the GUI thread.

### Plugin dialogs and UI chrome

Most plugin UI should be a core-hosted **settings page** (`ISettingsPageProvider`,
below): the plugin supplies only a content widget and the core owns the window
chrome. When a plugin genuinely needs its own top-level dialog (the **GPX Import /
Export** plugin is the worked example — reached from a menu action, parented to
`dialogParent()`), it can match the host's frameless white/blue look without
re-implementing it by including a small set of **sanctioned core UI-helper
headers** from `src/` (consumed by source, resolved against the host exe at link
time — the same way plugins already use `theme.hpp`):

| Header | Provides |
|--------|----------|
| `theme.hpp` | `theme::menu()` / `theme::input()` / `theme::textMuted()` — the shared palette. |
| `window_dragger.hpp` | Header-only; makes a frameless window draggable by a child widget (e.g. its title bar). |
| `dialog_chrome.hpp` | Header-only; the frameless side-menu dialog chrome used app-wide — `dialogchrome::setup(dlg, title[, closeOnDismiss])` builds the bordered panel + navy title bar with a ✕, returning the panel layout; plus `sectionHeader()`, `styledSlider()`, `styleAccentButton()` / `styleOutlinedButton()`, and `okCancelRow()`. |

```cpp
auto* dlg = new QDialog(core->dialogParent());
dlg->setAttribute(Qt::WA_DeleteOnClose);
// Modeless + self-deleting → dismiss via close() (closeOnDismiss), not reject().
auto* panelCol = dialogchrome::setup(dlg, QStringLiteral("My Plugin"), /*closeOnDismiss=*/true);
panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Options")));
// … add a margined body widget …
panelCol->addWidget(dialogchrome::okCancelRow(dlg));   // or a custom button bar
```

These are **host-internal** headers, not a frozen SDK surface: they are sanctioned
for in-tree plugins today but may change until the plugin SDK freezes an ABI. The
intended long-term direction mirrors the settings-page split — a core-hosted
dialog service on `ICoreApi` so plugins supply only content and never include
chrome internals.

### Data sources

`registerDataSource(sourceId, name, onOpenSettings)` makes the plugin a
first-class navigation source:

- The core adds an item named `name` under **Settings > Data Connections**, with
  a status dot, sitting alongside NMEA 0183 and Simulator.
- `sourceId` (the stable id the plugin stamps on `NavValueMeta.source`) is added
  to the runtime `DataSourceRegistry`, so the source **appears in the Data
  Priority dialog** and participates in priority/fall-back arbitration like a
  built-in source.
- Clicking the item invokes `onOpenSettings` — typically the plugin's own
  settings dialog.
- The returned `IDataSource*` lets the plugin drive its dot:

```cpp
class IDataSource { virtual void setActive(bool on) = 0; };   // green dot on/off
```

The core owns the handle; the plugin holds a non-owning pointer.

### Persistent settings

`pluginSettings(pluginId)` returns an `IPluginSettings` namespaced to the plugin,
backed by the same store the core uses (survives restarts):

```cpp
class IPluginSettings {
    void     setValue(const QString& key, const QVariant& value);
    QVariant value(const QString& key, const QVariant& def = {}) const;
};
```

Values are stored under `plugins/<pluginId>/<key>`. The NMEA 0183 plugin uses this
to remember its connection settings (port, baud) and restore them on the next run.

### Settings pages

A plugin supplies only the **content** of its settings page; the core hosts it
(window chrome, title, parenting, single-instance) so plugins don't manage their
own dialogs:

```cpp
class ISettingsPageProvider {
    QString  settingsPageTitle() const;
    QWidget* createSettingsPage(QWidget* parent);   // core takes ownership
};
```

`addSettingsPage(provider)` adds an item under **Settings > Plugin Settings**
(hidden until the first one). `showSettingsPage(provider)` opens it on demand —
e.g. a data source routes its item's click here, so the same page serves both
entry points. The NMEA 0183 plugin implements this; its page holds the connection
settings, and its Data Connections item opens that same page.

## Chart overlays

Plugins do not add `QGraphicsItem`s or know how the canvas is implemented. They
implement `IChartOverlay` and register it; the core calls `paint()` each frame
after its own drawing, in device coordinates.

```cpp
class IChartOverlay {
    virtual void paint(QPainter& painter, const ChartViewport& viewport) = 0;
    virtual bool hitTest(const QPointF& screenPt) { return false; }   // optional
};
```

`ChartViewport` is a per-frame snapshot of the camera with the coordinate helpers
an overlay needs — so drawing can be expressed geographically without knowing the
projection or zoom:

```cpp
struct ChartViewport {
    QPointF geoToScreen(double latDeg, double lonDeg) const;   // handles 180 wrap
    void    screenToGeo(const QPointF& px, double& latDeg, double& lonDeg) const;
    double  pixelsPerMetre() const;
    QSize   viewportSize() const;
};
```

The viewport is valid only for the duration of one `paint()` call. The core owns
z-order (registration order) and lifetime; the plugin owns the overlay object and
must `removeChartOverlay()` it in `shutdown()`.

## Chart sources

A plugin can supply **vector charts in a new format** by registering an
`IChartSource` backend (`chart_source.hpp`). The built-in ENC/S-57 reader
(GDAL-based) is the default way cells enter the pipeline; a chart source plugs an
alternative backend — e.g. a CM93 reader — into the *same* downstream pipeline.
Everything past a parsed cell (catalog, quilting, the `FeatureCache` LRU,
clip/build, S-52 symbology, paint) is unchanged: a source plugs in simply by
producing the same value types the ENC path already does.

```cpp
class IChartSource {
    QString sourceId() const;        // stable id, e.g. "cm93"
    QString displayName() const;     // human label for status text / UI

    // Cheap directory-signature test: does `root` hold charts this source reads?
    bool canHandle(const QString& root) const;

    // Enumerate every cell under `root` with a cheap footprint (no full parse).
    bool catalog(const QString& root, std::vector<ChartSourceCell>& out,
                 QString& errMsg,
                 const std::function<void(int done, int total)>& progress);

    // Full parse of one cell into projected Features + bbox.
    bool loadCell(const QString& cellId, std::vector<Feature>& out,
                  BBox& bbox, QString& errMsg);
};
```

The two work methods mirror the built-in path one-to-one:

| Built-in ENC path (default) | `IChartSource` method |
|---|---|
| `ChartCatalog` enumerates `*.000` + `computeCellCoverage` | `catalog(root)` → cells (id, band, bbox, coverage) |
| `chart::loadCellFeatures` (GDAL S-57) | `loadCell(id)` → `std::vector<Feature>` |

`catalog()` advertises each cell as a `ChartSourceCell`:

```cpp
struct ChartSourceCell {
    QString id;        // opaque cell identity, round-tripped verbatim to loadCell()
    int     band = 0;  // normalized usage band: 1 = overview .. 6 = berthing (0 = unknown)
    BBox    bbox;      // footprint, projected Mercator (north-up: +y north)
    std::vector<std::vector<Pt>> coverage;   // exterior rings; empty => use bbox
};
```

Key contracts:

- **Output is in the host's value types.** Geometry is projected to Mercator
  metres (`projection.hpp`), and every `Feature` carries an **S-57 object-class
  acronym** and attributes (`Feature::objClass` / `::attrs`). A non-S-57 source
  (CM93) must translate its native object/attribute dictionary onto S-57 acronyms
  so the host's S-52 symbology engine resolves them with no special-casing.
- **`band` is normalized.** A source with a different scale model (CM93 has 8
  scales, Z and A..G; the host band model spans 1..8) maps its native scale onto
  the host band so quilting and `bandForVisibleWidth` work unmodified.
- **`id` is opaque.** The host never parses it; it round-trips verbatim to
  `loadCell()` and stands in for the file path as the per-cell cache/loaded key.
- **Thread-safety is required.** `catalog()` runs once per scan on a worker
  thread; `loadCell()` runs concurrently for different cells. Both must be
  thread-safe. Return `false` and set `errMsg` on failure. The `progress(done,
  total)` callback (invoked on the scan worker thread) is optional to call — use
  it to drive the host's scan UI during a heavy first-run decode.

### Registration and selection

```cpp
void initialize(ICoreApi* core) override {
    core->registerChartSource(&cm93Source_);   // plugin owns the object
}
void shutdown() override {
    core_->unregisterChartSource(&cm93Source_);  // MUST unregister before it dies
}
```

The plugin **owns** the `IChartSource` object and must `unregisterChartSource()`
it in `shutdown()` before the object is destroyed. When the user switches chart
sets, the host offers each selected folder to every registered source's
`canHandle()` and uses the **first** that claims it; if none do, it falls back to
the built-in ENC reader. When several chart sets are active at once, the catalog
quilts only the sets that share the active backend; sets needing a *different*
backend are skipped (raster MBTiles still loads from every selected folder).

The **CM93** plugin (`chartplotter-cm93`, a runtime-loaded DLL in a separate
GPL-2.0 repository) is the worked example of this surface — see `docs/cm93.md`. It
translates the proprietary CM93 v2 dictionary onto S-57 acronyms, maps CM93's 8
global scales onto host bands, and uses the `catalog` progress callback during its
parallel first-run cell decode.

## Worked examples

Each contribution surface has a real plugin exercising it end-to-end:

| Plugin | API surface exercised | What it does |
|--------|-----------------------|--------------|
| **NMEA 0183** (`Nmea0183Plugin`, built-in) | `registerDataSource`, `IDataSource::setActive`, `navPublisher`, `ISettingsPageProvider`, `pluginSettings` | A first-class nav source with a status dot, a core-hosted settings page, persisted settings, and a raw-data debug window. Joins Data Priority. |
| **GPX Import / Export** (`plugins/gpx_plugin`, DLL) | `addMenuAction`, `routes`, `dialogParent`, the sanctioned chrome headers | Reads/writes the route store as GPX 1.1 behind a touch-friendly, host-styled dialog. |
| **CM93** (`chartplotter-cm93`, DLL, separate repo) | `registerChartSource`, `IChartSource` | Supplies CM93 v2 vector charts through the chart-source seam — see `docs/cm93.md`. |

Wiring the plugin host into the core (`MainWindow`): build the one `CoreApi`,
register the built-in plugins, discover dynamic ones from disk, then initialize all
of them together:

```cpp
coreApi_ = std::make_unique<CoreApi>(navStore_, aisStore_, routeStore_, sideMenu_,
                                     view_, &registry_, &chartSources_, this);
plugins_ = std::make_unique<PluginManager>(coreApi_.get());
plugins_->add(std::make_unique<Nmea0183Plugin>());   // built-in, in-process
plugins_->add(std::make_unique<Nmea2000Plugin>());
plugins_->loadFromDirectory(QCoreApplication::applicationDirPath() + "/plugins");
plugins_->initializeAll();                           // built-ins + DLLs, uniformly
```

## Dynamic loading

A dynamic plugin is a shared library (`*.dll` / `*.so` / `*.dylib`) dropped in the
`plugins/` folder beside the executable. `PluginManager::loadFromDirectory()`
discovers and validates each one; loaded plugins live alongside built-ins and are
indistinguishable to the rest of the host.

The library exposes exactly **one** `QObject` implementing `IPluginFactory`
(`plugin_factory.hpp`), declared with `Q_PLUGIN_METADATA`. Keeping the factory
separate from the `IPlugin` lets a plugin's main class stay free of `QObject`
inheritance (most plugins inherit both `IPlugin` and `ISettingsPageProvider` and
would otherwise need a diamond):

```cpp
class MyPluginFactory : public QObject, public IPluginFactory {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID CHARTPLOTTER_PLUGIN_IID FILE "my_plugin.json")
    Q_INTERFACES(IPluginFactory)
public:
    int abiVersion() const override { return kPluginAbiVersion; }
    std::unique_ptr<IPlugin> create() override { return std::make_unique<MyPlugin>(); }
};
```

**ABI versioning.** `kPluginAbiVersion` (currently **4**) is bumped whenever a
method on `ICoreApi` / `IPlugin` or the layout of a boundary type changes; the IID
(`CHARTPLOTTER_PLUGIN_IID`) moves in lock-step. The loader checks the version
twice — first via the JSON metadata (cheap, no binary mapped), then a
defence-in-depth `abiVersion()` virtual call after instantiation — and skips
mismatched plugins with a warning rather than failing the whole scan. The chart
source surface arrived in this history: **v3** added
`registerChartSource`/`unregisterChartSource` and the `IChartSource` value types;
**v4** added the `catalog` progress callback.

## Extending: how to add a plugin

1. Implement `IPlugin`. In `initialize(core)`, register your contributions:
   menu items, overlays, and/or a data source. Stash `core` and any handles.
2. Publish nav updates through `core->navPublisher()`; read/subscribe via
   `core->navData()`.
3. Draw with an `IChartOverlay` and the `ChartViewport` helpers.
4. Undo anything that outlives the plugin in `shutdown()`.
5. Ship it: either a built-in via `mgr.add(...)` in `MainWindow`, or a DLL in the
   `plugins/` folder (expose an `IPluginFactory` — see [Dynamic loading](#dynamic-loading)).

## Extensibility — honest assessment

Strong:

- The publish / subscribe / contribute-UI / draw-overlay boundary from the spec
  is real and enforced — plugins never reach into the chart scene, nav structs,
  or settings directly.
- Built-in and dynamically-loaded plugins use the exact same interfaces, so a
  plugin can move in-process ↔ DLL with no code change, and the host treats them
  identically.
- **Dynamic loading is in place.** `PluginManager::loadFromDirectory()` discovers
  DLL/SO plugins, validated against a versioned ABI (`kPluginAbiVersion`, the
  `IPluginFactory` IID) both via cheap JSON metadata and a defensive virtual call.
- Nav publishing rides the existing per-value source / aging / priority model,
  and plugin sources join the runtime `DataSourceRegistry`, so they appear in the
  Data Priority dialog and arbitrate alongside built-in sources.
- **Chart formats are pluggable.** `IChartSource` lets a plugin supply vector
  charts in a new format (CM93) through the existing catalog → quilt → S-52 → paint
  pipeline, with no special-casing downstream of a parsed cell.
- Plugins persist their own settings via `pluginSettings(pluginId)`, namespaced
  and backed by the core store, and contribute a core-hosted settings page via
  `ISettingsPageProvider` (the plugin supplies only the content widget).

Where it will grow (additions, not rewrites):

- **More contribution points.** `IAisProvider`, `IInstrumentProvider`,
  `IRouteTool`, and overlay `hitTest` routing are sketched in the spec and slot in
  as further `ICoreApi` services / interfaces, exactly as `IChartSource` did.

- **Threading.** Today plugins are initialized on the GUI thread (a chart source's
  `catalog()`/`loadCell()` already run on the host's scan/worker threads). A
  *source* doing blocking I/O on the GUI thread would still need its own thread and
  to marshal publishes back — a documentation/helper concern, not an interface
  change.

None of these breaks the `IPlugin` / `ICoreApi` contract or existing plugins.
