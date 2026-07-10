// src/app_log.cpp
#include "app_log.hpp"
#include "app_info.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QSysInfo>
#include <atomic>

namespace {

QtMessageHandler g_prev = nullptr;       // default handler, chained (console/IDE)
std::atomic<bool> g_enabled{false};      // read lock-free by the handler hot path
std::atomic<bool> g_installed{false};
QMutex g_mutex;                          // guards g_file open/close/write
QFile* g_file = nullptr;                 // owned; open only while enabled

// Rotate to a single ".1" backup once the live log passes this size, so the log
// can never grow without bound (worst case ~2x this on disk).
constexpr qint64 kRotateBytes = 5 * 1024 * 1024;

QString levelTag(QtMsgType t) {
    switch (t) {
        case QtDebugMsg:    return QStringLiteral("D");
        case QtInfoMsg:     return QStringLiteral("I");
        case QtWarningMsg:  return QStringLiteral("W");
        case QtCriticalMsg: return QStringLiteral("C");
        case QtFatalMsg:    return QStringLiteral("F");
    }
    return QStringLiteral("?");
}

QString formatLine(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                 + QLatin1String("  ") + levelTag(type) + QLatin1Char(' ');
    const char* cat = ctx.category;
    if (cat && qstrcmp(cat, "default") != 0)
        line += QLatin1Char('[') + QString::fromUtf8(cat) + QLatin1String("] ");
    line += msg;
    return line;
}

// Both assume g_mutex is held.
void writeRawLocked(const QString& line) {
    if (g_file && g_file->isOpen()) {
        g_file->write(line.toUtf8());
        g_file->write("\n", 1);
        g_file->flush();   // survive a hard crash right after the line that named it
    }
}
void writeMarkerLocked(const QString& what) {
    // Plain ASCII so the marker reads correctly in any log viewer/encoding.
    writeRawLocked(QStringLiteral("=== %1 %2 %3 (%4, %5) - %6 ===")
                       .arg(appinfo::name(), appinfo::version(), what,
                            QSysInfo::prettyProductName(),
                            QSysInfo::currentCpuArchitecture(),
                            QDateTime::currentDateTime().toString(Qt::ISODateWithMs)));
}

void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    if (g_enabled.load(std::memory_order_relaxed)) {
        QMutexLocker lock(&g_mutex);
        writeRawLocked(formatLine(type, ctx, msg));
    }
    if (g_prev) g_prev(type, ctx, msg);   // keep default/console behaviour intact
}

} // namespace

namespace applog {

QString logDir() {
#if defined(Q_OS_WIN)
    // Per-machine, all-users location: %ProgramData%\<App>. A normal user process
    // may create and write within its own subfolder of ProgramData.
    QString base = qEnvironmentVariable("ProgramData");
    if (base.isEmpty()) base = QStringLiteral("C:/ProgramData");
    return QDir::fromNativeSeparators(base) + QLatin1Char('/')
           + QCoreApplication::applicationName();
#else
    // The writable equivalent elsewhere: the app-data dir (a system-wide /var
    // location would need root). ~/.local/share/<App> on Linux, Application
    // Support on macOS.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
#endif
}

QString logFilePath() {
    return logDir() + QLatin1String("/hmv.log");
}

void setEnabled(bool on) {
    // Before install() the handler isn't live; just record the desired state and
    // let install() apply it once the handler is in place.
    if (!g_installed.load(std::memory_order_relaxed)) {
        g_enabled.store(on, std::memory_order_relaxed);
        return;
    }

    QMutexLocker lock(&g_mutex);
    if (on) {
        if (g_file && g_file->isOpen()) return;   // already logging
        QDir().mkpath(logDir());
        const QString path = logFilePath();
        // Rotate a large log to one backup before appending.
        if (QFileInfo(path).size() > kRotateBytes) {
            const QString bak = path + QStringLiteral(".1");
            QFile::remove(bak);
            QFile::rename(path, bak);
        }
        if (!g_file) g_file = new QFile();
        g_file->setFileName(path);
        if (!g_file->open(QIODevice::Append | QIODevice::Text))
            return;   // best-effort: leave disabled, keep chaining to the console
        g_enabled.store(true, std::memory_order_relaxed);
        writeMarkerLocked(QStringLiteral("log opened"));
    } else {
        if (g_file && g_file->isOpen()) {
            writeMarkerLocked(QStringLiteral("log closed"));
            g_file->close();
        }
        g_enabled.store(false, std::memory_order_relaxed);
    }
}

bool isEnabled() { return g_enabled.load(std::memory_order_relaxed); }

void install() {
    if (g_installed.exchange(true)) return;
    g_prev = qInstallMessageHandler(handler);
    // Apply a state requested before install() (setEnabled stored it and returned).
    if (g_enabled.load(std::memory_order_relaxed)) {
        g_enabled.store(false, std::memory_order_relaxed);   // force the open path
        setEnabled(true);
    }
}

} // namespace applog
