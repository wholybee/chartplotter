#pragma once
#include <QDialog>
#include <QString>
#include <vector>
#include "route_types.hpp"
#include "units.hpp"

class QLineEdit;
class QScrollArea;
class QWidget;
class QVBoxLayout;
class QLabel;
class QPushButton;
class QComboBox;
class QDateTimeEdit;
class QCheckBox;

// Properties editor for a single route. Operates on a working copy: name,
// description, and the ordered point list are edited here and only committed by
// the caller when the dialog is accepted (OK). Each point row exposes editable
// latitude/longitude fields, a Delete button, and an Edit button that asks the
// host to let the user drag the point on the chart (editPointRequested).
//
// The host (MainWindow) drives the drag round-trip: on editPointRequested it
// reads currentRoute(), hides this dialog, runs the chart drag, then calls
// setRoute() with the updated points and re-shows the dialog.
class RoutePropertiesDialog : public QDialog {
    Q_OBJECT
public:
    // `distUnit` selects the speed unit shown for planned speed (nm->kn,
    // mi->mph, km->km/h) and the units of the derived distance/arrival readout.
    RoutePropertiesDialog(const Route& route,
                          DistanceUnit distUnit = DistanceUnit::NauticalMiles,
                          QWidget* parent = nullptr);

    // Current edited state (name/description from the fields, points from the
    // row lat/lon editors). Keeps the original id / created / visible.
    Route currentRoute() const;
    // Replace the working state and rebuild the rows (used after a chart drag).
    void setRoute(const Route& route);

signals:
    void editPointRequested(int index);   // user tapped a row's Edit (drag) button

private:
    struct Row {
        QWidget*   widget = nullptr;
        QLineEdit* lat = nullptr;
        QLineEdit* lon = nullptr;
    };
    void rebuildRows();
    void commitFieldsToWorking();   // pull name/desc/coords/plan from widgets into work_
    void onDeletePoint(int index);
    void onOk();
    QWidget* buildColorRow();       // display-colour dropdown
    void selectColor(const QString& gpxName);   // pick the matching combo item
    void updateArrival();           // recompute the derived distance/arrival readout
    double totalDistanceNm() const; // sum of the current legs (from work_/rows)
    QDateTime departureUtc() const; // the chosen departure as UTC, or invalid if off

    Route work_;                    // working copy (keeps id/created/visible)
    DistanceUnit distUnit_ = DistanceUnit::NauticalMiles;
    QLineEdit*   nameEdit_ = nullptr;
    QLineEdit*   descEdit_ = nullptr;
    QLineEdit*   speedEdit_ = nullptr;      // planned speed, in distUnit_'s speed unit
    QCheckBox*     departEnable_ = nullptr; // whether a departure is planned at all
    QDateTimeEdit* departEdit_ = nullptr;   // planned departure (local time, calendar popup)
    QLabel*      arrivalLabel_ = nullptr;   // derived ETA / distance / duration
    QComboBox*   colorCombo_ = nullptr;     // display-colour dropdown
    QString      selectedColor_;            // current GPX DisplayColor name ("" = default)
    QScrollArea* scrollArea_ = nullptr;
    QWidget*     rowContainer_ = nullptr;
    QVBoxLayout* rowLayout_ = nullptr;
    QLabel*      countLabel_ = nullptr;
    std::vector<Row> rows_;
};
