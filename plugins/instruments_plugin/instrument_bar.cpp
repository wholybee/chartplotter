#include "instrument_bar.hpp"
#include "instrument_tile.hpp"
#include "nav_data_store.hpp"

#include <QBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QShowEvent>
#include <QEvent>
#include <algorithm>

InstrumentBar::InstrumentBar(const NavDataStore* store, QWidget* parent)
    : QFrame(parent), store_(store) {
    setObjectName(QStringLiteral("InstrumentBar"));
    setCursor(Qt::OpenHandCursor);
    setStyleSheet(QStringLiteral(
        "#InstrumentBar{ background: rgba(30,34,40,235);"
        " border:1px solid rgba(255,255,255,40); border-radius:8px; }"
        "QLabel{ color:#e6e9ee; background:transparent; border:none; }"));

    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(8, 8, 8, 8);
    layout_->setSpacing(4);

    if (store_)
        connect(store_, &NavDataStore::ownshipChanged, this, &InstrumentBar::refresh);
    if (parent)
        parent->installEventFilter(this);

    rebuild();
    hide();   // the host shows it once configured / enabled
}

void InstrumentBar::setHorizontal(bool horizontal) {
    if (horizontal == horizontal_ && !tiles_.isEmpty()) return;
    horizontal_ = horizontal;
    rebuild();
}

void InstrumentBar::setScale(double scale) {
    scale_ = std::clamp(scale, 0.4, 4.0);
    for (InstrumentTile* t : tiles_) t->setScale(scale_);
    layout_->setSpacing(std::max(2, qRound(4.0 * scale_)));
    const int m = std::max(4, qRound(8.0 * scale_));
    layout_->setContentsMargins(m, m, m, m);
    adjustSize();
    clampIntoParent();
}

void InstrumentBar::setInstruments(const QList<InstrumentDef>& defs) {
    defs_ = defs;
    rebuild();
}

void InstrumentBar::restorePosition(QPoint p) {
    if (p.x() < 0 || p.y() < 0) return;   // no saved position
    savedPos_ = p;
    placed_ = true;
    // Don't move/clamp yet: at plugin-init time the parent chart view isn't laid
    // out, so clamping would snap the bar to (0,0). Apply when the parent is sized
    // (first show / resize); see tryApplySavedPosition().
    tryApplySavedPosition();
}

void InstrumentBar::tryApplySavedPosition() {
    if (savedPos_.x() < 0 || !parentWidget()) return;
    const QRect pr = parentWidget()->rect();
    if (pr.width() <= width() || pr.height() <= height()) return;   // parent not sized yet
    move(savedPos_);
    clampIntoParent();
    savedPos_ = QPoint(-1, -1);   // consumed; further layout just clamps the live position
}

void InstrumentBar::rebuild() {
    // Tear down the existing contents (tiles + any placeholder) and repopulate.
    QLayoutItem* item = nullptr;
    while ((item = layout_->takeAt(0)) != nullptr) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }
    tiles_.clear();

    layout_->setDirection(horizontal_ ? QBoxLayout::LeftToRight
                                      : QBoxLayout::TopToBottom);
    layout_->setSpacing(std::max(2, qRound(4.0 * scale_)));
    const int m = std::max(4, qRound(8.0 * scale_));
    layout_->setContentsMargins(m, m, m, m);

    if (defs_.isEmpty()) {
        auto* hint = new QLabel(QStringLiteral("No instruments selected.\n"
                                               "Settings ▸ Plugin Settings ▸ Instruments"), this);
        hint->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // keep the bar draggable
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet(QStringLiteral("font-size:12px; color: rgba(230,233,238,150);"));
        layout_->addWidget(hint);
    } else {
        for (const InstrumentDef& d : defs_) {
            auto* tile = new InstrumentTile(d, store_, this);
            tile->setScale(scale_);
            layout_->addWidget(tile);
            tiles_.push_back(tile);
        }
    }

    // A child added to an *already visible* parent is not shown automatically
    // (Qt), and a hidden item collapses to zero size in the box layout — so on an
    // orientation flip (rebuild while the bar is visible) the new tiles stayed
    // hidden and the bar shrank to nothing. Show the freshly added items, then
    // activate the layout so adjustSize() measures the correct dimensions.
    for (int i = 0; i < layout_->count(); ++i)
        if (QWidget* w = layout_->itemAt(i)->widget()) w->show();
    layout_->activate();
    adjustSize();
    if (savedPos_.x() >= 0) tryApplySavedPosition();
    if (savedPos_.x() < 0) {              // no pending restore
        if (placed_) clampIntoParent();
        else if (isVisible()) { positionDefault(); placed_ = true; }
    }
    if (isVisible()) raise();             // keep it above the chart after the rebuild
    refresh();
}

void InstrumentBar::refresh() {
    for (InstrumentTile* t : tiles_) t->refresh();
}

void InstrumentBar::showEvent(QShowEvent* e) {
    QFrame::showEvent(e);
    adjustSize();
    if (savedPos_.x() >= 0) tryApplySavedPosition();   // first show: parent is now laid out
    if (savedPos_.x() < 0) {                            // no pending restore
        if (!placed_) { positionDefault(); placed_ = true; }
        else          { clampIntoParent(); }
    }
    raise();
    refresh();
}

void InstrumentBar::positionDefault() {
    if (!parentWidget()) return;
    adjustSize();
    const QRect pr = parentWidget()->rect();
    int x, y;
    if (horizontal_) {
        // Bottom-centre, clear of the status bar / chart scale.
        x = (pr.width() - width()) / 2;
        y = pr.height() - height() - 16;
    } else {
        // Right edge, vertically centred.
        x = pr.width() - width() - 12;
        y = (pr.height() - height()) / 2;
    }
    move(std::max(12, x), std::max(12, y));
}

void InstrumentBar::clampIntoParent() {
    if (!parentWidget()) return;
    const QRect pr = parentWidget()->rect();
    const int x = std::clamp(pos().x(), 0, std::max(0, pr.width()  - width()));
    const int y = std::clamp(pos().y(), 0, std::max(0, pr.height() - height()));
    move(x, y);
}

// Drag handling mirrors the core nav display window: the gesture is claimed so it
// doesn't fall through to the chart view underneath (which would pan the chart).
void InstrumentBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = e->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QFrame::mousePressEvent(e);
}

void InstrumentBar::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        move(mapToParent(e->position().toPoint()) - dragOffset_);
        clampIntoParent();
        placed_ = true;
        e->accept();
        return;
    }
    QFrame::mouseMoveEvent(e);
}

void InstrumentBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        emit positionChanged(pos());
        e->accept();
        return;
    }
    QFrame::mouseReleaseEvent(e);
}

bool InstrumentBar::eventFilter(QObject* obj, QEvent* e) {
    if (obj == parentWidget() && e->type() == QEvent::Resize && isVisible()) {
        if (savedPos_.x() >= 0) tryApplySavedPosition();   // parent now has a real size
        clampIntoParent();
    }
    return QFrame::eventFilter(obj, e);
}
