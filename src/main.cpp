#include <QApplication>
#include <QDir>
#include "chart_loader.hpp"
#include "main_window.hpp"
#include "app_info.hpp"
#include "debug_trace.hpp"

int main(int argc, char** argv) {
    // QApplication must come first: we need applicationDirPath() to locate the
    // bundled gdal-data/ folder before initialising GDAL.
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("net.holybee"));
    // The QSettings key stays "HMV Chart" so existing user settings aren't
    // orphaned by the rename; the user-visible name is the display name.
    QApplication::setApplicationName(QStringLiteral("HMV Chart"));
    QApplication::setApplicationDisplayName(appinfo::name());
    QApplication::setApplicationVersion(appinfo::version());

    // Resolve the bundled GDAL data directory (contains s57objectclasses.csv
    // etc.). CMake copies this from the GDAL installation into gdal-data/ next
    // to the executable at build time, so it travels with the app on any machine
    // regardless of whether GDAL is installed system-wide.
    std::string gdalDataDir;
    {
        const QDir d(QApplication::applicationDirPath() + QStringLiteral("/gdal-data"));
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
