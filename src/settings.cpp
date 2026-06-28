#include "settings.hpp"
#include <QSettings>
#include <QDir>
#include <cmath>

namespace {
constexpr auto kChartDir  = "charts/directory";   // legacy single active dir
constexpr auto kSelected  = "charts/selected";    // directories of active sets
constexpr auto kChartSets = "charts/sets";
constexpr auto kSoundings = "display/showSoundings";
constexpr auto kSymbols   = "display/showSymbols";
constexpr auto kText      = "display/showText";
constexpr auto kContours  = "display/showDepthContours";
constexpr auto kAisShow   = "display/showAisTargets";
constexpr auto kRasterShow = "display/showRasterCharts";
constexpr auto kVectorOverlay = "display/vectorOverlay";
constexpr auto kHideSymPan = "display/hideSymbolsWhilePanning";
constexpr auto kViewLon   = "view/centerLon";
constexpr auto kViewLat   = "view/centerLat";
constexpr auto kViewScale = "view/scale";
constexpr auto kBasemap   = "basemap/directory";
constexpr auto kStaleS      = "nav/staleSeconds";
constexpr auto kInvalidS    = "nav/invalidSeconds";
constexpr auto kAisStaleS   = "nav/aisStaleSeconds";
constexpr auto kAisLostS    = "nav/aisLostSeconds";
constexpr auto kPredMin   = "ships/ownshipPredictionMinutes";
constexpr auto kDepthUnit = "units/depth";
constexpr auto kDistUnit  = "units/distance";
constexpr auto kAngleFmt  = "units/angle";
constexpr auto kBearing   = "units/bearing";
constexpr auto kArrivalNm = "nav/arrivalRadiusNm";
constexpr auto kSrcPrio   = "data/sourcePriority";
constexpr auto kAutoHide  = "menu/autoHide";
constexpr auto kDetailLvl = "display/chartDetailLevel";
constexpr auto kScaminLvl = "display/chartScaminLevel";
constexpr auto kSymScale    = "display/symbolScale";
constexpr auto kVesselScale = "display/vesselScale";
constexpr auto kOwnMmsi     = "nav/ownshipMmsi";
constexpr auto kHeadingSrc  = "ships/headingSource";
constexpr auto kDangIgnoreFarOn = "ships/dangerIgnoreFarEnabled";
constexpr auto kDangIgnoreFarNm = "ships/dangerIgnoreFarNm";
constexpr auto kDangCpaOn   = "ships/dangerCpaEnabled";
constexpr auto kDangCpaNm   = "ships/dangerCpaNm";
constexpr auto kDangTcpaOn  = "ships/dangerTcpaEnabled";
constexpr auto kDangTcpaMin = "ships/dangerTcpaMin";
constexpr auto kDangAnchoredOn  = "ships/dangerAnchoredSafeEnabled";
constexpr auto kDangAnchoredKn  = "ships/dangerAnchoredSogKn";
} // namespace

Settings::Settings(QObject* parent) : QObject(parent) {
    QSettings s;
    chartDir_          = s.value(QLatin1String(kChartDir)).toString();
    selectedDirs_      = s.value(QLatin1String(kSelected)).toStringList();
    showSoundings_     = s.value(QLatin1String(kSoundings), true).toBool();
    showSymbols_       = s.value(QLatin1String(kSymbols),   true).toBool();
    showText_          = s.value(QLatin1String(kText),      true).toBool();
    showDepthContours_ = s.value(QLatin1String(kContours),  true).toBool();
    showAisTargets_    = s.value(QLatin1String(kAisShow),   true).toBool();
    showRasterCharts_  = s.value(QLatin1String(kRasterShow), true).toBool();
    vectorOverlay_     = s.value(QLatin1String(kVectorOverlay), false).toBool();
    hideSymbolsWhilePanning_ = s.value(QLatin1String(kHideSymPan), false).toBool();
    viewLon_   = s.value(QLatin1String(kViewLon),   0.0).toDouble();
    viewLat_   = s.value(QLatin1String(kViewLat),   0.0).toDouble();
    viewScale_ = s.value(QLatin1String(kViewScale), 0.0).toDouble();
    basemapDir_ = s.value(QLatin1String(kBasemap)).toString();
    staleSeconds_   = s.value(QLatin1String(kStaleS),   5.0).toDouble();
    invalidSeconds_ = s.value(QLatin1String(kInvalidS), 30.0).toDouble();
    aisStaleSeconds_ = s.value(QLatin1String(kAisStaleS), 360.0).toDouble();
    aisLostSeconds_  = s.value(QLatin1String(kAisLostS),  720.0).toDouble();
    ownshipPredMin_ = s.value(QLatin1String(kPredMin),  6.0).toDouble();
    depthUnit_    = units::depthUnitFromKey(s.value(QLatin1String(kDepthUnit)).toString(),
                                            DepthUnit::Feet);
    distanceUnit_ = units::distanceUnitFromKey(s.value(QLatin1String(kDistUnit)).toString(),
                                               DistanceUnit::NauticalMiles);
    angleFormat_  = units::angleFormatFromKey(s.value(QLatin1String(kAngleFmt)).toString(),
                                              AngleFormat::DecimalDegrees);
    bearingMode_  = units::bearingModeFromKey(s.value(QLatin1String(kBearing)).toString(),
                                              BearingMode::True);
    arrivalRadiusNm_ = s.value(QLatin1String(kArrivalNm), 0.1).toDouble();
    if (arrivalRadiusNm_ < 0.001) arrivalRadiusNm_ = 0.001;
    // Raw saved order; reconciled against the runtime DataSourceRegistry (which
    // includes plugin sources) where it is consumed.
    dataSourcePriority_ = s.value(QLatin1String(kSrcPrio)).toStringList();
    autoHideMenu_ = s.value(QLatin1String(kAutoHide), true).toBool();
    chartDetailLevel_ = s.value(QLatin1String(kDetailLvl), 0.0).toDouble();
    if (chartDetailLevel_ < -2.0) chartDetailLevel_ = -2.0;
    if (chartDetailLevel_ >  2.0) chartDetailLevel_ =  2.0;
    chartScaminLevel_ = s.value(QLatin1String(kScaminLvl), 0.0).toDouble();
    if (chartScaminLevel_ < -1.0) chartScaminLevel_ = -1.0;
    if (chartScaminLevel_ >  1.0) chartScaminLevel_ =  1.0;
    symbolScale_ = s.value(QLatin1String(kSymScale), 1.0).toDouble();
    if (symbolScale_ < 0.5) symbolScale_ = 0.5;
    if (symbolScale_ > 3.0) symbolScale_ = 3.0;
    vesselScale_ = s.value(QLatin1String(kVesselScale), 1.0).toDouble();
    if (vesselScale_ < 0.5) vesselScale_ = 0.5;
    if (vesselScale_ > 3.0) vesselScale_ = 3.0;
    ownshipMmsi_ = s.value(QLatin1String(kOwnMmsi)).toString();
    headingSource_ = headingsrc::fromKey(s.value(QLatin1String(kHeadingSrc)).toString(),
                                         HeadingSource::Heading);
    dangerIgnoreFarEnabled_ = s.value(QLatin1String(kDangIgnoreFarOn), true).toBool();
    dangerIgnoreFarNm_      = s.value(QLatin1String(kDangIgnoreFarNm), 20.0).toDouble();
    dangerCpaEnabled_  = s.value(QLatin1String(kDangCpaOn), true).toBool();
    dangerCpaNm_       = s.value(QLatin1String(kDangCpaNm), 2.0).toDouble();
    dangerTcpaEnabled_ = s.value(QLatin1String(kDangTcpaOn), true).toBool();
    dangerTcpaMin_     = s.value(QLatin1String(kDangTcpaMin), 30.0).toDouble();
    dangerAnchoredSafeEnabled_ = s.value(QLatin1String(kDangAnchoredOn), true).toBool();
    dangerAnchoredSogKn_       = s.value(QLatin1String(kDangAnchoredKn), 0.1).toDouble();
    loadChartSets();

    // Migrate a pre-chart-sets install: if no sets are defined yet but a chart
    // directory was remembered, seed one set from it so the existing setup keeps
    // working and appears in the menu.
    if (chartSets_.isEmpty() && !chartDir_.isEmpty()) {
        ChartSet cs;
        cs.directory = chartDir_;
        cs.name = QDir(chartDir_).dirName();
        if (cs.name.isEmpty()) cs.name = chartDir_;
        chartSets_.push_back(cs);
        saveChartSets();
    }

    // Migrate the single-active-directory selection to the multi-select list: if
    // nothing is selected yet but a chart directory was remembered, pre-select it.
    if (selectedDirs_.isEmpty() && !chartDir_.isEmpty()) {
        selectedDirs_ << chartDir_;
        QSettings().setValue(QLatin1String(kSelected), selectedDirs_);
    }
}

void Settings::loadChartSets() {
    chartSets_.clear();
    QSettings s;
    const int n = s.beginReadArray(QLatin1String(kChartSets));
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        ChartSet cs;
        cs.name      = s.value(QLatin1String("name")).toString();
        cs.directory = s.value(QLatin1String("directory")).toString();
        if (!cs.directory.isEmpty()) chartSets_.push_back(cs);
    }
    s.endArray();
}

void Settings::saveChartSets() {
    QSettings s;
    // Clear first so a shorter list doesn't leave stale trailing entries behind.
    s.remove(QLatin1String(kChartSets));
    s.beginWriteArray(QLatin1String(kChartSets));
    for (int i = 0; i < chartSets_.size(); ++i) {
        s.setArrayIndex(i);
        s.setValue(QLatin1String("name"),      chartSets_[i].name);
        s.setValue(QLatin1String("directory"), chartSets_[i].directory);
    }
    s.endArray();
}

void Settings::setChartSets(const QVector<ChartSet>& sets) {
    chartSets_ = sets;
    saveChartSets();
    emit chartSetsChanged();
}

void Settings::setView(double lon, double lat, double scale) {
    viewLon_ = lon; viewLat_ = lat; viewScale_ = scale;
    QSettings s;
    s.setValue(QLatin1String(kViewLon), lon);
    s.setValue(QLatin1String(kViewLat), lat);
    s.setValue(QLatin1String(kViewScale), scale);
}

void Settings::setOwnshipPredictionMinutes(double minutes) {
    if (minutes == ownshipPredMin_) return;
    ownshipPredMin_ = minutes;
    QSettings().setValue(QLatin1String(kPredMin), minutes);
    emit ownshipPredictionMinutesChanged(minutes);
}

void Settings::setDepthUnit(DepthUnit u) {
    if (u == depthUnit_) return;
    depthUnit_ = u;
    QSettings().setValue(QLatin1String(kDepthUnit), units::depthUnitKey(u));
    emit depthUnitChanged(u);
}

void Settings::setAngleFormat(AngleFormat u) {
    if (u == angleFormat_) return;
    angleFormat_ = u;
    QSettings().setValue(QLatin1String(kAngleFmt), units::angleFormatKey(u));
    emit angleFormatChanged(u);
}

void Settings::setDistanceUnit(DistanceUnit u) {
    if (u == distanceUnit_) return;
    distanceUnit_ = u;
    QSettings().setValue(QLatin1String(kDistUnit), units::distanceUnitKey(u));
    emit distanceUnitChanged(u);
}

void Settings::setBearingMode(BearingMode b) {
    if (b == bearingMode_) return;
    bearingMode_ = b;
    QSettings().setValue(QLatin1String(kBearing), units::bearingModeKey(b));
    emit bearingModeChanged(b);
}

void Settings::setArrivalRadiusNm(double nm) {
    if (nm < 0.001) nm = 0.001;
    if (nm == arrivalRadiusNm_) return;
    arrivalRadiusNm_ = nm;
    QSettings().setValue(QLatin1String(kArrivalNm), nm);
    emit arrivalRadiusNmChanged(nm);
}

void Settings::setDataSourcePriority(const QStringList& orderedSourceIds) {
    if (orderedSourceIds == dataSourcePriority_) return;
    dataSourcePriority_ = orderedSourceIds;
    QSettings().setValue(QLatin1String(kSrcPrio), orderedSourceIds);
    emit dataSourcePriorityChanged(orderedSourceIds);
}

void Settings::setAutoHideMenu(bool on) {
    if (on == autoHideMenu_) return;
    autoHideMenu_ = on;
    QSettings().setValue(QLatin1String(kAutoHide), on);
    emit autoHideMenuChanged(on);
}

void Settings::setVesselScale(double scale) {
    if (scale < 0.5) scale = 0.5;
    if (scale > 3.0) scale = 3.0;
    if (scale == vesselScale_) return;
    vesselScale_ = scale;
    QSettings().setValue(QLatin1String(kVesselScale), scale);
    emit vesselScaleChanged(scale);
}

void Settings::setOwnshipMmsi(const QString& mmsi) {
    if (mmsi == ownshipMmsi_) return;
    ownshipMmsi_ = mmsi;
    QSettings().setValue(QLatin1String(kOwnMmsi), mmsi);
    emit ownshipMmsiChanged(mmsi);
}

void Settings::setHeadingSource(HeadingSource src) {
    if (src == headingSource_) return;
    headingSource_ = src;
    QSettings().setValue(QLatin1String(kHeadingSrc), headingsrc::key(src));
    emit headingSourceChanged(src);
}

void Settings::setDangerousShips(bool ignoreFarEnabled, double ignoreFarNm,
                                 bool cpaEnabled, double cpaNm,
                                 bool tcpaEnabled, double tcpaMin,
                                 bool anchoredSafeEnabled, double anchoredSogKn) {
    if (ignoreFarEnabled == dangerIgnoreFarEnabled_ && ignoreFarNm == dangerIgnoreFarNm_
        && cpaEnabled == dangerCpaEnabled_ && cpaNm == dangerCpaNm_
        && tcpaEnabled == dangerTcpaEnabled_ && tcpaMin == dangerTcpaMin_
        && anchoredSafeEnabled == dangerAnchoredSafeEnabled_
        && anchoredSogKn == dangerAnchoredSogKn_)
        return;
    dangerIgnoreFarEnabled_ = ignoreFarEnabled;
    dangerIgnoreFarNm_      = ignoreFarNm;
    dangerCpaEnabled_  = cpaEnabled;
    dangerCpaNm_       = cpaNm;
    dangerTcpaEnabled_ = tcpaEnabled;
    dangerTcpaMin_     = tcpaMin;
    dangerAnchoredSafeEnabled_ = anchoredSafeEnabled;
    dangerAnchoredSogKn_       = anchoredSogKn;
    QSettings s;
    s.setValue(QLatin1String(kDangIgnoreFarOn), ignoreFarEnabled);
    s.setValue(QLatin1String(kDangIgnoreFarNm), ignoreFarNm);
    s.setValue(QLatin1String(kDangCpaOn),   cpaEnabled);
    s.setValue(QLatin1String(kDangCpaNm),   cpaNm);
    s.setValue(QLatin1String(kDangTcpaOn),  tcpaEnabled);
    s.setValue(QLatin1String(kDangTcpaMin), tcpaMin);
    s.setValue(QLatin1String(kDangAnchoredOn), anchoredSafeEnabled);
    s.setValue(QLatin1String(kDangAnchoredKn), anchoredSogKn);
    emit dangerousShipsChanged();
}

void Settings::setChartDetailLevel(double level) {
    if (level < -2.0) level = -2.0;
    if (level >  2.0) level =  2.0;
    if (level == chartDetailLevel_) return;
    chartDetailLevel_ = level;
    QSettings().setValue(QLatin1String(kDetailLvl), level);
    emit chartDetailLevelChanged(level);
}

void Settings::setChartScaminLevel(double level) {
    if (level < -1.0) level = -1.0;
    if (level >  1.0) level =  1.0;
    if (level == chartScaminLevel_) return;
    chartScaminLevel_ = level;
    QSettings().setValue(QLatin1String(kScaminLvl), level);
    emit chartScaminLevelChanged(level);
}

void Settings::setSymbolScale(double scale) {
    if (scale < 0.5) scale = 0.5;
    if (scale > 3.0) scale = 3.0;
    if (scale == symbolScale_) return;
    symbolScale_ = scale;
    QSettings().setValue(QLatin1String(kSymScale), scale);
    emit symbolScaleChanged(scale);
}

void Settings::setStaleThresholds(double staleS, double invalidS) {
    if (staleS == staleSeconds_ && invalidS == invalidSeconds_) return;
    staleSeconds_   = staleS;
    invalidSeconds_ = invalidS;
    QSettings s;
    s.setValue(QLatin1String(kStaleS),   staleS);
    s.setValue(QLatin1String(kInvalidS), invalidS);
    emit staleThresholdsChanged(staleS, invalidS);
}

void Settings::setAisStaleThresholds(double staleS, double lostS) {
    if (staleS == aisStaleSeconds_ && lostS == aisLostSeconds_) return;
    aisStaleSeconds_ = staleS;
    aisLostSeconds_  = lostS;
    QSettings s;
    s.setValue(QLatin1String(kAisStaleS), staleS);
    s.setValue(QLatin1String(kAisLostS),  lostS);
    emit aisStaleThresholdsChanged(staleS, lostS);
}

void Settings::setBasemapDirectory(const QString& dir) {
    if (dir == basemapDir_) return;
    basemapDir_ = dir;
    QSettings().setValue(QLatin1String(kBasemap), dir);
    emit basemapDirectoryChanged(dir);
}

void Settings::setSelectedDirectories(const QStringList& dirs) {
    // De-duplicate while preserving order.
    QStringList unique;
    for (const QString& d : dirs)
        if (!d.isEmpty() && !unique.contains(d)) unique << d;
    if (unique == selectedDirs_) return;
    selectedDirs_ = unique;
    QSettings().setValue(QLatin1String(kSelected), selectedDirs_);
    emit selectedDirectoriesChanged(selectedDirs_);
}

void Settings::setShowSoundings(bool on) {
    if (on == showSoundings_) return;
    showSoundings_ = on;
    QSettings().setValue(QLatin1String(kSoundings), on);
    emit showSoundingsChanged(on);
}

void Settings::setShowSymbols(bool on) {
    if (on == showSymbols_) return;
    showSymbols_ = on;
    QSettings().setValue(QLatin1String(kSymbols), on);
    emit showSymbolsChanged(on);
}

void Settings::setShowText(bool on) {
    if (on == showText_) return;
    showText_ = on;
    QSettings().setValue(QLatin1String(kText), on);
    emit showTextChanged(on);
}

void Settings::setShowDepthContours(bool on) {
    if (on == showDepthContours_) return;
    showDepthContours_ = on;
    QSettings().setValue(QLatin1String(kContours), on);
    emit showDepthContoursChanged(on);
}

void Settings::setShowAisTargets(bool on) {
    if (on == showAisTargets_) return;
    showAisTargets_ = on;
    QSettings().setValue(QLatin1String(kAisShow), on);
    emit showAisTargetsChanged(on);
}

void Settings::setShowRasterCharts(bool on) {
    if (on == showRasterCharts_) return;
    showRasterCharts_ = on;
    QSettings().setValue(QLatin1String(kRasterShow), on);
    emit showRasterChartsChanged(on);
}

void Settings::setVectorOverlay(bool on) {
    if (on == vectorOverlay_) return;
    vectorOverlay_ = on;
    QSettings().setValue(QLatin1String(kVectorOverlay), on);
    emit vectorOverlayChanged(on);
}

void Settings::setHideSymbolsWhilePanning(bool on) {
    if (on == hideSymbolsWhilePanning_) return;
    hideSymbolsWhilePanning_ = on;
    QSettings().setValue(QLatin1String(kHideSymPan), on);
    emit hideSymbolsWhilePanningChanged(on);
}
