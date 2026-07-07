#pragma once
#include <QMainWindow>
#include <QHash>
#include <QPointer>
#include <QString>
#include <memory>
#include "data_sources.hpp"   // DataSourceRegistry (value member)
#include "chart_source.hpp"   // ChartSourceRegistry (value member)
#include "route_types.hpp"    // Route (value member for the props working copy)
#include "chart_object.hpp"   // ChartObjectInfo (signal/slot parameter)

class ChartView;
class ChartCatalog;
class Settings;
class SideMenu;
class NavDataStore;
class AisTargetStore;
class CpaCalculator;
class NavDataBrowserWindow;
class AisOverlay;
class AisTargetInfoWindow;
class AisQuickInfoWindow;
class AisTargetListDialog;
class RouteStore;
class RouteNavigator;
class NavDisplayWindow;
class RouteOverlay;
class RouteWaypointDialog;
class RoutePropertiesDialog;
class RouteQuickInfoWindow;
class ChartObjectInfoWindow;
class LayersDialog;
struct ClickedRouteObject;
class CoreApi;
class PluginManager;
class QLabel;
class QPushButton;
class QWidget;
class QPointF;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;   // out-of-line: unique_ptr to incomplete plugin types

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onChartSetToggled(const QString& dir);
    void manageChartSets();
    void prepareChartCache();
    void chooseBasemapFolder();
    void editUnits();
    void editNavigationOptions();
    void editStaleThresholds();
    void editOwnshipPrediction();
    void showNavDataBrowser();
    void editDataPriority();
    void editChartDetailLevel();
    void editSymbolSize();
    void editVesselSize();
    void editOwnshipMmsi();
    void editHeadingSource();
    void editDangerousShips();
    void showAisTargetList();
    void showAbout();
    // Routes & Waypoints.
    void startCreateRoute();
    void startEditRoute();
    void startCreateWaypoint();
    void startEditWaypoint();
    void dropWaypoint();
    void showRouteWaypointList();             // open the combined tabbed browser
    void openRouteProperties(qint64 id);     // from the Routes tab "Properties"
    void openWaypointProperties(qint64 id);  // from the Waypoints tab "Properties"
    void publishOwnshipToView();
    void onCursorMoved(double lon, double lat);
    void onScanProgress(int done, int total);
    void onScanFinished(bool ok, const QString& message);
    void onRasterChartsChanged(int count);
    void onViewStatus(const QString& text);

private:
    void rescanSelected();       // (re)load all currently-selected chart sets
    void refreshChartStatus();   // compose status from ENC + raster results
    void positionMenuButton();
    void positionLayersButton();  // stack the Layers button under the "+" button
    void showLayersDialog();      // open the floating layer-toggle panel
    void positionCourseUpButton();// stack the Course-Up button under Layers
    void syncCourseUpButton(bool on);       // reflect course-up on/off on the button
    void setCompassHeading(double upDegrees);// rotate the needle to true north
    void refreshCompassIcon();               // repaint the needle (colour + angle)

    // Routes & Waypoints helpers ---------------------------------------------
    void buildEditBar();              // construct the floating action bar (hidden)
    void positionEditBar();           // centre it over the chart top
    void showRouteEditBar(const QString& hint);    // Complete/Delete/Cancel
    void showWaypointPlaceBar(const QString& hint); // Cancel only
    void showWaypointEditBar(const QString& hint);  // Done/Cancel (no Delete Point)
    void showPointDragBar(const QString& hint);     // Done/Cancel for props-drag
    void endRouteMode();              // clear overlay edit + editor + bar
    // Transient dark banner over the chart top announcing route completion;
    // auto-dismisses after a few seconds. Reuses the edit-bar visual style.
    void showNavigationCompleteBanner();
    void positionNavBanner();
    void beginEditRoute(qint64 id);   // fit chart + start editing a saved route
    void beginEditWaypoint(qint64 id);// fit chart + start moving a saved waypoint
    void completeEdit();              // Complete/Done button: dispatch by mode
    void cancelEdit();                // Cancel button: dispatch by mode
    void completeRoute();             // name + persist the working route
    void completeWaypointMove();      // persist the moved waypoint
    void onWaypointPlaced(double lat, double lon);  // create-waypoint tap
    // Route Properties drag round-trip.
    void onPropsEditPoint(int index); // dialog asked to drag point `index`
    void finishPropsDrag(bool apply); // return from chart drag to the dialog

    // Chart-tap / create-affordance handlers ---------------------------------
    void onRouteObjectClicked(const ClickedRouteObject& hit);  // chart tap on saved obj
    // Chart-object query: a click that no route/AIS overlay claimed landed on
    // chart object(s). Shows a chooser when several, else the detail window.
    void onObjectsPicked(const QList<ChartObjectInfo>& objects, const QPoint& globalPos);
    void showObjectInfo(const ChartObjectInfo& obj, const QPoint& globalPos);
    void renameRoute(qint64 id);
    void renameWaypoint(qint64 id);
    void toggleRouteVisible(qint64 id);
    void toggleWaypointVisible(qint64 id);
    void confirmDeleteRoute(qint64 id);
    void confirmDeleteWaypoint(qint64 id);
    // Route navigation (APB/RMB). Navigate from a route popup starts it at the
    // tapped waypoint; the "Navigating" menu checkbox mirrors and can stop/resume.
    void startNavigation(qint64 routeId, int destIndex = -1);
    void onNavigatingToggled(bool on);
    // Long-press on the chart and the floating "+" button both open a small
    // popup at `globalPt`. When `atPoint` is true (long-press) the menu reads
    // "New waypoint here" / "Start route here" and the object is placed at
    // `screenPt` immediately. When false (the "+" button, which has no chart
    // position) it reads "New waypoint" / "New route" and the next chart tap
    // places the first point.
    void onChartLongPressed(const QPointF& screenPt);
    void showAddPopup(const QPointF& screenPt, const QPoint& globalPt, bool atPoint);
    void positionAddButton();

    ChartView*    view_ = nullptr;
    ChartCatalog* catalog_ = nullptr;
    Settings*     settings_ = nullptr;
    SideMenu*     sideMenu_ = nullptr;
    NavDataStore* navStore_ = nullptr;
    AisTargetStore* aisStore_ = nullptr;
    CpaCalculator* cpaCalc_ = nullptr;   // keeps target CPA/TCPA up to date
    NavDataBrowserWindow* navBrowser_ = nullptr;
    std::unique_ptr<AisOverlay>    aisOverlay_;  // core-owned AIS chart overlay
    // Open AIS info windows, keyed by MMSI; QPointer auto-clears on close
    // (windows are WA_DeleteOnClose), so clicking a target again creates a
    // fresh window rather than raising a destroyed one.
    QHash<quint32, QPointer<AisTargetInfoWindow>> aisInfoWindows_;
    // Quick-look popup: the first click on a target shows this; a second click
    // on the same target opens the full window. Only one exists at a time; any
    // chart interaction dismisses it. QPointer clears when it self-deletes.
    QPointer<AisQuickInfoWindow> aisQuickInfo_;
    quint32 aisQuickInfoMmsi_ = 0;
    // Reused list window: one instance per session, raised on subsequent opens.
    QPointer<AisTargetListDialog> aisListDlg_;
    void showAisTarget(quint32 mmsi);   // drives the two-click open behaviour

    // Routes & Waypoints -----------------------------------------------------
    RouteStore* routeStore_ = nullptr;
    RouteNavigator* navigator_ = nullptr;   // route-following engine (child QObject)
    NavDisplayWindow* navDisplay_ = nullptr;  // floating readout over the chart
    std::unique_ptr<RouteOverlay> routeOverlay_;
    QPointer<RouteWaypointDialog> routeWptDlg_;
    // Route Properties editor + the drag round-trip it hands off to the chart.
    QPointer<RoutePropertiesDialog> propsDlg_;
    Route propsWork_;                  // working route while Properties is open
    enum class EditContext { None, RouteProps };
    EditContext editContext_ = EditContext::None;
    // Floating action bar shown over the chart during a create/edit session.
    QWidget*     editBar_ = nullptr;
    QLabel*      editHint_ = nullptr;
    QPushButton* completeBtn_ = nullptr;
    QPushButton* deletePointBtn_ = nullptr;
    QPushButton* cancelEditBtn_ = nullptr;
    QLabel*      navBanner_ = nullptr;      // transient "Navigation Complete." banner
    DataSourceRegistry             registry_;    // nav sources (built-in + plugin)
    ChartSourceRegistry            chartSources_; // vector-chart backends (plugin)
    std::unique_ptr<CoreApi>       coreApi_;     // plugin-facing core services
    std::unique_ptr<PluginManager> plugins_;     // owns built-in plugins
    QPushButton*  menuButton_ = nullptr;
    QPushButton*  addButton_  = nullptr;   // floating "+" next to menu button
    QPushButton*  layersButton_ = nullptr; // floating layers toggle, under "+"
    QPointer<LayersDialog> layersDlg_;     // floating layer-toggle panel (one at a time)
    QPushButton*  courseUpButton_ = nullptr; // floating course-up toggle, under Layers
    double        courseUpAngle_ = 0.0;      // bearing at screen-top (for the needle)
    // Quick-look popup for a tapped saved route/waypoint. One at a time; chart
    // interaction dismisses it. QPointer clears when it self-deletes.
    QPointer<RouteQuickInfoWindow> routeQuickInfo_;
    // Detail window for a clicked chart object. One at a time; chart interaction
    // dismisses it. QPointer clears when it self-deletes.
    QPointer<ChartObjectInfoWindow> objectInfo_;
    QLabel*       statusLeft_ = nullptr;   // root folder + scan summary
    QLabel*       statusMid_ = nullptr;    // band / cells shown
    QLabel*       statusRight_ = nullptr;  // cursor lat/lon
    QString       root_;
    double        lastCursorLon_ = 0.0;   // last cursor pos, for re-rendering the
    double        lastCursorLat_ = 0.0;   // status bar when the coord format changes
    // Latest scan results for the active folder, reconciled into one status line
    // (ENC scan and raster discovery finish independently / out of order).
    bool          encScanDone_ = false;
    bool          encScanOk_ = false;
    QString       encScanMsg_;
    int           rasterCount_ = 0;
};
