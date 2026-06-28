#pragma once
#include <QObject>
#include <QString>

class NavDataStore;
class Nmea2000Client;

// Transmits NMEA 2000 navigation PGNs while a route is being navigated. Mirrors
// Nmea0183NavSender: it subscribes to the NavDataStore and, each time the route-
// navigation output updates (NavDataStore::navigationChanged, ~1 Hz) with a route
// active, builds and sends:
//   PGN 129283  Cross Track Error
//   PGN 129284  Navigation Data
//   PGN 129285  Navigation - Route / WP Information
// through the Nmea2000Client (which serializes them and writes to the gateway).
//
// Loop guard: if the navigation solution itself was sourced from this same NMEA
// 2000 link, nothing is transmitted, so received navigation can never be echoed
// straight back out.
class N2kNavSender : public QObject {
    Q_OBJECT
public:
    N2kNavSender(NavDataStore* store, Nmea2000Client* client,
                 QString ownSourceId, QObject* parent = nullptr);

private slots:
    void onNavigationChanged();

private:
    NavDataStore*   store_  = nullptr;
    Nmea2000Client* client_ = nullptr;
    QString         ownSourceId_;
};
