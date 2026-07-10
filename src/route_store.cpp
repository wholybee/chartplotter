#include "route_store.hpp"

#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <atomic>

namespace {
// Unique connection name so this store never collides with the MBTiles readers
// (or a re-open) in QSqlDatabase's global registry.
QString nextConnName() {
    static std::atomic<quint64> n{0};
    return QStringLiteral("routes_%1").arg(n.fetch_add(1));
}

QString isoOrNull(const QDateTime& t) {
    return t.isValid() ? t.toUTC().toString(Qt::ISODate) : QString();
}
}  // namespace

RouteStore::RouteStore(QObject* parent) : QObject(parent) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/routes.db");

    conn_ = nextConnName();
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
    db_.setDatabaseName(path);
    if (!db_.open()) {
        qWarning() << "RouteStore: cannot open" << path << db_.lastError().text();
        return;
    }
    // Enforce ON DELETE CASCADE for route_points.
    QSqlQuery(db_).exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    QString err;
    if (!createSchema(err)) {
        qWarning() << "RouteStore: schema error:" << err;
        return;
    }
    ok_ = true;
    loadAll();
}

RouteStore::~RouteStore() {
    if (db_.isOpen()) db_.close();
    db_ = QSqlDatabase();
    if (!conn_.isEmpty()) QSqlDatabase::removeDatabase(conn_);
}

bool RouteStore::createSchema(QString& err) {
    static const char* kDdl[] = {
        "CREATE TABLE IF NOT EXISTS waypoints("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT,"
        " lat REAL NOT NULL, lon REAL NOT NULL, symbol TEXT, description TEXT,"
        " created TEXT, visible INTEGER NOT NULL DEFAULT 1)",
        "CREATE TABLE IF NOT EXISTS routes("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, description TEXT,"
        " created TEXT, visible INTEGER NOT NULL DEFAULT 1)",
        "CREATE TABLE IF NOT EXISTS route_points("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " route_id INTEGER NOT NULL REFERENCES routes(id) ON DELETE CASCADE,"
        " seq INTEGER NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL, name TEXT)",
        "CREATE INDEX IF NOT EXISTS idx_route_points_route"
        " ON route_points(route_id, seq)",
        "CREATE TABLE IF NOT EXISTS tracks("
        " id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, description TEXT,"
        " created TEXT, visible INTEGER NOT NULL DEFAULT 1)",
        "CREATE TABLE IF NOT EXISTS track_points("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
        " seq INTEGER NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL, time TEXT)",
        "CREATE INDEX IF NOT EXISTS idx_track_points_track"
        " ON track_points(track_id, seq)",
    };
    for (const char* ddl : kDdl) {
        QSqlQuery q(db_);
        if (!q.exec(QLatin1String(ddl))) { err = q.lastError().text(); return false; }
    }
    return true;
}

void RouteStore::loadAll() {
    waypoints_.clear();
    routes_.clear();
    tracks_.clear();

    QSqlQuery wq(db_);
    if (wq.exec(QStringLiteral("SELECT id,name,lat,lon,symbol,description,created,visible"
                               " FROM waypoints ORDER BY id"))) {
        while (wq.next()) {
            Waypoint w;
            w.id          = wq.value(0).toLongLong();
            w.name        = wq.value(1).toString();
            w.lat         = wq.value(2).toDouble();
            w.lon         = wq.value(3).toDouble();
            w.symbol      = wq.value(4).toString();
            w.description = wq.value(5).toString();
            w.createdUtc  = QDateTime::fromString(wq.value(6).toString(), Qt::ISODate);
            w.visible     = wq.value(7).toInt() != 0;
            waypoints_.push_back(std::move(w));
        }
    }

    QSqlQuery rq(db_);
    if (rq.exec(QStringLiteral("SELECT id,name,description,created,visible"
                               " FROM routes ORDER BY id"))) {
        while (rq.next()) {
            Route r;
            r.id          = rq.value(0).toLongLong();
            r.name        = rq.value(1).toString();
            r.description = rq.value(2).toString();
            r.createdUtc  = QDateTime::fromString(rq.value(3).toString(), Qt::ISODate);
            r.visible     = rq.value(4).toInt() != 0;
            routes_.push_back(std::move(r));
        }
    }
    // Points for each route, in sequence order.
    for (Route& r : routes_) {
        QSqlQuery pq(db_);
        pq.prepare(QStringLiteral("SELECT lat,lon,name FROM route_points"
                                  " WHERE route_id=? ORDER BY seq"));
        pq.addBindValue(r.id);
        if (pq.exec()) {
            while (pq.next()) {
                RoutePoint p;
                p.lat  = pq.value(0).toDouble();
                p.lon  = pq.value(1).toDouble();
                p.name = pq.value(2).toString();
                r.points.push_back(std::move(p));
            }
        }
    }

    QSqlQuery tq(db_);
    if (tq.exec(QStringLiteral("SELECT id,name,description,created,visible"
                               " FROM tracks ORDER BY id"))) {
        while (tq.next()) {
            Track t;
            t.id          = tq.value(0).toLongLong();
            t.name        = tq.value(1).toString();
            t.description = tq.value(2).toString();
            t.createdUtc  = QDateTime::fromString(tq.value(3).toString(), Qt::ISODate);
            t.visible     = tq.value(4).toInt() != 0;
            tracks_.push_back(std::move(t));
        }
    }
    for (Track& t : tracks_) {
        QSqlQuery pq(db_);
        pq.prepare(QStringLiteral("SELECT lat,lon,time FROM track_points"
                                  " WHERE track_id=? ORDER BY seq"));
        pq.addBindValue(t.id);
        if (pq.exec()) {
            while (pq.next()) {
                TrackPoint p;
                p.lat     = pq.value(0).toDouble();
                p.lon     = pq.value(1).toDouble();
                p.timeUtc = QDateTime::fromString(pq.value(2).toString(), Qt::ISODate);
                t.points.push_back(std::move(p));
            }
        }
    }
}

const Route* RouteStore::route(qint64 id) const {
    for (const Route& r : routes_)
        if (r.id == id) return &r;
    return nullptr;
}

const Track* RouteStore::track(qint64 id) const {
    for (const Track& t : tracks_)
        if (t.id == id) return &t;
    return nullptr;
}

namespace {
// Find the highest N from names matching exactly "{prefix} N" (case-insensitive,
// optional surrounding whitespace). Returns 0 if none match.
int highestNumberedSuffix(const QStringList& names, const QString& prefix) {
    int best = 0;
    for (const QString& n : names) {
        const QString s = n.trimmed();
        if (s.size() <= prefix.size() || !s.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        bool ok = false;
        const int v = s.mid(prefix.size()).trimmed().toInt(&ok);
        if (ok && v > best) best = v;
    }
    return best;
}
}  // namespace

QString RouteStore::nextRouteName() const {
    QStringList names;
    names.reserve(routes_.size());
    for (const Route& r : routes_) names.push_back(r.name);
    return QStringLiteral("Route %1").arg(highestNumberedSuffix(names, QStringLiteral("Route ")) + 1);
}

QString RouteStore::nextWaypointName() const {
    QStringList names;
    names.reserve(waypoints_.size());
    for (const Waypoint& w : waypoints_) names.push_back(w.name);
    return QStringLiteral("Waypoint %1").arg(highestNumberedSuffix(names, QStringLiteral("Waypoint ")) + 1);
}

QString RouteStore::trackNameFor(const QDateTime& createdUtc) {
    // Local time: the skipper reads this against the clock in the cockpit, not
    // against UTC. Year-first so the list sorts chronologically by name too.
    const QDateTime local = createdUtc.isValid() ? createdUtc.toLocalTime()
                                                 : QDateTime::currentDateTime();
    return local.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
}

// ---- waypoints -------------------------------------------------------------

qint64 RouteStore::addWaypoint(Waypoint w) {
    if (!ok_) return -1;
    if (!w.createdUtc.isValid()) w.createdUtc = QDateTime::currentDateTimeUtc();
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO waypoints(name,lat,lon,symbol,description,created,visible)"
                             " VALUES(?,?,?,?,?,?,?)"));
    q.addBindValue(w.name);
    q.addBindValue(w.lat);
    q.addBindValue(w.lon);
    q.addBindValue(w.symbol);
    q.addBindValue(w.description);
    q.addBindValue(isoOrNull(w.createdUtc));
    q.addBindValue(w.visible ? 1 : 0);
    if (!q.exec()) { qWarning() << "addWaypoint:" << q.lastError().text(); return -1; }
    w.id = q.lastInsertId().toLongLong();
    waypoints_.push_back(w);
    emit waypointsChanged();
    return w.id;
}

void RouteStore::updateWaypoint(const Waypoint& w) {
    if (!ok_ || w.id < 0) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE waypoints SET name=?,lat=?,lon=?,symbol=?,description=?,visible=?"
                             " WHERE id=?"));
    q.addBindValue(w.name);
    q.addBindValue(w.lat);
    q.addBindValue(w.lon);
    q.addBindValue(w.symbol);
    q.addBindValue(w.description);
    q.addBindValue(w.visible ? 1 : 0);
    q.addBindValue(w.id);
    if (!q.exec()) { qWarning() << "updateWaypoint:" << q.lastError().text(); return; }
    for (Waypoint& cur : waypoints_)
        if (cur.id == w.id) { const QDateTime keep = cur.createdUtc; cur = w; cur.createdUtc = keep; break; }
    emit waypointsChanged();
}

void RouteStore::setWaypointVisible(qint64 id, bool on) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE waypoints SET visible=? WHERE id=?"));
    q.addBindValue(on ? 1 : 0);
    q.addBindValue(id);
    if (!q.exec()) return;
    for (Waypoint& w : waypoints_)
        if (w.id == id) { w.visible = on; break; }
    emit waypointsChanged();
}

void RouteStore::removeWaypoint(qint64 id) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM waypoints WHERE id=?"));
    q.addBindValue(id);
    if (!q.exec()) return;
    for (int i = 0; i < waypoints_.size(); ++i)
        if (waypoints_[i].id == id) { waypoints_.remove(i); break; }
    emit waypointsChanged();
}

// ---- routes ----------------------------------------------------------------

qint64 RouteStore::addRoute(Route r) {
    if (!ok_) return -1;
    if (!r.createdUtc.isValid()) r.createdUtc = QDateTime::currentDateTimeUtc();
    db_.transaction();
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO routes(name,description,created,visible)"
                             " VALUES(?,?,?,?)"));
    q.addBindValue(r.name);
    q.addBindValue(r.description);
    q.addBindValue(isoOrNull(r.createdUtc));
    q.addBindValue(r.visible ? 1 : 0);
    if (!q.exec()) { qWarning() << "addRoute:" << q.lastError().text(); db_.rollback(); return -1; }
    r.id = q.lastInsertId().toLongLong();

    for (int i = 0; i < r.points.size(); ++i) {
        QSqlQuery pq(db_);
        pq.prepare(QStringLiteral("INSERT INTO route_points(route_id,seq,lat,lon,name)"
                                  " VALUES(?,?,?,?,?)"));
        pq.addBindValue(r.id);
        pq.addBindValue(i);
        pq.addBindValue(r.points[i].lat);
        pq.addBindValue(r.points[i].lon);
        pq.addBindValue(r.points[i].name);
        if (!pq.exec()) { qWarning() << "addRoute pt:" << pq.lastError().text(); db_.rollback(); return -1; }
    }
    db_.commit();
    routes_.push_back(r);
    emit routesChanged();
    return r.id;
}

void RouteStore::updateRoute(const Route& r) {
    if (!ok_ || r.id < 0) return;
    db_.transaction();
    {
        QSqlQuery q(db_);
        q.prepare(QStringLiteral("UPDATE routes SET name=?,description=?,visible=? WHERE id=?"));
        q.addBindValue(r.name);
        q.addBindValue(r.description);
        q.addBindValue(r.visible ? 1 : 0);
        q.addBindValue(r.id);
        if (!q.exec()) { qWarning() << "updateRoute:" << q.lastError().text(); db_.rollback(); return; }
    }
    {
        QSqlQuery q(db_);   // replace the whole point list
        q.prepare(QStringLiteral("DELETE FROM route_points WHERE route_id=?"));
        q.addBindValue(r.id);
        if (!q.exec()) { db_.rollback(); return; }
    }
    for (int i = 0; i < r.points.size(); ++i) {
        QSqlQuery pq(db_);
        pq.prepare(QStringLiteral("INSERT INTO route_points(route_id,seq,lat,lon,name)"
                                  " VALUES(?,?,?,?,?)"));
        pq.addBindValue(r.id);
        pq.addBindValue(i);
        pq.addBindValue(r.points[i].lat);
        pq.addBindValue(r.points[i].lon);
        pq.addBindValue(r.points[i].name);
        if (!pq.exec()) { db_.rollback(); return; }
    }
    db_.commit();
    for (Route& cur : routes_)
        if (cur.id == r.id) { const QDateTime keep = cur.createdUtc; cur = r; cur.createdUtc = keep; break; }
    emit routesChanged();
}

void RouteStore::setRouteVisible(qint64 id, bool on) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE routes SET visible=? WHERE id=?"));
    q.addBindValue(on ? 1 : 0);
    q.addBindValue(id);
    if (!q.exec()) return;
    for (Route& r : routes_)
        if (r.id == id) { r.visible = on; break; }
    emit routesChanged();
}

void RouteStore::removeRoute(qint64 id) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM routes WHERE id=?"));   // cascade drops points
    q.addBindValue(id);
    if (!q.exec()) return;
    for (int i = 0; i < routes_.size(); ++i)
        if (routes_[i].id == id) { routes_.remove(i); break; }
    emit routesChanged();
}

// ---- tracks ----------------------------------------------------------------

qint64 RouteStore::addTrack(Track t) {
    if (!ok_) return -1;
    if (!t.createdUtc.isValid()) t.createdUtc = QDateTime::currentDateTimeUtc();
    db_.transaction();
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO tracks(name,description,created,visible)"
                             " VALUES(?,?,?,?)"));
    q.addBindValue(t.name);
    q.addBindValue(t.description);
    q.addBindValue(isoOrNull(t.createdUtc));
    q.addBindValue(t.visible ? 1 : 0);
    if (!q.exec()) { qWarning() << "addTrack:" << q.lastError().text(); db_.rollback(); return -1; }
    t.id = q.lastInsertId().toLongLong();

    for (int i = 0; i < t.points.size(); ++i) {
        QSqlQuery pq(db_);
        pq.prepare(QStringLiteral("INSERT INTO track_points(track_id,seq,lat,lon,time)"
                                  " VALUES(?,?,?,?,?)"));
        pq.addBindValue(t.id);
        pq.addBindValue(i);
        pq.addBindValue(t.points[i].lat);
        pq.addBindValue(t.points[i].lon);
        pq.addBindValue(isoOrNull(t.points[i].timeUtc));
        if (!pq.exec()) { qWarning() << "addTrack pt:" << pq.lastError().text(); db_.rollback(); return -1; }
    }
    db_.commit();
    tracks_.push_back(t);
    emit tracksChanged();
    return t.id;
}

bool RouteStore::appendTrackPoint(qint64 trackId, const TrackPoint& p) {
    if (!ok_) return false;
    Track* t = nullptr;
    for (Track& cur : tracks_)
        if (cur.id == trackId) { t = &cur; break; }
    if (!t) return false;   // deleted underneath us (e.g. from the Tracks tab)

    QSqlQuery q(db_);
    q.prepare(QStringLiteral("INSERT INTO track_points(track_id,seq,lat,lon,time)"
                             " VALUES(?,?,?,?,?)"));
    q.addBindValue(trackId);
    q.addBindValue(t->points.size());   // points are append-only, so size == next seq
    q.addBindValue(p.lat);
    q.addBindValue(p.lon);
    q.addBindValue(isoOrNull(p.timeUtc));
    if (!q.exec()) { qWarning() << "appendTrackPoint:" << q.lastError().text(); return false; }
    t->points.push_back(p);
    emit tracksChanged();
    return true;
}

// Points are owned by the recorder, so this writes only the editable metadata.
void RouteStore::updateTrack(const Track& t) {
    if (!ok_ || t.id < 0) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE tracks SET name=?,description=?,visible=? WHERE id=?"));
    q.addBindValue(t.name);
    q.addBindValue(t.description);
    q.addBindValue(t.visible ? 1 : 0);
    q.addBindValue(t.id);
    if (!q.exec()) { qWarning() << "updateTrack:" << q.lastError().text(); return; }
    for (Track& cur : tracks_)
        if (cur.id == t.id) {
            cur.name = t.name;
            cur.description = t.description;
            cur.visible = t.visible;
            break;
        }
    emit tracksChanged();
}

void RouteStore::setTrackVisible(qint64 id, bool on) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("UPDATE tracks SET visible=? WHERE id=?"));
    q.addBindValue(on ? 1 : 0);
    q.addBindValue(id);
    if (!q.exec()) return;
    for (Track& t : tracks_)
        if (t.id == id) { t.visible = on; break; }
    emit tracksChanged();
}

void RouteStore::removeTrack(qint64 id) {
    if (!ok_) return;
    QSqlQuery q(db_);
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE id=?"));   // cascade drops points
    q.addBindValue(id);
    if (!q.exec()) return;
    for (int i = 0; i < tracks_.size(); ++i)
        if (tracks_[i].id == id) { tracks_.remove(i); break; }
    emit tracksChanged();
}
