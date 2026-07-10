#pragma once
#include <QDialog>
#include "units.hpp"        // DepthUnit (by value)

class TouchSpinBox;

// Settings editor for how densely a track is recorded. Both gates must be met
// before a point is laid: the time must have elapsed AND the boat must have moved
// at least the minimum distance.
//
// The distance is stored in nautical miles but shown in feet or metres — a
// 0.025 nm threshold is unreadable at this scale — following the user's depth
// unit, which is the app's existing short-distance preference.
class TrackingDialog : public QDialog {
    Q_OBJECT
public:
    TrackingDialog(double intervalSeconds, double minDistanceNm, DepthUnit shortUnit,
                   QWidget* parent = nullptr);

    double intervalSeconds() const;
    double minDistanceNm() const;

private:
    DepthUnit     shortUnit_;
    TouchSpinBox* secondsBox_ = nullptr;
    TouchSpinBox* distanceBox_ = nullptr;
};
