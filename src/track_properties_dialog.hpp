#pragma once
#include <QDialog>
#include "route_types.hpp"
#include "units.hpp"        // DistanceUnit (by value)

class QLineEdit;

// Properties editor for a single recorded track: name and description, alongside
// read-only statistics (start time, duration, point count, distance run).
//
// Unlike a route, a track's points are a record of where the boat actually went,
// so they are not editable here — reshape a copy instead ("Copy to Route").
class TrackPropertiesDialog : public QDialog {
    Q_OBJECT
public:
    TrackPropertiesDialog(const Track& track, DistanceUnit distanceUnit,
                          QWidget* parent = nullptr);

    // The edited track: the original with name/description replaced.
    Track currentTrack() const;

private:
    Track      work_;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* descEdit_ = nullptr;
};
