#include "nav_display_window.hpp"
#include "nav_data_store.hpp"

#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QMouseEvent>
#include <QEvent>
#include <QPainter>
#include <QPolygonF>
#include <QtMath>
#include <algorithm>
#include <cmath>

// ---- CDI graphic -----------------------------------------------------------
//
// A heading-up course-deviation indicator drawn from the live NavigationData +
// ownship state. The compass ring rotates so the boat heading sits under the
// fixed lubber line at the top (with the numeric heading); a magenta marker on
// the ring shows the course-to-steer; a green vertical needle slides left/right
// with cross-track error (deflecting toward the side you must steer), against a
// row of deviation dots. Transparent to mouse events so the parent window stays
// draggable when grabbed here.
class CdiWidget : public QWidget {
public:
    CdiWidget(const NavDataStore* store, QWidget* parent)
        : QWidget(parent), store_(store) {
        setFixedSize(186, 192);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    const NavDataStore* store_ = nullptr;
};

namespace {
constexpr double kCdiD2R = 3.14159265358979323846 / 180.0;
constexpr double kCdiFullScaleNm = 0.20;   // cross-track error at full needle deflection

double normalize360(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}
}  // namespace

void CdiWidget::paintEvent(QPaintEvent*) {
    if (!store_) return;
    const NavigationData& n = store_->navigation();
    const OwnshipState&   os = store_->ownship();
    const bool magMode = (n.bearingUnits == QLatin1Char('M'));
    const bool haveFix = os.latitudeDeg.valid() && os.longitudeDeg.valid();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Reserve a band above the ring so the numeric heading (and lubber) clear the
    // top edge, and use a slightly smaller ring radius. Fonts are unchanged.
    constexpr double kTopReserve = 30.0;     // heading readout above the ring
    constexpr double kBottomReserve = 20.0;  // full-scale label below the ring
    const double R = (height() - kTopReserve - kBottomReserve) / 2.0;   // compass ring radius
    const QPointF C(width() / 2.0, kTopReserve + R);

    // Boat heading, in the same reference (true/magnetic) as the course-to-steer
    // so their relative angle is exact. Prefer a real heading, then COG, then
    // north-up as a last resort.
    double heading = 0.0;
    bool haveHeading = false;
    const double var = os.variationDeg.valid() ? os.variationDeg.value : 0.0;
    const bool haveVar = os.variationDeg.valid();
    if (magMode) {
        if (os.headingDegMag.valid())            { heading = os.headingDegMag.value; haveHeading = true; }
        else if (os.headingDegTrue.valid() && haveVar) { heading = normalize360(os.headingDegTrue.value - var); haveHeading = true; }
    } else {
        if (os.headingDegTrue.valid())           { heading = os.headingDegTrue.value; haveHeading = true; }
        else if (os.headingDegMag.valid() && haveVar)  { heading = normalize360(os.headingDegMag.value + var); haveHeading = true; }
    }
    if (!haveHeading && os.cogDegTrue.valid()) {
        heading = os.cogDegTrue.value;
        if (magMode && haveVar) heading = normalize360(heading - var);
        haveHeading = true;
    }

    // Screen point for a compass bearing, in the heading-up frame.
    auto pt = [&](double bearing, double r) -> QPointF {
        const double a = (bearing - heading) * kCdiD2R;   // clockwise from the top
        return QPointF(C.x() + r * std::sin(a), C.y() - r * std::cos(a));
    };

    const QColor ringCol(210, 214, 220);
    const QColor dimCol(150, 155, 163);
    const QColor courseCol(224, 64, 251);   // magenta course-to-steer
    const QColor needleCol(57, 211, 83);     // green deviation needle
    const QColor lubberCol(255, 210, 74);    // yellow heading/lubber

    // Compass ring.
    p.setPen(QPen(ringCol, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(C, R, R);

    // Ticks every 15 degrees (longer/brighter at the cardinals).
    for (int b = 0; b < 360; b += 15) {
        const bool major = (b % 90 == 0);
        const bool mid   = (b % 45 == 0);
        const double inset = major ? 11.0 : (mid ? 8.0 : 5.0);
        p.setPen(QPen(major ? ringCol : dimCol, major ? 2.0 : 1.0));
        p.drawLine(pt(b, R), pt(b, R - inset));
    }

    // Cardinal letters.
    QFont f = p.font();
    f.setPixelSize(11);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(235, 238, 244));
    const char* card[4] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; ++i) {
        const QPointF lp = pt(i * 90.0, R - 22);
        p.drawText(QRectF(lp.x() - 9, lp.y() - 9, 18, 18), Qt::AlignCenter,
                   QString::fromLatin1(card[i]));
    }

    // Fixed lubber line at the top + numeric heading: this is the boat heading.
    p.setPen(Qt::NoPen);
    p.setBrush(lubberCol);
    QPolygonF lub;
    lub << QPointF(C.x(), C.y() - R + 3)
        << QPointF(C.x() - 6, C.y() - R - 7)
        << QPointF(C.x() + 6, C.y() - R - 7);
    p.drawPolygon(lub);
    if (haveHeading) {
        f.setPixelSize(13);
        p.setFont(f);
        p.setPen(lubberCol);
        p.drawText(QRectF(C.x() - 26, C.y() - R - 26, 52, 15), Qt::AlignCenter,
                   QString::number(qRound(normalize360(heading))) + QChar(0x00B0));
    }

    // Course-to-steer marker: a magenta triangle on the ring pointing inward.
    if (haveFix) {
        const double crs = n.bearingPresentToDestDeg;
        const QPointF tip = pt(crs, R - 13);
        const QPointF b1  = pt(crs - 4.0, R - 1);
        const QPointF b2  = pt(crs + 4.0, R - 1);
        p.setPen(Qt::NoPen);
        p.setBrush(courseCol);
        QPolygonF tri;
        tri << tip << b1 << b2;
        p.drawPolygon(tri);
    }

    // Deviation dots along the horizontal centreline (±2 = full scale).
    const double half = R * 0.62;
    p.setPen(Qt::NoPen);
    p.setBrush(dimCol);
    for (int k = -2; k <= 2; ++k) {
        if (k == 0) continue;
        p.drawEllipse(QPointF(C.x() + k * half / 2.0, C.y()), 2.2, 2.2);
    }
    // Centre index.
    p.setPen(QPen(dimCol, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(C, 3.0, 3.0);

    // Cross-track needle: vertical line that slides toward the side to steer.
    // Guard against a non-finite XTE: drawing at a NaN/Inf coordinate crashes the
    // raster paint engine. (The source of any such value is clamped upstream in
    // geonav::distanceNm; this is belt-and-suspenders so bad data can never paint.)
    if (haveFix && std::isfinite(n.xteNm)) {
        const double mag = std::clamp(n.xteNm / kCdiFullScaleNm, 0.0, 1.0) * half;
        const double dir = (n.steerDirection == QLatin1Char('L')) ? -1.0 : 1.0;
        const double nx  = C.x() + dir * mag;
        const double nh  = R * 0.58;
        p.setPen(QPen(needleCol, 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(nx, C.y() - nh), QPointF(nx, C.y() + nh));
    }

    // Full-scale note, below the ring (same size as the readout's small labels).
    f.setPixelSize(11);
    f.setBold(false);
    p.setFont(f);
    p.setPen(dimCol);
    p.drawText(QRectF(C.x() - 60, C.y() + R + 4, 120, 15), Qt::AlignCenter,
               QStringLiteral("±%1 nm FS").arg(kCdiFullScaleNm, 0, 'f', 2));
}

namespace {
// Right-aligned bold value label.
QLabel* valueLabel(QWidget* parent) {
    auto* l = new QLabel(parent);
    l->setStyleSheet(QStringLiteral("font-size:15px; font-weight:600;"));
    return l;
}
// Dim caption in the left column.
QLabel* captionLabel(QWidget* parent, const QString& text) {
    auto* l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("font-size:12px; color: rgba(230,233,238,150);"));
    return l;
}
}  // namespace

NavDisplayWindow::NavDisplayWindow(const NavDataStore* store, QWidget* parent)
    : QFrame(parent), store_(store) {
    setObjectName(QStringLiteral("NavDisplayWindow"));
    setCursor(Qt::OpenHandCursor);
    setStyleSheet(QStringLiteral(
        "#NavDisplayWindow{ background: rgba(30,34,40,235);"
        " border:1px solid rgba(255,255,255,40); border-radius:8px; }"
        "QLabel{ color:#e6e9ee; background:transparent; border:none; }"));

    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(12, 8, 12, 10);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(4);

    destLabel_ = new QLabel(this);
    destLabel_->setStyleSheet(QStringLiteral("font-size:12px; color: rgba(230,233,238,175);"));
    grid->addWidget(destLabel_, 0, 0, 1, 3);

    grid->addWidget(captionLabel(this, QStringLiteral("XTE")), 1, 0);
    xteLabel_ = valueLabel(this);
    grid->addWidget(xteLabel_, 1, 1);
    steerLabel_ = new QLabel(this);
    steerLabel_->setStyleSheet(QStringLiteral("font-size:16px; font-weight:700; color:#ffd24a;"));
    steerLabel_->setAlignment(Qt::AlignCenter);
    grid->addWidget(steerLabel_, 1, 2);

    grid->addWidget(captionLabel(this, QStringLiteral("Bearing")), 2, 0);
    bearingLabel_ = valueLabel(this);
    grid->addWidget(bearingLabel_, 2, 1, 1, 2);

    grid->addWidget(captionLabel(this, QStringLiteral("Range")), 3, 0);
    rangeLabel_ = valueLabel(this);
    grid->addWidget(rangeLabel_, 3, 1, 1, 2);

    grid->addWidget(captionLabel(this, QStringLiteral("VMG")), 4, 0);
    vmgLabel_ = valueLabel(this);
    grid->addWidget(vmgLabel_, 4, 1, 1, 2);

    // Transmit indicator: a small dot (green = APB/RMB/RMC actually going out on
    // a live NMEA 0183 link, grey = not transmitting) plus a static label.
    txDot_ = new QLabel(this);
    txDot_->setFixedSize(12, 12);
    txLabel_ = new QLabel(QStringLiteral("APB · XTE · RMB · RMC"), this);
    txLabel_->setStyleSheet(QStringLiteral("font-size:11px; color: rgba(230,233,238,150);"));
    grid->addWidget(txDot_, 5, 0, Qt::AlignCenter);
    grid->addWidget(txLabel_, 5, 1, 1, 2);

    // Second transmit indicator for the NMEA 2000 navigation PGNs.
    txDot2k_ = new QLabel(this);
    txDot2k_->setFixedSize(12, 12);
    txLabel2k_ = new QLabel(QStringLiteral("PGN 129283 · 129284 · 129285"), this);
    txLabel2k_->setStyleSheet(QStringLiteral("font-size:11px; color: rgba(230,233,238,150);"));
    grid->addWidget(txDot2k_, 6, 0, Qt::AlignCenter);
    grid->addWidget(txLabel2k_, 6, 1, 1, 2);

    // CDI graphic, below the numeric readout, separated by a thin divider.
    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("color: rgba(255,255,255,40);"));
    grid->addWidget(sep, 7, 0, 1, 3);
    cdi_ = new CdiWidget(store_, this);
    grid->addWidget(cdi_, 8, 0, 1, 3, Qt::AlignHCenter);

    if (store_) {
        connect(store_, &NavDataStore::navigationChanged, this, &NavDisplayWindow::refresh);
        // The senders update their transmit status just after navigationChanged
        // (or whenever a link connects/drops), so refresh on that too to recolour
        // the indicators without waiting for the next navigation tick.
        connect(store_, &NavDataStore::navTransmitStateChanged, this, &NavDisplayWindow::refresh);
    }
    if (parent) parent->installEventFilter(this);

    hide();   // shown only while navigating
    refresh();
}

void NavDisplayWindow::refresh() {
    if (!store_) return;
    const NavigationData& n = store_->navigation();
    if (!n.active) { hide(); return; }

    destLabel_->setText(QStringLiteral("To: %1")
        .arg(n.destinationWaypointId.isEmpty() ? QStringLiteral("—") : n.destinationWaypointId));
    xteLabel_->setText(QString::number(n.xteNm, 'f', 3) + QStringLiteral(" nm"));
    steerLabel_->setText(QString(n.steerDirection));
    bearingLabel_->setText(QString::number(n.bearingPresentToDestDeg, 'f', 1)
                           + QStringLiteral("° ") + QString(n.bearingUnits));
    rangeLabel_->setText(QString::number(n.rangeToDestNm, 'f', 2) + QStringLiteral(" nm"));
    vmgLabel_->setText(QString::number(n.closingVelocityKn, 'f', 1) + QStringLiteral(" kn"));

    // Transmit indicators: green only while the link is actually putting the nav
    // data on the wire, as reported by that link's sender (route active + link
    // connected + not suppressed by the loop guard). Grey otherwise, with the
    // tooltip naming the reason — the loop guard, or simply that the link is not
    // active.
    auto updateTxDot = [&](QLabel* dot, const QString& sourceId,
                           const QString& what) {
        const bool transmitting = store_->navTransmitting(sourceId);
        const bool suppressed =
            n.source.compare(sourceId, Qt::CaseInsensitive) == 0;
        dot->setStyleSheet(QStringLiteral("background:%1; border-radius:6px;")
            .arg(transmitting ? QStringLiteral("#2e9e44")    // green: on the wire
                              : QStringLiteral("#555b63"))); // grey: not transmitting
        if (transmitting)
            dot->setToolTip(QStringLiteral("Transmitting %1").arg(what));
        else if (suppressed)
            dot->setToolTip(QStringLiteral(
                "Not transmitting %1: navigation data came from this connection "
                "(loop guard)").arg(what));
        else
            dot->setToolTip(QStringLiteral(
                "Not transmitting %1: the connection is not active").arg(what));
    };
    updateTxDot(txDot_,   QStringLiteral("nmea0183"),
                QStringLiteral("APB/XTE/RMB/RMC on the NMEA 0183 connection"));
    updateTxDot(txDot2k_, QStringLiteral("nmea2000"),
                QStringLiteral("PGN 129283/129284/129285 on the NMEA 2000 connection"));

    if (cdi_) cdi_->update();   // repaint the course-deviation graphic

    const bool wasVisible = isVisible();
    adjustSize();
    if (!placed_) { positionDefault(); placed_ = true; }
    show();
    // Only raise on the hidden->shown transition, so a live update never pops the
    // readout above an open side menu (also a child of the view).
    if (!wasVisible) raise();
}

void NavDisplayWindow::positionDefault() {
    if (!parentWidget()) return;
    adjustSize();
    const int x = parentWidget()->width() - width() - 12;
    move(std::max(12, x), 12);
}

void NavDisplayWindow::clampIntoParent() {
    if (!parentWidget()) return;
    const QRect pr = parentWidget()->rect();
    const int x = std::clamp(pos().x(), 0, std::max(0, pr.width()  - width()));
    const int y = std::clamp(pos().y(), 0, std::max(0, pr.height() - height()));
    move(x, y);
}

// The drag gesture is claimed (e->accept(), no base call) so it doesn't fall
// through to the ChartView underneath — the default QWidget handlers ignore the
// event, which would propagate it to the parent and pan the chart while moving
// the window.
void NavDisplayWindow::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        dragOffset_ = e->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QFrame::mousePressEvent(e);
}

void NavDisplayWindow::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        move(mapToParent(e->position().toPoint()) - dragOffset_);
        clampIntoParent();
        e->accept();
        return;
    }
    QFrame::mouseMoveEvent(e);
}

void NavDisplayWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        e->accept();
        return;
    }
    QFrame::mouseReleaseEvent(e);
}

bool NavDisplayWindow::eventFilter(QObject* obj, QEvent* e) {
    if (obj == parentWidget() && e->type() == QEvent::Resize && isVisible())
        clampIntoParent();
    return QFrame::eventFilter(obj, e);
}
