#include <QApplication>
#include <QDir>
#include <QSettings>
#include "chart_loader.hpp"
#include "main_window.hpp"
#include "app_info.hpp"
#include "app_log.hpp"
#include "bundle_paths.hpp"
#include "debug_trace.hpp"

int main(int argc, char** argv) {
    // Share GL resources across all contexts. The GPU chart layer is a
    // QRhiWidget composited into the top-level window's RHI backing store; on the
    // OpenGL path (Linux/Raspberry Pi) that composition is more reliable when
    // contexts share. Must be set before QApplication is constructed.
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // QApplication must come first: we need applicationDirPath() to locate the
    // bundled gdal-data/ folder before initialising GDAL.
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("net.holybee"));
    // The QSettings key stays "HMV Chart" so existing user settings aren't
    // orphaned by the rename; the user-visible name is the display name.
    QApplication::setApplicationName(QStringLiteral("HMV Chart"));
    QApplication::setApplicationDisplayName(appinfo::name());
    QApplication::setApplicationVersion(appinfo::version());

    // Install the file-logging message handler as early as possible so startup
    // messages are captured, then apply the persisted opt-in state. The handler
    // always chains to the default (console) handler; it only writes a file while
    // enabled. The live Settings object drives later on/off changes from the menu.
    // (Key must match settings.cpp's kLogToFile.)
    applog::install();
    applog::setEnabled(QSettings().value(QStringLiteral("system/logToFile"), false).toBool());
    // One self-describing startup line: which build, Qt version, and QPA platform
    // (e.g. "wayland" vs "xcb") — the context needed to read a GPU/RHI report.
    qInfo().noquote() << "Startup:" << appinfo::name() << appinfo::version()
                      << "| Qt" << qVersion()
                      << "| platform" << QGuiApplication::platformName();

    // Resolve the bundled GDAL data directory (contains s57objectclasses.csv
    // etc.). CMake copies this from the GDAL installation next to the executable
    // (Contents/Resources on macOS) at build time, so it travels with the app on
    // any machine regardless of whether GDAL is installed system-wide.
    std::string gdalDataDir;
    {
        const QDir d(bundlepaths::dataDir() + QStringLiteral("/gdal-data"));
        if (d.exists())
            gdalDataDir = d.absolutePath().toStdString();
    }

    // Registers GDAL drivers and S-57 options; sets GDAL_DATA if we found the
    // bundle. Must be called before any worker threads are spawned.
    chart::init(gdalDataDir);

    MainWindow w;
    w.show();
    const int rc = app.exec();
    hmvtrace::mark("app.exec() returned; destroying MainWindow");
    // MainWindow `w` is destroyed as this scope unwinds (after the trace above);
    // a final "clean exit" line is written by its destructor chain. If the trace
    // file ends before that, teardown blocked — the last line names where.
    return rc;
}
