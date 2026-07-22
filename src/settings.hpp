#pragma once
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include "units.hpp"
#include "heading_source.hpp"
#include "render_backend.hpp"

// A named chart directory the user can switch between from the menu.
struct ChartSet {
    QString name;
    QString directory;
};

// Central, persistent application settings: the single source of truth for
// user-facing preferences, backed by QSettings.
//
// Components read current values and subscribe to the change signals rather
// than touching QSettings directly. This keeps settings consistent and
// observable, and matches the core/plugin model in ProjectSpec.md — the core
// owns shared state, everything else publishes and subscribes.
class Settings : public QObject {
    Q_OBJECT
public:
    explicit Settings(QObject* parent = nullptr);

    // Directories of the chart sets currently selected ("active"). More than one
    // may be active at a time; their charts combine (e.g. a raster set in one
    // folder layered with a vector set in another). Order follows the chart-set
    // list. Empty means nothing is loaded.
    const QStringList& selectedDirectories() const { return selectedDirs_; }
    bool showSoundings() const { return showSoundings_; }
    bool showSymbols() const { return showSymbols_; }
    // Object text labels (S-57 OBJNAM) drawn next to point objects.
    bool showText() const { return showText_; }
    bool showDepthContours() const { return showDepthContours_; }
    // AIS targets drawn on the chart. When off, the overlay paints nothing and
    // ignores clicks, but the store and CpaCalculator keep tracking and the
    // dangerous-ship logic keeps running — it just has nothing to draw.
    bool showAisTargets() const { return showAisTargets_; }
    // MBTiles raster charts drawn beneath the ENC vector cells. When off, the
    // raster layer paints nothing; discovery/loading is unaffected.
    bool showRasterCharts() const { return showRasterCharts_; }
    // Vector-overlay mode: hide the opaque chart base (land/water area fills and
    // the GSHHG basemap) so the raster layer shows through, leaving only vector
    // linework/symbols on top. Intended for satellite/imagery raster bases.
    bool vectorOverlay() const { return vectorOverlay_; }

    // The user's defined chart sets, in menu order.
    const QVector<ChartSet>& chartSets() const { return chartSets_; }

    // Folder holding the GSHHG basemap data (contains GSHHS_shp/). Empty means
    // "search the standard locations"; an explicit path also lets the user point
    // at a higher-resolution tier they dropped in.
    QString basemapDirectory() const { return basemapDir_; }

    // Last view location, so the app reopens where the user left off. The view
    // is the center in geographic degrees plus the zoom (scene pixels per metre).
    bool hasSavedView() const { return viewScale_ > 0.0; }
    double viewLon() const { return viewLon_; }
    double viewLat() const { return viewLat_; }
    double viewScale() const { return viewScale_; }

    // Ownship stale-data thresholds (seconds). The fix is considered Stale at
    // staleSeconds and Invalid (hidden) at invalidSeconds.
    double staleSeconds()   const { return staleSeconds_; }
    double invalidSeconds() const { return invalidSeconds_; }

    // AIS target stale-data thresholds (seconds). Targets are greyed at
    // aisStaleSeconds and removed at aisLostSeconds. Defaults 360 s / 720 s
    // (6 min / 12 min), matching standard AIS reporting intervals.
    double aisStaleSeconds() const { return aisStaleSeconds_; }
    double aisLostSeconds()  const { return aisLostSeconds_; }

    // Length of the ownship course-prediction line, in minutes of run-time at
    // the current SOG. Drawn from the bow along the boat's heading.
    double ownshipPredictionMinutes() const { return ownshipPredMin_; }

    // Display units. Depth drives how chart soundings are labelled; distance
    // drives the scale bar and route-leg labels.
    DepthUnit    depthUnit()    const { return depthUnit_; }
    DistanceUnit distanceUnit() const { return distanceUnit_; }
    AngleFormat  angleFormat()  const { return angleFormat_; }
    // Whether bearings/headings are shown relative to true or magnetic north.
    BearingMode  bearingMode()  const { return bearingMode_; }

    // Arrival radius: how close (nautical miles) the boat must come to a waypoint
    // to count as "arrived". Default 0.1 nm.
    double arrivalRadiusNm() const { return arrivalRadiusNm_; }

    // Track recording interval. A track point is laid down once BOTH have been
    // satisfied: at least trackIntervalSeconds have passed since the last point,
    // and the boat has moved at least trackMinDistanceNm from it. The distance
    // gate is what keeps a boat at anchor from filling a track with one point per
    // interval on top of itself. Defaults: 60 s and 150 ft (~0.0247 nm).
    static constexpr double kDefaultTrackIntervalSec = 60.0;
    static constexpr double kDefaultTrackMinDistNm   = 150.0 / units::kNmToFeet;
    double trackIntervalSeconds() const { return trackIntervalSec_; }
    double trackMinDistanceNm()   const { return trackMinDistNm_; }

    // When true, tapping outside the side menu (or an action item) closes it
    // automatically. When false, the menu stays open until the user presses
    // Close, and the chart remains interactive while it is visible.
    bool autoHideMenu() const { return autoHideMenu_; }

    // When true, the application mirrors its log stream (qDebug/qWarning and the
    // categorized GPU lifecycle — the same output shown on the console on Linux)
    // to a file under %ProgramData%/<App> (or the app-data dir elsewhere). Opt-in;
    // default off. Applied via applog::setEnabled().
    bool logToFile() const { return logToFile_; }

    // Chart detail bias, in fractional bands. 0 = nominal; positive values
    // pull in higher-detail (larger-scale) cells; negative values back off to
    // lower-detail. Range -2.0 .. +2.0, in steps of 1.0 from the dialog.
    double chartDetailLevel() const { return chartDetailLevel_; }

    // SCAMIN declutter bias for point objects (symbols + soundings), in
    // [-1.0, +1.0]. 0 = honour each object's SCAMIN at the current zoom;
    // positive reveals more; negative hides more; the extremes show all / hide
    // all point objects. Consumed by ChartView::setChartScaminLevel.
    double chartScaminLevel() const { return chartScaminLevel_; }

    // Symbol scale factor. 1.0 = nominal (baked) size; range 0.5 .. 3.0 in
    // steps of 0.25 from the dialog (50 % .. 300 %).
    double symbolScale() const { return symbolScale_; }

    // Text-label scale factor (object names, light characters, etc.). 1.0 =
    // nominal; range 0.5 .. 3.0 in 0.25 steps from the Symbol Size dialog.
    double textScale() const { return textScale_; }

    // Depth-sounding scale factor. 1.0 = nominal; range 0.5 .. 3.0 in 0.25
    // steps from the Symbol Size dialog. Independent of textScale so the two
    // families of numbers can be sized separately.
    double soundingScale() const { return soundingScale_; }

    // Text de-clutter: when on, text labels are nudged up to labelNudgeMaxPx to
    // avoid overlapping other labels, symbols, and soundings (soundings and
    // symbols are fixed and never move). Default on, 20 px.
    bool   labelNudgeEnabled() const { return labelNudgeEnabled_; }
    double labelNudgeMaxPx()   const { return labelNudgeMaxPx_; }

    // Vessel glyph scale factor (ownship + AIS). 1.0 = nominal; range 0.5..3.0
    // in steps of 0.25 from the dialog.
    double vesselScale() const { return vesselScale_; }

    // MMSI of the user's own vessel. Empty string means not configured.
    // Validated to be exactly 9 digits before being stored.
    QString ownshipMmsi() const { return ownshipMmsi_; }

    // Which direction the ownship glyph points: true heading or COG.
    HeadingSource headingSource() const { return headingSource_; }

    // Chart-rendering backend preference (Stage 7). Auto = use the GPU backend
    // when available, else the CPU painter; Cpu = always painter (the safe
    // choice if a machine's GPU drivers misbehave). The user-facing control is a
    // "Use GPU acceleration" toggle (Auto/Cpu).
    RenderBackend renderBackend() const { return renderBackend_; }

    // "Dangerous ship" rules. The values are persisted now; the logic that
    // consumes them (flagging targets) is added later. Each rule has an enable
    // flag and a threshold.
    bool   dangerIgnoreFarEnabled() const { return dangerIgnoreFarEnabled_; }
    double dangerIgnoreFarNm()      const { return dangerIgnoreFarNm_; }
    bool   dangerCpaEnabled()  const { return dangerCpaEnabled_; }
    double dangerCpaNm()       const { return dangerCpaNm_; }
    bool   dangerTcpaEnabled() const { return dangerTcpaEnabled_; }
    double dangerTcpaMin()     const { return dangerTcpaMin_; }
    bool   dangerAnchoredSafeEnabled() const { return dangerAnchoredSafeEnabled_; }
    double dangerAnchoredSogKn()       const { return dangerAnchoredSogKn_; }
    // Sound an audible alarm while any target is dangerous. Opt-in (default off).
    bool   dangerAlarmSound() const { return dangerAlarmSound_; }

    // Data-source priority: ordered source ids, highest priority first.
    QStringList dataSourcePriority() const { return dataSourcePriority_; }

public slots:
    void setSelectedDirectories(const QStringList& dirs);
    void setShowSoundings(bool on);
    void setShowSymbols(bool on);
    void setShowText(bool on);
    void setShowDepthContours(bool on);
    void setShowAisTargets(bool on);
    void setShowRasterCharts(bool on);
    void setVectorOverlay(bool on);
    void setChartSets(const QVector<ChartSet>& sets);
    void setView(double lon, double lat, double scale);
    void setBasemapDirectory(const QString& dir);
    void setStaleThresholds(double staleS, double invalidS);
    void setAisStaleThresholds(double staleS, double lostS);
    void setOwnshipPredictionMinutes(double minutes);
    void setDepthUnit(DepthUnit u);
    void setDistanceUnit(DistanceUnit u);
    void setAngleFormat(AngleFormat u);
    void setBearingMode(BearingMode b);
    void setArrivalRadiusNm(double nm);
    void setTrackInterval(double seconds, double minDistanceNm);
    void setDataSourcePriority(const QStringList& orderedSourceIds);
    void setAutoHideMenu(bool on);
    void setLogToFile(bool on);
    void setChartDetailLevel(double level);
    void setChartScaminLevel(double level);
    void setSymbolScale(double scale);
    void setTextScale(double scale);
    void setSoundingScale(double scale);
    void setLabelNudge(bool enabled, double maxPx);
    void setVesselScale(double scale);
    void setOwnshipMmsi(const QString& mmsi);
    void setHeadingSource(HeadingSource s);
    void setRenderBackend(RenderBackend b);
    void setDangerousShips(bool ignoreFarEnabled, double ignoreFarNm,
                           bool cpaEnabled, double cpaNm,
                           bool tcpaEnabled, double tcpaMin,
                           bool anchoredSafeEnabled, double anchoredSogKn,
                           bool alarmSoundEnabled);

signals:
    void selectedDirectoriesChanged(const QStringList& dirs);
    void showSoundingsChanged(bool on);
    void showSymbolsChanged(bool on);
    void showTextChanged(bool on);
    void showDepthContoursChanged(bool on);
    void showAisTargetsChanged(bool on);
    void showRasterChartsChanged(bool on);
    void vectorOverlayChanged(bool on);
    void chartSetsChanged();
    void basemapDirectoryChanged(const QString& dir);
    void staleThresholdsChanged(double staleS, double invalidS);
    void aisStaleThresholdsChanged(double staleS, double lostS);
    void ownshipPredictionMinutesChanged(double minutes);
    void depthUnitChanged(DepthUnit u);
    void distanceUnitChanged(DistanceUnit u);
    void angleFormatChanged(AngleFormat u);
    void bearingModeChanged(BearingMode b);
    void arrivalRadiusNmChanged(double nm);
    void trackIntervalChanged(double seconds, double minDistanceNm);
    void dataSourcePriorityChanged(const QStringList& orderedSourceIds);
    void autoHideMenuChanged(bool on);
    void logToFileChanged(bool on);
    void chartDetailLevelChanged(double level);
    void chartScaminLevelChanged(double level);
    void symbolScaleChanged(double scale);
    void textScaleChanged(double scale);
    void soundingScaleChanged(double scale);
    void labelNudgeChanged(bool enabled, double maxPx);
    void vesselScaleChanged(double scale);
    void ownshipMmsiChanged(const QString& mmsi);
    void headingSourceChanged(HeadingSource s);
    void renderBackendChanged(RenderBackend b);
    void dangerousShipsChanged();   // any dangerous-ship rule changed

private:
    void loadChartSets();
    void saveChartSets();

    QString chartDir_;            // legacy single-active dir; read only for migration
    QStringList selectedDirs_;    // directories of the active chart sets
    bool showSoundings_ = true;
    bool showSymbols_ = true;
    bool showText_ = true;
    bool showDepthContours_ = true;
    bool showAisTargets_ = true;
    bool showRasterCharts_ = true;
    bool vectorOverlay_ = false;
    QVector<ChartSet> chartSets_;
    QString basemapDir_;
    double viewLon_ = 0.0;
    double viewLat_ = 0.0;
    double viewScale_ = 0.0;   // 0 => no saved view
    double staleSeconds_      = 5.0;
    double invalidSeconds_    = 30.0;
    double aisStaleSeconds_   = 360.0;   // 6 min
    double aisLostSeconds_    = 720.0;   // 12 min
    double ownshipPredMin_ = 6.0;   // minutes of run-time ahead
    DepthUnit    depthUnit_    = DepthUnit::Feet;
    DistanceUnit distanceUnit_ = DistanceUnit::NauticalMiles;
    AngleFormat  angleFormat_  = AngleFormat::DecimalDegrees;
    BearingMode  bearingMode_  = BearingMode::True;
    double       arrivalRadiusNm_ = 0.1;     // nautical miles
    double       trackIntervalSec_ = kDefaultTrackIntervalSec;
    double       trackMinDistNm_   = kDefaultTrackMinDistNm;
    QStringList   dataSourcePriority_;
    bool          autoHideMenu_ = true;   // legacy default = current behaviour
    bool          logToFile_    = false;  // opt-in file logging (default off)
    double        chartDetailLevel_ = 0.0;   // -2.0 .. +2.0, 0 = nominal
    double        chartScaminLevel_ = 0.0;   // -1.0 .. +1.0, 0 = nominal SCAMIN
    double        symbolScale_      = 1.0;   // 0.5 .. 3.0, 1.0 = nominal
    double        textScale_        = 1.0;   // 0.5 .. 3.0, label size multiplier
    double        soundingScale_    = 1.0;   // 0.5 .. 3.0, sounding size multiplier
    bool          labelNudgeEnabled_ = true; // nudge labels to reduce overlap
    double        labelNudgeMaxPx_   = 20.0; // max label nudge distance (device px)
    double        vesselScale_      = 1.0;   // 0.5 .. 3.0, 1.0 = nominal
    QString       ownshipMmsi_;              // 9-digit string or empty
    HeadingSource headingSource_ = HeadingSource::Heading;
    // Default to the CPU painter while the GPU backend is brought to S-52 parity
    // (Stage 7 A5). The GPU path renders vector cells but not yet basemap/symbols/
    // text/soundings/raster, so it must be opt-in ("Use GPU acceleration") until
    // complete; flip this back to Auto once parity lands.
    RenderBackend renderBackend_ = RenderBackend::Cpu;
    // Dangerous-ship rules (consumed later); enabled by default with the
    // requested threshold defaults.
    bool   dangerIgnoreFarEnabled_ = true;
    double dangerIgnoreFarNm_      = 20.0;
    bool   dangerCpaEnabled_  = true;
    double dangerCpaNm_       = 2.0;
    bool   dangerTcpaEnabled_ = true;
    double dangerTcpaMin_     = 30.0;
    bool   dangerAnchoredSafeEnabled_ = true;   // suppress flags on stationary vessels
    double dangerAnchoredSogKn_       = 0.1;     // SOG (kn) at/below which it's anchored
    bool   dangerAlarmSound_          = false;   // audible alarm (opt-in)
};
