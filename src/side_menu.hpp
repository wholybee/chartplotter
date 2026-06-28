#pragma once
#include <QWidget>
#include <QString>
#include <functional>

class Settings;
class QPushButton;
class QLabel;
class QStackedWidget;
class QVBoxLayout;
class QPropertyAnimation;
class QWheelEvent;

// Touch-first navigation drawer that slides in over the chart from the left.
//
// Built to the touch UI requirements in ProjectSpec.md: large tap targets,
// no hover-only or right-click interactions, and one-touch dismissal by tapping
// the dimmed scrim. Display toggles are bound to the core Settings object, so
// changing them publishes through the normal settings channel rather than
// reaching into the chart view directly.
//
// The panel has two pages: the main menu (chart sets + view options) and a
// Settings page reached via the "Settings" item. The main page lists the user's
// defined chart sets so they can switch charts with one tap.
class SideMenu : public QWidget {
    Q_OBJECT
public:
    SideMenu(Settings* settings, QWidget* parent);

    void openMenu();
    void closeMenu();
    bool isOpen() const { return open_; }

    // When false, the menu stays open until the user presses Close — tapping
    // outside and tapping action items no longer dismisses it, and the chart
    // underneath stays interactive (no scrim).
    void setAutoHide(bool on);

    // Reflect the view's auto-follow state in the menu's checkmark (e.g. when a
    // pan turns it off). Does not re-emit autoFollowToggled.
    void setAutoFollowChecked(bool on);

    // Reflect the current route-navigation state in the "Navigating" checkbox
    // (e.g. when the user starts navigating from a route popup, or navigation
    // stops on its own). Guards against re-emitting navigatingToggled.
    void setNavigatingChecked(bool on);

    // Plugin contributions to the main menu's Plugins section.
    void addPluginAction(const QString& title, std::function<void()> onTriggered);
    void addPluginToggle(const QString& title, bool checked,
                         std::function<void(bool)> onToggled);

    // A plugin-registered data source: an item under Data Connections. Returns
    // the button so its status dot can be driven via setItemDot().
    QPushButton* addDataSourceItem(const QString& title, std::function<void()> onClicked);
    void setItemDot(QPushButton* item, bool on);   // green dot on/off

    // A plugin settings page entry under the Settings page's Plugin Settings
    // section (hidden until the first one is added).
    void addPluginSettingsItem(const QString& title, std::function<void()> onClicked);

signals:
    void centerOnOwnshipRequested();                  // recenter on ownship
    void zoomToChartsRequested();                      // fit view to the chart set
    void autoFollowToggled(bool on);                  // auto-follow on/off
    void chartSetToggled(const QString& directory);   // user tapped a set to add/remove
    void manageChartSetsRequested();                  // open the Chart Sets dialog
    void basemapFolderRequested();                    // pick the GSHHG data folder
    void editUnitsRequested();                        // open the Units dialog
    void editStaleThresholdsRequested();              // open stale-data dialog
    void editOwnshipPredictionRequested();            // open predictor-length dialog
    void navDataBrowserRequested();                    // open NavData Browser window
    void editDataPriorityRequested();                  // open Data Priority dialog
    void editChartDetailLevelRequested();              // open Chart Detail Level dialog
    void editSymbolSizeRequested();                    // open Symbol Size dialog
    void editVesselSizeRequested();                    // open Ship Size dialog
    void editOwnshipMmsiRequested();                   // open Own Ship MMSI dialog
    void editHeadingSourceRequested();                 // open Heading Source dialog
    void editDangerousShipsRequested();                // open Dangerous Ships dialog
    void aisTargetListRequested();                     // open AIS Targets list dialog
    void aboutRequested();                             // open the About dialog
    void navigationOptionsRequested();                 // open Navigation Options dialog
    void navigatingToggled(bool on);                   // user toggled route navigation
    // Routes & Waypoints sub-page actions.
    void createRouteRequested();
    void editRouteRequested();
    void createWaypointRequested();
    void editWaypointRequested();
    void dropWaypointRequested();
    void routeWaypointListRequested();   // open the combined Routes & Waypoints dialog

protected:
    void resizeEvent(QResizeEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    QLabel*      makeHeader(const QString& text);
    QWidget*     makeSeparator();           // thin divider line within a section
    QPushButton* makeAction(const QString& text);
    // Settings-page action: like makeAction but reserves a leading status-dot
    // column so every item lines up with the ones that show a dot.
    QPushButton* makeSettingsAction(const QString& text);
    // Main-page plain action with a blank check-mark column, so it lines up with
    // the checkable items (active set, Auto Follow, chart-detail toggles).
    QPushButton* makeIndentedAction(const QString& text);
    // Checkable action that shows a check mark when on (like the active set).
    QPushButton* makeCheckAction(const QString& text, bool checked);
    QWidget*     buildMainPage();
    QWidget*     buildSettingsPage();
    QWidget*     buildRoutesPage();
    QWidget*     wrapScroll(QWidget* content);   // scrollable container for a page
    void rebuildChartSets();
    void showMainPage();
    void showSettingsPage();
    void showRoutesPage();
    void layoutPanel();
    void applyModeGeometry();   // size + scrim visibility for current autoHide_

    Settings* settings_ = nullptr;
    QWidget*  scrim_ = nullptr;   // dim layer; tap to dismiss
    QWidget*  panel_ = nullptr;   // the menu surface itself
    QLabel*   title_ = nullptr;   // top bar, reflects the current page
    QPushButton* navBtn_ = nullptr;  // header close (✕) / back (‹) affordance
    QStackedWidget* stack_ = nullptr;
    QVBoxLayout* chartSetsBox_ = nullptr;   // container for the dynamic set buttons
    QPushButton* autoFollowBtn_ = nullptr;  // checkable Auto Follow item
    QPushButton* navigatingBtn_ = nullptr;  // checkable Navigating item
    QLabel*      pluginHeader_ = nullptr;   // "Plugins" header (hidden until used)
    QVBoxLayout* pluginBox_ = nullptr;      // container for plugin-contributed items
    QVBoxLayout* dataSourceBox_ = nullptr;  // plugin data-source items (Data Connections)
    QLabel*      pluginSettingsHeader_ = nullptr;  // "Plugin Settings" header (hidden until used)
    QVBoxLayout* pluginSettingsBox_ = nullptr;     // plugin settings-page items
    QPropertyAnimation* anim_ = nullptr;
    int  panelWidth_ = 320;
    bool open_ = false;
    bool autoHide_ = true;
    int  mainIndex_ = 0;
    int  settingsIndex_ = 1;
    int  routesIndex_ = 2;
};
