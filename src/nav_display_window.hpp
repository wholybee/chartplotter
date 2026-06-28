#pragma once
#include <QFrame>
#include <QPoint>

class NavDataStore;
class QLabel;
class CdiWidget;

// Small floating readout shown over the chart while navigating a route. Displays
// the values a helmsman steers by — cross-track error (+ which way to steer),
// bearing to the next waypoint, range, and VMG — and updates live from the
// NavDataStore's NavigationData (the APB/RMB output of RouteNavigator). Below the
// numbers is a CDI-style graphic: a heading-up compass ring marking heading and
// course-to-steer, with a vertical needle that deflects with cross-track error.
//
// A child of the chart view so it floats on the chart. The user can drag it
// anywhere within the view to reposition it; it clamps itself inside the view on
// drag and when the view is resized. It shows itself when navigation becomes
// active and hides when it stops, so the host just constructs one and forgets it.
class NavDisplayWindow : public QFrame {
    Q_OBJECT
public:
    explicit NavDisplayWindow(const NavDataStore* store, QWidget* parent);

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;   // parent resize -> clamp

private slots:
    void refresh();

private:
    void clampIntoParent();
    void positionDefault();   // top-right of the view, first time shown

    const NavDataStore* store_ = nullptr;
    QLabel* destLabel_    = nullptr;
    QLabel* xteLabel_     = nullptr;
    QLabel* steerLabel_   = nullptr;
    QLabel* bearingLabel_ = nullptr;
    QLabel* rangeLabel_   = nullptr;
    QLabel* vmgLabel_     = nullptr;
    QLabel* txDot_        = nullptr;   // green = APB/XTE/RMB/RMC on the wire, grey = not transmitting
    QLabel* txLabel_      = nullptr;
    QLabel* txDot2k_      = nullptr;   // NMEA 2000 nav PGNs: green = on the wire, grey = not transmitting
    QLabel* txLabel2k_    = nullptr;
    CdiWidget* cdi_       = nullptr;   // course-deviation graphic

    QPoint dragOffset_;          // grab point within the widget while dragging
    bool   dragging_ = false;
    bool   placed_   = false;    // false until first shown -> set default position
};
