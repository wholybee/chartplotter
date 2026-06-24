#pragma once
#include <QFrame>
#include <QPoint>
#include <QList>
#include "instrument_config.hpp"

class NavDataStore;
class InstrumentTile;
class QBoxLayout;

// The floating instrument bar: a row (horizontal) or column (vertical) of
// instrument tiles, styled like the core nav display window so it sits over the
// chart consistently. A child of the chart view, it can be dragged anywhere
// within the view and clamps itself on drag and on view resize.
//
// It subscribes to the nav store's ownshipChanged() and refreshes its tiles, so
// values (and their freshness) stay live. The host constructs one, feeds it the
// chosen instruments / orientation / scale, and shows or hides it.
class InstrumentBar : public QFrame {
    Q_OBJECT
public:
    InstrumentBar(const NavDataStore* store, QWidget* parent);

    void setHorizontal(bool horizontal);
    void setScale(double scale);
    void setInstruments(const QList<InstrumentDef>& defs);   // rebuilds the tiles

    // Restore a previously saved top-left position (within the parent). An
    // invalid point leaves the bar at its default placement.
    void restorePosition(QPoint p);

signals:
    void positionChanged(QPoint topLeft);   // emitted after a drag, for persistence

protected:
    void showEvent(QShowEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;   // parent resize -> clamp

private slots:
    void refresh();

private:
    void rebuild();
    void clampIntoParent();
    void positionDefault();
    // Move to the saved position once the parent has a real (laid-out) size, then
    // clear it. A no-op while the parent is still unsized (e.g. during plugin
    // init, before the window is shown), so the position isn't clamped to (0,0).
    void tryApplySavedPosition();

    const NavDataStore*    store_ = nullptr;
    QBoxLayout*            layout_ = nullptr;
    QList<InstrumentDef>   defs_;
    QList<InstrumentTile*> tiles_;
    bool   horizontal_ = true;
    double scale_      = 1.0;

    QPoint dragOffset_;
    bool   dragging_ = false;
    bool   placed_   = false;   // false until first positioned
    QPoint savedPos_{-1, -1};   // pending restored position, applied once parent is sized
};
