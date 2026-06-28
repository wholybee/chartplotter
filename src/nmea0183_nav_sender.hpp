#pragma once
#include <QObject>
#include <QString>

class NavDataStore;
class Nmea0183Client;

// Generates and transmits NMEA 0183 navigation sentences while a route is being
// navigated. Subscribes to the NavDataStore: each time the route-navigation
// output updates (NavDataStore::navigationChanged, ~1 Hz) and a route is active,
// it builds APB, RMB, and RMC from the store's NavigationData + ownship state and
// sends them out the NMEA 0183 connection via the client.
//
// Loop guard: if the navigation solution itself was sourced from this same NMEA
// 0183 link (i.e. APB/RMB were received over the link and parsed into the store),
// nothing is transmitted — so received navigation sentences can never be echoed
// straight back out. (That should never happen with the current internal
// navigator, but it makes a loop impossible if it ever did.)
class Nmea0183NavSender : public QObject {
    Q_OBJECT
public:
    // `store` is read for navigation + ownship state and written with this link's
    // transmit status (NavDataStore::setNavTransmitting); `client` transmits the
    // sentences; `ownSourceId` is this link's source id (e.g. "nmea0183"), used
    // by the loop guard and as the transmit-status key.
    Nmea0183NavSender(NavDataStore* store, Nmea0183Client* client,
                      QString ownSourceId, QObject* parent = nullptr);

private slots:
    void onNavigationChanged();

private:
    NavDataStore*   store_  = nullptr;
    Nmea0183Client* client_ = nullptr;
    QString         ownSourceId_;
};
