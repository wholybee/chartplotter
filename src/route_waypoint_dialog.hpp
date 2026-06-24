#pragma once
#include <QDialog>
#include <QWidget>
#include <vector>

class RouteStore;
class QScrollArea;
class QLabel;
class QPushButton;
class QCheckBox;
class QVBoxLayout;
class QTabWidget;

// Touch-first list of saved routes, as a content widget (no window chrome) so it
// can live inside a tab of RouteWaypointDialog. Drag-to-scroll via QScroller on a
// QScrollArea; each row shows the route name, point count, and a Visible checkbox.
// Tapping a row selects it; in pick mode a row tap emits routePicked() instead and
// the action buttons are hidden.
class RouteListPanel : public QWidget {
    Q_OBJECT
public:
    RouteListPanel(RouteStore* store, bool pickMode, QWidget* parent = nullptr);

signals:
    void routePicked(qint64 id);          // pick mode: user chose a route
    void propertiesRequested(qint64 id);  // open the Properties editor
    void editRequested(qint64 id);        // start the chart-drag edit session
    void newRouteRequested();             // "New": start a fresh route

public slots:
    void refresh();

private:
    struct Row {
        QPushButton* btn  = nullptr;
        QLabel*      name = nullptr;
        QLabel*      meta = nullptr;
        QCheckBox*   vis  = nullptr;
        qint64       id   = -1;
    };
    Row  makeRow();
    void selectRow(qint64 id);
    void restyleRows();
    void updateActionState();
    void deleteSelected();

    RouteStore*  store_       = nullptr;
    bool         pickMode_    = false;
    qint64       selectedId_  = -1;
    QScrollArea* scrollArea_  = nullptr;
    QWidget*     rowContainer_= nullptr;
    QVBoxLayout* rowLayout_   = nullptr;
    QPushButton* deleteBtn_   = nullptr;
    QPushButton* propsBtn_    = nullptr;
    QPushButton* editBtn_     = nullptr;
    QPushButton* newBtn_      = nullptr;
    std::vector<Row> rows_;
};

// Touch-first list of saved waypoints, as a content widget (no chrome). Same
// pattern as RouteListPanel; rows show name and position.
class WaypointListPanel : public QWidget {
    Q_OBJECT
public:
    WaypointListPanel(RouteStore* store, bool pickMode, QWidget* parent = nullptr);

signals:
    void waypointPicked(qint64 id);       // pick mode: user chose a waypoint
    void propertiesRequested(qint64 id);  // open the Properties editor
    void editRequested(qint64 id);        // start the chart-drag edit session
    void newWaypointAtOwnshipRequested(); // "Drop at Boat"

public slots:
    void refresh();

private:
    struct Row {
        QPushButton* btn  = nullptr;
        QLabel*      name = nullptr;
        QLabel*      pos  = nullptr;
        QCheckBox*   vis  = nullptr;
        qint64       id   = -1;
    };
    Row  makeRow();
    void selectRow(qint64 id);
    void restyleRows();
    void updateActionState();
    void deleteSelected();

    RouteStore*  store_       = nullptr;
    bool         pickMode_    = false;
    qint64       selectedId_  = -1;
    QScrollArea* scrollArea_  = nullptr;
    QWidget*     rowContainer_= nullptr;
    QVBoxLayout* rowLayout_   = nullptr;
    QPushButton* deleteBtn_   = nullptr;
    QPushButton* propsBtn_    = nullptr;
    QPushButton* editBtn_     = nullptr;
    QPushButton* dropBtn_     = nullptr;
    std::vector<Row> rows_;
};

// Combined saved-objects browser: the route and waypoint lists in a tabbed,
// frameless side-menu dialog (gpx-style tabs).
//
//  - Manage mode (modeless): both tabs, with per-tab action buttons; reached from
//    the side menu's "Routes & Waypoints…" item. The host connects the relayed
//    *Requested signals.
//  - Pick mode (modal): a single category for the Edit-Route / Edit-Waypoint
//    pickers. exec(); on Accepted read pickedId().
class RouteWaypointDialog : public QDialog {
    Q_OBJECT
public:
    enum class Tab { Routes = 0, Waypoints = 1 };

    explicit RouteWaypointDialog(RouteStore* store, QWidget* parent = nullptr);   // manage
    RouteWaypointDialog(RouteStore* store, Tab pickCategory, QWidget* parent);    // pick

    qint64 pickedId() const { return pickedId_; }
    void   selectTab(Tab t);
    void   refreshLists();   // re-render both lists (e.g. after a coord-format change)

signals:
    void routePropertiesRequested(qint64 id);
    void routeEditRequested(qint64 id);
    void newRouteRequested();
    void waypointPropertiesRequested(qint64 id);
    void waypointEditRequested(qint64 id);
    void newWaypointAtOwnshipRequested();

private:
    RouteStore*        store_      = nullptr;
    QTabWidget*        tabs_       = nullptr;
    RouteListPanel*    routePanel_ = nullptr;
    WaypointListPanel* wptPanel_   = nullptr;
    qint64             pickedId_   = -1;
};
