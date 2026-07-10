#pragma once
#include <QDialog>

class QCheckBox;
class TouchSpinBox;

// Configures the rules that decide whether an AIS target counts as "dangerous".
// The dialog only collects and returns the settings; what they do (flagging
// targets) is wired up separately. Each rule pairs an enable checkbox with a
// threshold; the threshold stepper is disabled while its checkbox is unchecked.
class DangerousShipsDialog : public QDialog {
    Q_OBJECT
public:
    DangerousShipsDialog(bool ignoreFarEnabled, double ignoreFarNm,
                         bool cpaEnabled, double cpaNm,
                         bool tcpaEnabled, double tcpaMin,
                         bool anchoredSafeEnabled, double anchoredSogKn,
                         bool alarmSoundEnabled,
                         QWidget* parent = nullptr);

    bool   ignoreFarEnabled() const;
    double ignoreFarNm()      const;
    bool   cpaEnabled()  const;
    double cpaNm()       const;
    bool   tcpaEnabled() const;
    double tcpaMin()     const;
    bool   anchoredSafeEnabled() const;
    double anchoredSogKn()       const;
    bool   alarmSoundEnabled()   const;

private:
    QCheckBox*    ignoreFarCheck_ = nullptr;
    TouchSpinBox* ignoreFarBox_ = nullptr;
    QCheckBox*    cpaCheck_ = nullptr;
    TouchSpinBox* cpaBox_ = nullptr;
    QCheckBox*    tcpaCheck_ = nullptr;
    TouchSpinBox* tcpaBox_ = nullptr;
    QCheckBox*    anchoredCheck_ = nullptr;
    TouchSpinBox* anchoredSogBox_ = nullptr;
    QCheckBox*    alarmCheck_ = nullptr;
};
