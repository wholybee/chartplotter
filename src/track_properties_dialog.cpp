#include "track_properties_dialog.hpp"
#include "geo_nav.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

namespace {
// Total distance run along the track, following the recorded points.
double trackDistanceNm(const Track& t) {
    double sum = 0.0;
    for (int i = 1; i < t.points.size(); ++i)
        sum += geonav::distanceNm(t.points[i - 1].lat, t.points[i - 1].lon,
                                  t.points[i].lat,     t.points[i].lon);
    return sum;
}

// Elapsed time between the first and last recorded point, as "2 h 14 m".
QString trackDuration(const Track& t) {
    if (t.points.size() < 2) return QStringLiteral("—");
    const QDateTime& a = t.points.front().timeUtc;
    const QDateTime& b = t.points.back().timeUtc;
    if (!a.isValid() || !b.isValid()) return QStringLiteral("—");
    const qint64 secs = a.secsTo(b);
    if (secs < 0) return QStringLiteral("—");
    const qint64 h = secs / 3600, m = (secs % 3600) / 60;
    if (h > 0) return QStringLiteral("%1 h %2 m").arg(h).arg(m);
    return QStringLiteral("%1 m").arg(m);
}

QString startedAt(const Track& t) {
    if (!t.createdUtc.isValid()) return QStringLiteral("—");
    return t.createdUtc.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss"));
}
}  // namespace

TrackPropertiesDialog::TrackPropertiesDialog(const Track& track, DistanceUnit distanceUnit,
                                             QWidget* parent)
    : QDialog(parent), work_(track) {
    setWindowTitle(QStringLiteral("Track Properties"));
    resize(440, 400);

    const theme::MenuPalette& t = theme::menu();
    auto* panelCol = dialogchrome::setup(this, QStringLiteral("Track Properties"));
    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Details")));

    auto* body = new QWidget;
    body->setStyleSheet(QStringLiteral("QLabel{ color:%1; font-size:14px; }").arg(t.actionFg));
    auto* bcol = new QVBoxLayout(body);
    bcol->setContentsMargins(16, 4, 16, 12);
    bcol->setSpacing(8);

    auto* form = new QFormLayout;
    form->setSpacing(8);
    nameEdit_ = new QLineEdit(work_.name);
    dialogchrome::styleLineEdit(nameEdit_);
    descEdit_ = new QLineEdit(work_.description);
    dialogchrome::styleLineEdit(descEdit_);
    form->addRow(QStringLiteral("Name"), nameEdit_);
    form->addRow(QStringLiteral("Description"), descEdit_);
    bcol->addLayout(form);
    panelCol->addWidget(body);

    panelCol->addWidget(dialogchrome::sectionHeader(QStringLiteral("Recording")));
    auto* stats = new QWidget;
    stats->setStyleSheet(QStringLiteral("QLabel{ color:%1; font-size:14px; }").arg(t.actionFg));
    auto* scol = new QVBoxLayout(stats);
    scol->setContentsMargins(16, 4, 16, 12);
    scol->setSpacing(8);
    auto* sform = new QFormLayout;
    sform->setSpacing(8);
    sform->addRow(QStringLiteral("Started"),  new QLabel(startedAt(work_)));
    sform->addRow(QStringLiteral("Duration"), new QLabel(trackDuration(work_)));
    sform->addRow(QStringLiteral("Points"),   new QLabel(QString::number(work_.points.size())));
    sform->addRow(QStringLiteral("Distance"),
                  new QLabel(units::formatDistance(trackDistanceNm(work_), distanceUnit)));
    scol->addLayout(sform);
    panelCol->addWidget(stats);

    panelCol->addStretch(1);
    panelCol->addWidget(dialogchrome::okCancelRow(this));

    nameEdit_->setFocus();
    nameEdit_->selectAll();
}

Track TrackPropertiesDialog::currentTrack() const {
    Track t = work_;                 // keeps id / created / visible / points
    t.name = nameEdit_->text().trimmed();
    t.description = descEdit_->text().trimmed();
    return t;
}
