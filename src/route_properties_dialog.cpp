#include "route_properties_dialog.hpp"
#include "units.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"
#include "route_colors.hpp"
#include "geo_nav.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QCheckBox>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QWidget>
#include <QMessageBox>
#include <QDateTime>
#include <QPixmap>
#include <QIcon>
#include <cmath>

namespace {
// Local wall-clock format for the planned-departure field. Stored internally as
// UTC; shown and entered in local time (the skipper reads it against the cockpit
// clock), matching how track names are localised.
const QString kDepartFormat = QStringLiteral("yyyy-MM-dd hh:mm");

// The colour the overlay uses when a route has no explicit DisplayColor; shown as
// the "Auto" swatch so the picker previews what "default" looks like.
const QColor kDefaultRouteColor(0xB0, 0x30, 0xD0);

// Coordinate entry field, shown in the user's selected lat/lon format. Free text
// (no validator) since formats like DMS aren't plain numbers; the input is parsed
// tolerantly on commit via units::parseLatitude/Longitude.
QLineEdit* makeCoordEdit(double value, bool isLat) {
    const theme::MenuPalette& t = theme::menu();
    const theme::InputPalette& in = theme::input();
    auto* e = new QLineEdit;
    e->setMinimumHeight(36);
    e->setText(isLat ? units::formatLatitude(value) : units::formatLongitude(value));
    e->setStyleSheet(QStringLiteral(
        "QLineEdit{ font-size:14px; padding:4px 8px; background:%1; color:%2;"
        " border:1px solid %3; border-radius:6px; }"
        "QLineEdit:focus{ border:1px solid %4; }")
        .arg(in.fieldBg, in.fg, in.border, t.accent));
    return e;
}
}  // namespace

RoutePropertiesDialog::RoutePropertiesDialog(const Route& route, DistanceUnit distUnit,
                                             QWidget* parent)
    : QDialog(parent), work_(route), distUnit_(distUnit) {
    setWindowTitle(QStringLiteral("Route Properties"));
    resize(520, 720);
    selectedColor_ = work_.displayColor;

    const theme::MenuPalette& t = theme::menu();
    // Modeless + self-deleting (WA_DeleteOnClose), single-instance; dismiss via
    // close() so the dialog is destroyed and the owner's pointer clears.
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Route Properties"), true);
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Details")));

    auto* body = new QWidget;
    body->setStyleSheet(QStringLiteral("QLabel{ color:%1; font-size:14px; }").arg(t.actionFg));
    auto* bcol = new QVBoxLayout(body);
    bcol->setContentsMargins(16, 4, 16, 8);
    bcol->setSpacing(8);
    auto* form = new QFormLayout;
    form->setSpacing(8);
    nameEdit_ = new QLineEdit(work_.name);
    dialogchrome::styleLineEdit(nameEdit_);
    descEdit_ = new QLineEdit(work_.description);
    dialogchrome::styleLineEdit(descEdit_);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descEdit_);

    // Voyage plan: planned speed + departure, with the arrival derived below.
    speedEdit_ = new QLineEdit;
    dialogchrome::styleLineEdit(speedEdit_);
    if (work_.plannedSpeedKts > 0.0)
        speedEdit_->setText(QString::number(
            units::speedFromKnots(work_.plannedSpeedKts, distUnit_), 'f', 1));
    speedEdit_->setPlaceholderText(QStringLiteral("e.g. 5.5"));
    form->addRow(QStringLiteral("Planned speed (%1)").arg(units::speedUnitKey(distUnit_)),
                 speedEdit_);

    // Departure: a checkbox toggles whether one is planned; the picker offers a
    // calendar popup for the date and spin sections for the time, so nothing has
    // to be typed in a set format. Stored as UTC, shown/edited in local time.
    departEnable_ = new QCheckBox;
    departEnable_->setCursor(Qt::PointingHandCursor);
    departEnable_->setStyleSheet(QStringLiteral("QCheckBox::indicator{ width:22px; height:22px; }"));
    departEdit_ = new QDateTimeEdit;
    departEdit_->setCalendarPopup(true);
    departEdit_->setDisplayFormat(kDepartFormat);
    departEdit_->setMinimumHeight(44);
    {
        const theme::InputPalette& in = theme::input();
        departEdit_->setStyleSheet(QStringLiteral(
            "QDateTimeEdit{ font-size:16px; padding:6px 10px; background:%1; color:%2;"
            " border:1px solid %3; border-radius:6px; }"
            "QDateTimeEdit:focus{ border:1px solid %4; }"
            "QDateTimeEdit:disabled{ color:%5; }"
            "QDateTimeEdit::up-button, QDateTimeEdit::down-button{ width:22px; }")
            .arg(in.fieldBg, in.fg, in.border, t.accent, theme::textMuted()));
    }
    if (work_.plannedDepartureUtc.isValid()) {
        departEnable_->setChecked(true);
        departEdit_->setDateTime(work_.plannedDepartureUtc.toLocalTime());
    } else {
        departEnable_->setChecked(false);
        departEdit_->setDateTime(QDateTime::currentDateTime());
        departEdit_->setEnabled(false);
    }
    auto* departRow = new QWidget;
    auto* departHl = new QHBoxLayout(departRow);
    departHl->setContentsMargins(0, 0, 0, 0);
    departHl->setSpacing(8);
    departHl->addWidget(departEnable_);
    departHl->addWidget(departEdit_, 1);
    form->addRow(QStringLiteral("Departure"), departRow);

    arrivalLabel_ = new QLabel;
    arrivalLabel_->setWordWrap(true);
    arrivalLabel_->setStyleSheet(QStringLiteral("color:%1; font-size:13px;")
                                     .arg(theme::textMuted()));
    form->addRow(QStringLiteral("Arrival"), arrivalLabel_);

    // Route colour: a compact dropdown (Auto + named swatches).
    form->addRow(QStringLiteral("Route colour"), buildColorRow());

    bcol->addLayout(form);
    panelCol->addWidget(body);

    connect(speedEdit_,  &QLineEdit::textChanged, this, [this] { updateArrival(); });
    connect(departEdit_, &QDateTimeEdit::dateTimeChanged, this, [this] { updateArrival(); });
    connect(departEnable_, &QCheckBox::toggled, this, [this](bool on) {
        departEdit_->setEnabled(on);
        updateArrival();
    });

    // Points header doubles as the section-header strip; rebuildRows updates the count.
    countLabel_ = new QLabel;
    countLabel_->setStyleSheet(QStringLiteral(
        "font-size:12px; font-weight:600; color:%1;"
        "padding:14px 16px 6px 16px; background:%2;").arg(t.headerFg, t.headerBg));
    panelCol->addWidget(countLabel_);

    scrollArea_ = new QScrollArea;
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setStyleSheet(QStringLiteral(
        "QScrollArea, QScrollArea > QWidget > QWidget { background:%1; }").arg(t.panelBg));
    rowContainer_ = new QWidget;
    rowLayout_ = new QVBoxLayout(rowContainer_);
    rowLayout_->setContentsMargins(0, 0, 0, 0);
    rowLayout_->setSpacing(0);
    rowLayout_->addStretch(1);
    scrollArea_->setWidget(rowContainer_);
    // Drag-to-scroll, consistent with the list dialogs. Taps still reach the
    // per-row fields and buttons.
    QScroller::grabGesture(scrollArea_->viewport(), QScroller::LeftMouseButtonGesture);
    panelCol->addWidget(scrollArea_, 1);

    auto* btnBar = new QWidget;
    auto* btnRow = new QHBoxLayout(btnBar);
    btnRow->setContentsMargins(16, 8, 16, 16);
    btnRow->setSpacing(10);
    btnRow->addStretch(1);
    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    auto* okBtn     = new QPushButton(QStringLiteral("OK"));
    dialogchrome::styleOutlinedButton(cancelBtn);
    dialogchrome::styleAccentButton(okBtn);
    okBtn->setDefault(true);
    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(okBtn);
    panelCol->addWidget(btnBar);

    // Modeless + self-deleting: Cancel closes (destroys) rather than just hiding.
    connect(cancelBtn, &QPushButton::clicked, this, &QWidget::close);
    connect(okBtn,     &QPushButton::clicked, this, &RoutePropertiesDialog::onOk);

    rebuildRows();
}

void RoutePropertiesDialog::rebuildRows() {
    // Tear down existing rows (cheap: point counts are modest, not on a timer).
    for (Row& r : rows_) r.widget->deleteLater();
    rows_.clear();

    const theme::MenuPalette& t = theme::menu();
    countLabel_->setText(QStringLiteral("Points (%1)").arg(work_.points.size()));

    for (int i = 0; i < work_.points.size(); ++i) {
        const RoutePoint& p = work_.points[i];
        auto* w = new QWidget(rowContainer_);
        w->setObjectName(QStringLiteral("RoutePtRow"));
        // Scope the divider to the row (#id) so it doesn't cascade to children.
        w->setStyleSheet(QStringLiteral("#RoutePtRow{ border-bottom:1px solid %1; }")
                             .arg(t.separator));
        auto* hl = new QHBoxLayout(w);
        hl->setContentsMargins(8, 4, 8, 4);
        hl->setSpacing(6);

        auto* num = new QLabel(QString::number(i + 1), w);
        num->setFixedWidth(28);
        num->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        num->setStyleSheet(QStringLiteral("border:none; font-weight:600; color:%1;")
                               .arg(t.actionFg));
        hl->addWidget(num);

        Row row;
        row.widget = w;
        row.lat = makeCoordEdit(p.lat, /*isLat=*/true);
        row.lon = makeCoordEdit(p.lon, /*isLat=*/false);
        row.lat->setToolTip(QStringLiteral("Latitude"));
        row.lon->setToolTip(QStringLiteral("Longitude"));
        hl->addWidget(row.lat, 1);
        hl->addWidget(row.lon, 1);

        auto* editBtn = new QPushButton(QStringLiteral("Edit"), w);
        dialogchrome::styleOutlinedButton(editBtn);
        editBtn->setToolTip(QStringLiteral("Drag this point on the chart"));
        connect(editBtn, &QPushButton::clicked, this, [this, i] { emit editPointRequested(i); });
        hl->addWidget(editBtn);

        auto* delBtn = new QPushButton(QStringLiteral("Delete"), w);
        dialogchrome::styleOutlinedButton(delBtn);
        connect(delBtn, &QPushButton::clicked, this, [this, i] { onDeletePoint(i); });
        hl->addWidget(delBtn);

        rowLayout_->insertWidget(rowLayout_->count() - 1, w);   // before the stretch
        rows_.push_back(row);
    }
    updateArrival();   // leg count/length changed -> refresh the derived readout
}

// ---- colour picker ---------------------------------------------------------

namespace {
// A small solid-colour swatch icon for a combo item.
QIcon swatchIcon(const QColor& c) {
    QPixmap pm(28, 18);
    pm.fill(c);
    return QIcon(pm);
}
}  // namespace

QWidget* RoutePropertiesDialog::buildColorRow() {
    const theme::MenuPalette& t = theme::menu();
    const theme::InputPalette& in = theme::input();
    colorCombo_ = new QComboBox;
    colorCombo_->setMinimumHeight(44);
    colorCombo_->setCursor(Qt::PointingHandCursor);
    colorCombo_->setIconSize(QSize(28, 18));
    colorCombo_->setStyleSheet(QStringLiteral(
        "QComboBox{ font-size:16px; padding:6px 10px; background:%1; color:%2;"
        " border:1px solid %3; border-radius:6px; }"
        "QComboBox:focus{ border:1px solid %4; }"
        "QComboBox::drop-down{ border:none; width:28px; }"
        "QComboBox QAbstractItemView{ background:%1; color:%2;"
        " selection-background-color:%4; selection-color:%5; }")
        .arg(in.fieldBg, in.fg, in.border, t.accent, t.titleFg));

    // "Auto" (no explicit colour) first, previewed in the default route colour,
    // then the named palette. The GPX name is stored in each item's data.
    colorCombo_->addItem(swatchIcon(kDefaultRouteColor), QStringLiteral("Auto (default)"),
                         QString());
    for (const routecolor::Entry& e : routecolor::palette())
        colorCombo_->addItem(swatchIcon(e.color), QString::fromLatin1(e.name),
                             QString::fromLatin1(e.name));

    selectColor(selectedColor_);   // reflect the route's stored colour
    connect(colorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { selectedColor_ = colorCombo_->currentData().toString(); });
    return colorCombo_;
}

void RoutePropertiesDialog::selectColor(const QString& gpxName) {
    selectedColor_ = gpxName;
    if (!colorCombo_) return;
    int idx = colorCombo_->findData(gpxName, Qt::UserRole, Qt::MatchFixedString);
    if (idx < 0 && !gpxName.isEmpty()) {
        // A colour imported from elsewhere that isn't in our palette: keep it
        // representable by adding it as its own item.
        colorCombo_->addItem(swatchIcon(routecolor::toColor(gpxName, kDefaultRouteColor)),
                             gpxName, gpxName);
        idx = colorCombo_->count() - 1;
    }
    colorCombo_->setCurrentIndex(idx < 0 ? 0 : idx);
}

// ---- derived arrival readout ----------------------------------------------

QDateTime RoutePropertiesDialog::departureUtc() const {
    if (!departEnable_ || !departEnable_->isChecked() || !departEdit_) return QDateTime();
    return departEdit_->dateTime().toUTC();   // picker holds local time
}

double RoutePropertiesDialog::totalDistanceNm() const {
    double nm = 0.0;
    for (int i = 1; i < work_.points.size(); ++i)
        nm += geonav::distanceNm(work_.points[i - 1].lat, work_.points[i - 1].lon,
                                 work_.points[i].lat, work_.points[i].lon);
    return nm;
}

void RoutePropertiesDialog::updateArrival() {
    if (!arrivalLabel_ || !speedEdit_ || !departEdit_) return;

    const double totalNm = totalDistanceNm();
    const QString distStr = units::formatDistance(totalNm, distUnit_);

    bool speedOk = false;
    const double speedDisp = speedEdit_->text().trimmed().toDouble(&speedOk);
    const double speedKts = speedOk ? units::knotsFromSpeed(speedDisp, distUnit_) : 0.0;

    // Local-time departure for the ETA display (invalid when none is planned).
    const QDateTime dep = (departEnable_ && departEnable_->isChecked())
        ? departEdit_->dateTime() : QDateTime();

    // Distance is always meaningful; the ETA needs a positive speed, and a clock
    // time needs a valid departure too.
    if (speedKts <= 0.0) {
        arrivalLabel_->setText(QStringLiteral("%1 total — enter a planned speed for ETA")
                                   .arg(distStr));
        return;
    }
    const double hours = totalNm / speedKts;
    const int totalMin = int(std::lround(hours * 60.0));
    const QString dur = QStringLiteral("%1h %2m").arg(totalMin / 60).arg(totalMin % 60);

    if (dep.isValid()) {
        const QDateTime eta = dep.addSecs(qint64(std::llround(hours * 3600.0)));
        arrivalLabel_->setText(QStringLiteral("%1 · %2 · arrive %3")
                                   .arg(distStr, dur, eta.toString(kDepartFormat)));
    } else {
        arrivalLabel_->setText(QStringLiteral("%1 · %2 underway").arg(distStr, dur));
    }
}

void RoutePropertiesDialog::commitFieldsToWorking() {
    work_.name = nameEdit_->text().trimmed();
    work_.description = descEdit_->text().trimmed();
    work_.displayColor = selectedColor_;
    {
        bool ok = false;
        const double v = speedEdit_->text().trimmed().toDouble(&ok);
        work_.plannedSpeedKts = (ok && v > 0.0) ? units::knotsFromSpeed(v, distUnit_) : 0.0;
        work_.plannedDepartureUtc = departureUtc();
    }
    for (int i = 0; i < int(rows_.size()) && i < work_.points.size(); ++i) {
        bool ok = false;
        const double la = units::parseLatitude(rows_[i].lat->text(), &ok);
        if (ok) work_.points[i].lat = la;             // keep old value if unparseable
        const double lo = units::parseLongitude(rows_[i].lon->text(), &ok);
        if (ok) work_.points[i].lon = lo;
    }
}

void RoutePropertiesDialog::onDeletePoint(int index) {
    if (index < 0 || index >= work_.points.size()) return;
    commitFieldsToWorking();          // preserve other edits before rebuilding
    work_.points.remove(index);
    rebuildRows();
}

Route RoutePropertiesDialog::currentRoute() const {
    Route r = work_;                  // keeps id / created / visible
    r.name = nameEdit_->text().trimmed();
    r.description = descEdit_->text().trimmed();
    r.displayColor = selectedColor_;
    {
        bool ok = false;
        const double v = speedEdit_->text().trimmed().toDouble(&ok);
        r.plannedSpeedKts = (ok && v > 0.0) ? units::knotsFromSpeed(v, distUnit_) : 0.0;
        r.plannedDepartureUtc = departureUtc();
    }
    for (int i = 0; i < int(rows_.size()) && i < r.points.size(); ++i) {
        bool ok = false;
        const double la = units::parseLatitude(rows_[i].lat->text(), &ok);
        if (ok) r.points[i].lat = la;                 // fall back to stored value
        const double lo = units::parseLongitude(rows_[i].lon->text(), &ok);
        if (ok) r.points[i].lon = lo;
    }
    return r;
}

void RoutePropertiesDialog::setRoute(const Route& route) {
    work_ = route;
    nameEdit_->setText(work_.name);
    descEdit_->setText(work_.description);
    speedEdit_->setText(work_.plannedSpeedKts > 0.0
        ? QString::number(units::speedFromKnots(work_.plannedSpeedKts, distUnit_), 'f', 1)
        : QString());
    const bool hasDep = work_.plannedDepartureUtc.isValid();
    departEnable_->setChecked(hasDep);
    departEdit_->setEnabled(hasDep);
    departEdit_->setDateTime(hasDep ? work_.plannedDepartureUtc.toLocalTime()
                                    : QDateTime::currentDateTime());
    selectColor(work_.displayColor);
    rebuildRows();
}

void RoutePropertiesDialog::onOk() {
    // Every coordinate field must parse (and be in range) in the current format.
    for (int i = 0; i < int(rows_.size()); ++i) {
        bool okLat = false, okLon = false;
        units::parseLatitude(rows_[i].lat->text(), &okLat);
        units::parseLongitude(rows_[i].lon->text(), &okLon);
        if (!okLat || !okLon) {
            QMessageBox::information(this, QStringLiteral("Route Properties"),
                QStringLiteral("Point %1 has an invalid latitude/longitude.").arg(i + 1));
            return;
        }
    }
    commitFieldsToWorking();
    if (work_.points.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Route Properties"),
            QStringLiteral("A route needs at least two points."));
        return;
    }
    accept();
}
