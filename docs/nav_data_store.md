# NavDataStore

The core navigation data store: the single source of truth for live navigation
state. Sources publish updates through a stable interface; consumers subscribe
to signals; the core owns the data.

This is the foundation everything navigational eventually hangs off (instruments,
AIS, routing, autopilot). It implements the publish/subscribe contract in
`ProjectSpec.md`: plugins never mutate shared state directly — they call into a
stable publisher API, and the core decides what's authoritative.

```
+----------+     publish      +--------------+     signal      +-----------+
| sources  | ---------------> | NavDataStore | --------------> | consumers |
| sim,     |                  |              |                 | chart     |
| NMEA0183,|                  | OwnshipState |                 | view,     |
| NMEA2000,|                  | (per-value   |                 | NavData   |
| Signal K |                  |  source +    |                 | browser,  |
| ...      |                  |  freshness + |                 | route     |
|          |                  |  priority)   |                 | navigator |
+----------+                  +--------------+                 +-----------+
```

## Data model

Each navigation value is **self-describing**: it carries its own source and
timestamp and ages independently. This is what lets position come from NMEA 0183
while depth and wind arrive from NMEA 2000, each with its own freshness.

### `NavValueMeta`

Provenance a publisher supplies when it sets a value:

| field          | meaning                                                |
|----------------|--------------------------------------------------------|
| `source`       | e.g. `"simulator"`, `"nmea0183"`, `"nmea2000"`         |
| `timestampUtc` | when the source produced this value (UTC)              |

### `NavValue`

A single value plus the freshness the store derives for it:

```cpp
struct NavValue {
    double       value;
    QString      source;        // empty until first set
    QDateTime    timestampUtc;  // invalid until first set
    double       ageSeconds;    // maintained by the store's tick
    NavFreshness freshness;     // Fresh / Stale / Invalid

    bool   valid() const;                 // freshness != Invalid
    bool   stale() const;                 // freshness == Stale
    double valueOr(double fallback) const;
};
```

A value is "available" when `valid()` — i.e. it has been set and has not aged
out. A never-set value and an aged-out value both read as Invalid, so consumers
treat them identically.

### `OwnshipState`

Live ownship navigation as a flat struct of `NavValue`s. Names mirror Signal K
paths conceptually (`navigation.position`, `navigation.courseOverGroundTrue`, …)
but storage is typed C++, not a JSON tree.

```cpp
NavValue latitudeDeg;
NavValue longitudeDeg;
NavValue cogDegTrue;
NavValue sogKnots;
NavValue waterSpeedKnots;
NavValue headingDegTrue;
NavValue headingDegMag;
NavValue variationDeg;
NavValue depthMeters;
// Wind: apparent and true are distinct (relative to the bow); true wind
// direction is geographic.
NavValue apparentWindAngleDeg;
NavValue apparentWindSpeedKnots;
NavValue trueWindAngleDeg;
NavValue trueWindSpeedKnots;
NavValue trueWindDirectionDeg;
```

### `NavFreshness`

Per-value state machine reflecting how stale that value is:

| state     | when                                  |
|-----------|---------------------------------------|
| `Fresh`   | `age < staleSeconds`                  |
| `Stale`   | `staleSeconds ≤ age < invalidSeconds` |
| `Invalid` | `age ≥ invalidSeconds` (or never set) |

Thresholds are runtime-configurable via `setStaleSeconds()` /
`setInvalidSeconds()`; defaults are 5 s and 30 s. They apply to every value.

## Publisher API

Sources call into the store through `INavDataPublisher`. Each call carries its
own `meta`, so different fields can originate from different sources and age
independently:

```cpp
class INavDataPublisher {
    virtual void publishOwnshipPosition(double latDeg, double lonDeg,
                                        const NavValueMeta& meta) = 0;
    virtual void publishCogSog(double cogDegTrue, double sogKnots,
                               const NavValueMeta& meta) = 0;
    virtual void publishHeading(std::optional<double> headingDegTrue,
                                std::optional<double> headingDegMag,
                                const NavValueMeta& meta) = 0;
    virtual void publishVariation(double variationDeg, const NavValueMeta& meta) = 0;
    virtual void publishDepth(double depthMeters, const NavValueMeta& meta) = 0;
    virtual void publishWaterSpeed(double knots, const NavValueMeta& meta) = 0;
    virtual void publishApparentWind(double speedKnots, double angleDeg,
                                     const NavValueMeta& meta) = 0;
    virtual void publishTrueWind(double speedKnots, double angleDeg,
                                 const NavValueMeta& meta) = 0;
    virtual void publishTrueWindDirection(double directionDeg, double speedKnots,
                                          const NavValueMeta& meta) = 0;
};
```

The NMEA 0183 plugin decodes RMC, GLL, GGA, VTG, HDT, HDG, VHW, DBT, DPT, MWV,
MWD, VWR, and VWT into these (apparent wind from MWV-R / VWR, true wind from
MWV-T / VWT, true wind direction from MWD, water speed from VHW, etc.).

`NavDataStore` implements this; `Simulator` and `Nmea0183Client` call it. The
store is the only place that knows about the underlying data.

Why an interface and not a direct `NavDataStore*`:

- Sources compile against `INavDataPublisher` only, not the whole store.
- Tests can inject a fake publisher to assert what a source emits.
- The same interface works whether the publisher is built-in or a dynamically
  loaded plugin (the NMEA 0183/2000 plugins publish through exactly this).

## Source arbitration (priority with fall-back)

When more than one source publishes the *same* value (e.g. two GPS feeds both
giving position), the store decides per value which one is authoritative. It is
**priority with fall-back**, not last-writer-wins:

- Priority is an ordered list of source ids, highest first, set via
  `setSourcePriority(QStringList)`. Unknown (unlisted) sources rank below every
  listed one, so any configured source outranks an unconfigured one.
- An incoming publish is **accepted** to overwrite the current value when any of:
  - the value has never been set; **or**
  - the current value has aged out to `Invalid` (fall-back — once the preferred
    source goes quiet, anyone may take over); **or**
  - it is the *same* source refreshing its own value; **or**
  - the new source ranks **equal or higher** than the current value's source.
- A rejected publish leaves the value (and its provenance) untouched and emits no
  signal.

Arbitration is per value, so a higher-priority position source and a
lower-priority depth source coexist — each field is contested independently. The
choke point is `setValue()`, which calls `accept()` before overwriting; publishers
need no awareness of it. The order is configured from the **Data Priority** dialog;
plugin data sources join that list when they call `registerDataSource()` (see
`docs/plugin_api.md`).

## Subscriber API

Consumers subscribe to two signals:

- `ownshipChanged()` — fired on any accepted ownship publish **and** on any
  per-value freshness transition (so a value going Stale/Invalid notifies
  consumers even with no new data).
- `navigationChanged()` — fired when the route-following output changes (see
  [Route-following output](#route-following-output-navigationdata) below).

Current ownship state is read via `ownship()`; the position fix's freshness
(which drives the ownship symbol) is available via `positionFreshness()`. Example:

```cpp
connect(store, &NavDataStore::ownshipChanged, this, [this, store] {
    const OwnshipState& o = store->ownship();
    if (o.latitudeDeg.valid() && o.longitudeDeg.valid())
        updateMyOverlay(o.latitudeDeg.value, o.longitudeDeg.value);
});
```

The store mutates its members on the GUI thread; signals are direct by default.

## Freshness lifecycle

A 2 Hz internal `tick()` re-ages **every** value:

```
for each value with a timestamp:
    age = now - value.timestampUtc
    freshness = age<stale ? Fresh : age<invalid ? Stale : Invalid
```

`ownshipChanged()` is emitted only when at least one value transitions (not on
every tick), so idle repaints are avoided. The chart view dims the ownship
triangle and draws a horizontal cancellation slash when the **position** is
`Stale`, and hides the symbol when `Invalid` — the marine convention for an
unreliable / DR fix. The NavData Browser greys each `Stale` row and removes each
`Invalid` row independently.

## Route-following output (`NavigationData`)

Alongside the arbitrated *sensor* values, the store holds the **computed
route-following solution** — the derived quantities a chartplotter shows and
transmits while navigating a route (cross-track error, bearing and range to the
next waypoint, arrival flags, …). These mirror the fields of the NMEA 0183 **APB**
and **RMB** sentences (and feed NMEA 2000 PGNs 129283/129284/129285).

Because these are *derived*, not arbitrated sensor inputs, they live outside
`OwnshipState` in a separate `NavigationData` struct with its own signal:

```cpp
const NavigationData& navigation() const;   // read current solution
void setNavigationData(const NavigationData& d);   // replace + emit navigationChanged()
void clearNavigation();                            // reset to inactive defaults
signals: void navigationChanged();
```

```cpp
struct NavigationData {
    bool    active = false;          // false => not navigating; struct at defaults
    QString source;                  // "route-navigator", or a link id if received
    double  xteNm;  QChar steerDirection;            // cross-track error + L/R to regain
    bool    arrivalCircleEntered, perpendicularPassed;
    double  bearingOriginToDestDeg;  QChar bearingUnits;   // 'T' true / 'M' magnetic
    QString destinationWaypointId, originWaypointId;
    double  bearingPresentToDestDeg, headingToSteerDeg;
    bool    hasOrigin;  double originLatDeg, originLonDeg; // active leg's "from" waypoint
    double  destinationLatDeg, destinationLonDeg;
    double  rangeToDestNm, closingVelocityKn;        // range + VMG to next waypoint
    QChar   faaStatus, faaMode;
};
```

`RouteNavigator` is the producer: while the user navigates a route it recomputes
this each second from the current fix and the active leg and calls
`setNavigationData()`; on arrival or stop it calls `clearNavigation()`. Consumers
(the nav display, the NMEA 0183/2000 transmitters) read `navigation()` and
subscribe to `navigationChanged()`.

The `source` field is the **transmit loop guard**: it is `"route-navigator"` when
computed internally (always safe to send), or the id of the link the solution
*arrived* on if it was received and parsed in (e.g. `"nmea0183"`). A transmitter
checks it to avoid echoing APB/RMB back out the same link they came from. Bearings
are in the user-selected reference, with `bearingUnits` recording which; all
distances are nautical miles.

## Extending: how to add things

### A new publisher (e.g. NMEA 2000)

Same shape as `Simulator` / `Nmea0183Client` — depth and wind from a CAN gateway
simply carry a different `source`:

```cpp
void onPgnDepth(double metres) {
    NavValueMeta m;
    m.source       = QStringLiteral("nmea2000");
    m.timestampUtc = QDateTime::currentDateTimeUtc();
    publisher_->publishDepth(metres, m);   // ages independently of position
}
```

The source has no dependency on `NavDataStore`. `MainWindow` wires it up the
same way the simulator and NMEA 0183 client are today.

### A new ownship field

Three small steps:

1. Add the field as a `NavValue` on `OwnshipState`, and include it in
   `NavDataStore::recompute()`'s aging list.
2. If sources will publish it, add a `publishXxx(...)` method to
   `INavDataPublisher` and implement it on `NavDataStore` (call `setValue`, emit
   `ownshipChanged()`).
3. Subscribers that care read `ownship().xxx.value` when `.valid()`; ones that
   don't, ignore it. Additions are non-breaking.

## Extensibility — honest assessment

Strong:

- The publisher/subscriber boundary is real. NMEA 0183/2000, AIS decoders,
  Signal K bridges, and replay sources all slot in as new publishers with no
  store or subscriber changes.
- Per-value source + freshness means mixed-source rigs (position from one bus,
  depth/wind from another) work correctly, and each value ages on its own.
- **Source priority / arbitration is in place.** Multiple sources for the same
  value are resolved by an ordered priority list with aged-out fall-back, decided
  per value at the `setValue` choke point — no external API change was needed, and
  plugin sources join the priority list automatically.
- **AIS targets and routes/waypoints landed as their own stores**, exactly as the
  pattern predicted: `AisTargetStore` (keyed by MMSI, its own `IAisPublisher` and
  `targetUpdated`/`targetExpired` signals — see `docs/ais_target_store.md`) and
  `RouteStore` (core-owned, with plugin-provided GPX import/export — see
  `docs/plugin_api.md`). Both sit alongside, not inside, `OwnshipState`.
- New fields are non-breaking additions.
- Freshness logic is centralized, not scattered through consumers.

Where it will grow. These are additions, not rewrites:

- **Per-category thresholds.** Today all values share one stale/invalid pair;
  AIS, instruments, and depth/wind may eventually want their own.

- **Field set vs. typed map.** With ~14 fields the flat `OwnshipState` struct is
  the clearest representation. If the field count grows toward Signal K scope
  (50+ paths) a typed key→value map becomes worth considering. Not needed yet.

None of these breaks the publish/subscribe contract, the interface, or any
existing subscriber.
