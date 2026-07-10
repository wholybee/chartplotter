#pragma once
// src/app_log.hpp
//
// Application-wide logging to a file, toggled by a user setting.
//
// install() sets a Qt message handler that captures every qDebug/qInfo/qWarning/
// qCritical and all categorized logging (e.g. the GPU lifecycle on the "hmv.gpu"
// category) — the same stream that prints to the console on Linux — and, while
// enabled, mirrors each line to a log file. The previous handler is still
// chained, so console/default output is unchanged whether logging is on or off.
//
// The log lives in a per-machine location: %ProgramData%/<App>/ on Windows, the
// user's app-data directory elsewhere. setEnabled() opens or closes it (rotating
// one backup when it has grown past a size cap) and writes a session marker.
// Thread-safe: qDebug() may be called from any thread, so file access is mutexed.

#include <QString>

namespace applog {

// Install the Qt message handler. Call once, as early as possible in main()
// (after QApplication and the application name exist). Idempotent.
void install();

// Turn file logging on or off. Opens the file (rotating it first if large) or
// closes it, writing a session marker either way. Best-effort: if the file can't
// be opened the handler simply keeps chaining to the default handler.
void setEnabled(bool on);
bool isEnabled();

// Absolute path of the log file, and its directory. Shown to the user (e.g. the
// GPU-fallback note) so the log is easy to find.
QString logFilePath();
QString logDir();

} // namespace applog
