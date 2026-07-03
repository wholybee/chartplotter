# AisTargetStore

The core store for AIS targets (Milestone 5 in `ProjectSpec.md`). It mirrors the
`NavDataStore` model — sources publish through a stable interface, consumers
subscribe to signals, the core owns the data — but is keyed by **MMSI** and
holds many independent vessels instead of one ownship state.

```
+-----------+   publish    +----------------+   signals   +------------------+
| AIS source| -----------> | AisTargetStore | ----------> | overlay, target  |
| (decoder  |              |  QHash<mmsi,   |             | list, CPA calc   |
|  plugin)  |              |    AisTarget>  |             |                  |
+-----------+              +----------------+             +------------------+
```

Today the store is fed by `AisDecoder`, owned by the NMEA 0183 plugin: AIS rides
the same connection as the nav sentences, so the plugin forwards `!AIVDM`/`!AIVDO`
lines to the decoder, which publishes via `IAisPublisher`. Sources reach the
publisher from plugins as `ICoreApi::aisPublisher()`.

## Data model

### `AisTarget`

One tracked vessel, built up from position reports (dynamic) and static/voyage
reports. Optional fields are absent until a report supplies them.

| group | fields |
|-------|--------|
| identity | `mmsi`, `cls` (A/B/Unknown), `name`, `callSign`, `shipType`, `imoNumber` |
| voyage (Class A) | `destination`, `draughtMeters` |
| size | `dimensions` (to bow/stern/port/starboard; `lengthMeters()`, `beamMeters()`) |
| dynamic | `latitudeDeg`, `longitudeDeg`, `cogDegTrue`, `sogKnots`, `headingDegTrue`, `rotDegPerMin`, `navStatus` |
| computed | `rangeMeters`, `cpaMeters`, `tcpaSeconds`, `cpaOwnshipPos`/`cpaTargetPos` (set by a collision component, not from AIS) |
| provenance | `source`, `lastUpdateUtc`, `ageSeconds`, `freshness` |

`AisDimensions` stores the four reference-point offsets AIS transmits; length and
beam are derived. `AisNavStatus` is the standard 0–15 status enum (Class A only).

### `AisFreshness`

Per-target lifecycle, on AIS timescales (minutes, not the ownship's seconds):

| state     | when                                  |
|-----------|---------------------------------------|
| `Current` | `age < staleSeconds` (default 6 min)  |
| `Stale`   | `staleSeconds ≤ age < lostSeconds`    |
| `Lost`    | `age ≥ lostSeconds` (default 12 min)  |

A 1 Hz tick ages every target. On reaching `Lost` the target is **removed** and
`targetExpired(mmsi)` fires; while `Current`/`Stale` it stays in the store (an
overlay would grey a stale contact).

## Publisher API

Sources call `IAisPublisher`. Position and static reports are separate because
AIS sends them in different messages (1/2/3 & 18/19 vs 5 & 24); each carries the
fields it actually has, and either kind keeps the target alive.

```cpp
class IAisPublisher {
    virtual void publishAisPosition(const AisPositionReport& report, const QString& source) = 0;
    virtual void publishAisStatic(const AisStaticData& data, const QString& source) = 0;
};
```

A report only overwrites fields it supplies, so a position update never wipes the
name and a static update never wipes the position. `source` records provenance
(e.g. `"ais.nmea0183"`).

## Subscriber API

```cpp
const QHash<quint32, AisTarget>& targets() const;
const AisTarget* target(quint32 mmsi) const;   // nullptr if not tracked
int count() const;

signals:
    void targetUpdated(quint32 mmsi);   // added, refreshed, or freshness changed
    void targetExpired(quint32 mmsi);   // aged out and removed
```

Consumers connect to the signals and read `targets()`. CPA/TCPA are attached by a
collision component via `setCpaTcpa(mmsi, cpa, tcpa)`, which emits `targetUpdated`
only when the values materially changed (range/CPA by more than ~1 m, TCPA by
more than ~0.5 s, or a value appearing/disappearing). The calculator republishes
every second, so without this dead band a live feed would emit per target per
second forever and the chart could never go idle. Suppressed values are not
stored, so slow drift accumulates against the last published value and still
gets through.

## AisDecoder

`AisDecoder` turns raw `!AIVDM`/`!AIVDO` lines into store updates. It is
transport-agnostic — fed by whoever owns the connection (today the NMEA 0183
plugin) — and handles:

- **Multi-fragment reassembly** (long messages span several sentences, keyed by
  the sequential message id).
- **6-bit ASCII payload** unpacking with typed bit-field readers.
- The common message types: **1/2/3** (Class A position), **5** (Class A static
  & voyage), **9** (SAR aircraft position), **18/19** (Class B position; 19 also
  carries static), **21** (Aid to Navigation — publishes a position *and* a
  static report, like 19), **24** (Class B static, Parts A & B). "Not available"
  sentinels (lat 91°, lon 181°, COG 3600, heading 511, SOG 1023, ROT −128,
  altitude 4095) decode to absent fields.

### Target kinds beyond vessels

`AisClass` covers more than the Class A/B vessels: **AtoN** (aids to navigation —
buoys, beacons, lighthouses; from msg 21, carrying an aid-type code plus
virtual/real and off-position flags), **SarAircraft** (search-and-rescue aircraft;
msg 9, carrying altitude and whole-knot SOG with no heading), and **Sart**
(distress beacons: AIS-SART, MOB, and EPIRB-AIS). Distress beacons transmit
ordinary type-1 position reports, so the **store** reclassifies any A/B target
whose MMSI is in the 970 / 972 / 974 ranges to `Sart` — that single rule covers
every transport (0183, N2K, SignalK), not just the AIVDM decoder.

## CpaCalculator

`CpaCalculator` is the collision component referenced above. It reads ownship
position/COG/SOG from `NavDataStore` and each target's position/COG/SOG from the
store, treats both as straight-line tracks in a local east/north plane centred on
the ownship, and computes:

- **TCPA** = `-(Δr · Δv) / (Δv · Δv)` — the instant the relative-position vector
  is perpendicular to the relative-velocity vector (negative once the contact is
  opening, i.e. CPA already passed).
- **CPA** = the range at that instant, `|Δr + Δv·TCPA|`.
- **CPA endpoints** (`cpaOwnshipPos` / `cpaTargetPos`) — each vessel projected
  along its own track to TCPA; the geographic stage of the encounter, consumed
  by the overlay's CPA graphics. Absent when TCPA has no finite solution.

It also computes each target's **distance to ownship** (`rangeMeters`), written
via `setRangeMeters`. Distance is kept separate from CPA/TCPA because it needs
only the two positions: a target that reports no course/speed (so CPA can't be
solved) still gets a range, as does the case where the ownship has a fix but no
COG/SOG.

It runs on a 1 Hz tick and also recomputes the moment ownship updates, writing
results back with `setRangeMeters` / `setCpaTcpa`. It deliberately does **not**
listen to `targetUpdated` (which those setters emit) to avoid recursion. When the
ownship position is unavailable, every target's range/CPA/TCPA are cleared rather
than left stale; the configured own MMSI is skipped so the boat's own AIS echo
never alarms on itself. Both the quick-look popup and the full info window display
distance and (when solvable) CPA (nm) and TCPA (min).

## AisOverlay

`AisOverlay` (`IChartOverlay`) draws each `AisTarget` with `hasPosition()` using
the shared `vessel::drawSymbol` glyph — the same triangle / course-prediction
line / cancellation slash as the ownship symbol, just green so the two are
distinguishable. Each target kind gets its own standard symbol: Class A a filled
triangle, Class B an arrowhead, **AtoN** an amber diamond (hollow for a virtual
aid), **SAR aircraft** a blue plane silhouette oriented along its track, and a
**distress beacon** (SART/MOB/EPIRB) a red cross-in-circle. AtoN and distress
symbols are orientation-independent (drawn upright); only vessels and SAR
aircraft point along heading/COG. CPA danger flagging applies to Class A/B
vessels only. Targets are dimmed and crossed when their freshness is `Stale`
(default ≥ 6 min since the last message); `Lost` targets are already gone from
the store, so the overlay never has to draw them. The predictor length tracks
the ownship one (a single user-configurable value, kept in sync).

**Dangerous targets** are drawn **red** instead of green, inside a yellow disc
with a grey rim (diameter = 1.5× the glyph length) so they stand out against any
chart. A target is dangerous when, per the user's *Dangerous Ships* rules, its
CPA is within the configured limit (the base trigger), optionally also requiring
its TCPA to be ahead and within a time window; a target beyond the optional
range limit is never flagged. The overlay evaluates this each paint from the
`rangeMeters` / `cpaMeters` / `tcpaSeconds` the `CpaCalculator` maintains, so no
extra per-target state is stored.

When a dangerous encounter is still ahead (TCPA ≥ 0), the overlay also draws the
**CPA encounter graphics**: a bold dashed red track from each vessel's bow to
the point it will occupy at TCPA (`cpaTargetPos` for the target, `cpaOwnshipPos`
for ownship — the overlay reads ownship's current fix from the `NavDataStore`),
a blue dot at each of those points, and a red-edged yellow bar joining the dots —
the closest-approach separation itself.

## Not yet (next steps)

- **Per-source arbitration** — currently last-writer-wins per MMSI (fine for a
  single receiver); priority could be added like the nav store if needed.
- **Native N2K / SignalK AtoN & SAR-aircraft** — these kinds are decoded from
  AIVDM (0183) today. The equivalent NMEA 2000 PGNs (129041 AtoN, 129798 SAR
  aircraft) and the SignalK equivalents are not yet wired up. Distress beacons
  (SART/MOB/EPIRB) already work on every transport via the store's MMSI rule.
