#pragma once
#include <QDialog>

class QSlider;
class QLabel;
class QCheckBox;

// Touch-friendly editor for the chart symbol scale plus the text-label and
// depth-sounding sizes, and the text de-clutter (nudge) options. Each size is
// an 11-stop slider (50 % .. 300 % in 25 % steps); 100 % (scale 1.0) is the
// default. Symbol, text, and sounding sizes are independent so the three
// families of markings can be tuned apart. The de-clutter section turns label
// nudging on/off and sets its maximum distance.
//
// Edits a working copy; the caller reads the getters after acceptance and
// persists through Settings.
class ChartSymbolSizeDialog : public QDialog {
    Q_OBJECT
public:
    ChartSymbolSizeDialog(double symbolScale, double textScale,
                          double soundingScale, bool nudgeEnabled,
                          double nudgeMaxPx, QWidget* parent = nullptr);

    // Returns the selected scale factors (0.5 .. 3.0).
    double symbolScale() const;
    double textScale() const;
    double soundingScale() const;

    // Text de-clutter options.
    bool   nudgeEnabled() const;
    double nudgeMaxPx() const;

private:
    QSlider*   symbolSlider_   = nullptr;
    QSlider*   textSlider_     = nullptr;
    QSlider*   soundingSlider_ = nullptr;
    QCheckBox* nudgeCheck_     = nullptr;
    QSlider*   nudgeSlider_    = nullptr;
};
