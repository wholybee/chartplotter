#pragma once
#include <QColor>
#include <QString>
#include <QVector>

// Route display colours, keyed by the Garmin GPX extension names used in
// <gpxx:DisplayColor>. Keeping the name<->colour table in one header lets the
// route overlay (drawing) and the Route Properties dialog (the swatch picker)
// agree on the same palette, and lets colours round-trip through GPX with
// OpenCPN / Garmin unchanged.
//
// An empty name means "no explicit colour" — the overlay falls back to the
// app's default route colour.
namespace routecolor {

struct Entry {
    const char* name;    // GPX <gpxx:DisplayColor> value
    QColor      color;   // on-screen colour
};

// The 16 standard Garmin DisplayColor names, in a picker-friendly order
// (grays, then saturated hues). Values chosen to read clearly over a chart.
inline const QVector<Entry>& palette() {
    static const QVector<Entry> kPalette = {
        { "Black",       QColor(0x00, 0x00, 0x00) },
        { "DarkGray",    QColor(0x69, 0x69, 0x69) },
        { "LightGray",   QColor(0xD3, 0xD3, 0xD3) },
        { "White",       QColor(0xFF, 0xFF, 0xFF) },
        { "DarkRed",     QColor(0x8B, 0x00, 0x00) },
        { "Red",         QColor(0xE0, 0x20, 0x20) },
        { "DarkGreen",   QColor(0x00, 0x64, 0x00) },
        { "Green",       QColor(0x1E, 0xA0, 0x1E) },
        { "DarkYellow",  QColor(0xB8, 0x86, 0x0B) },
        { "Yellow",      QColor(0xF0, 0xC0, 0x00) },
        { "DarkBlue",    QColor(0x00, 0x00, 0x8B) },
        { "Blue",        QColor(0x20, 0x60, 0xE0) },
        { "DarkMagenta", QColor(0x8B, 0x00, 0x8B) },
        { "Magenta",     QColor(0xD0, 0x30, 0xD0) },
        { "DarkCyan",    QColor(0x00, 0x8B, 0x8B) },
        { "Cyan",        QColor(0x00, 0xB0, 0xC0) },
    };
    return kPalette;
}

// Resolve a GPX colour name to a QColor. Match is case-insensitive so files
// from other writers ("red", "RED") still resolve. Returns `fallback` for an
// empty or unrecognised name.
inline QColor toColor(const QString& name, const QColor& fallback) {
    if (name.isEmpty()) return fallback;
    for (const Entry& e : palette())
        if (name.compare(QLatin1String(e.name), Qt::CaseInsensitive) == 0)
            return e.color;
    return fallback;
}

}  // namespace routecolor
