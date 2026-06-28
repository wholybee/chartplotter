#pragma once
#include <QString>
#include <QStringList>
#include <QHash>
#include "ais_target_store.hpp"   // IAisPublisher and report structs

// Decodes AIS sentences (!AIVDM / !AIVDO) into the AIS target store.
//
// Owns the AIS concern so the NMEA-0183 sentence layer doesn't have to: it
// handles multi-fragment reassembly, 6-bit ASCII payload unpacking, and the
// common message types (1/2/3 Class A position, 5 Class A static, 9 SAR aircraft
// position, 18/19 Class B position, 21 Aid-to-Navigation, 24 Class B static).
// Distress beacons (AIS-SART/MOB/EPIRB) arrive as type-1 reports and are sorted
// out by MMSI in the store. It is transport-agnostic — fed raw VDM/VDO
// lines by whoever owns the connection (today the NMEA 0183 plugin), publishing
// through IAisPublisher.
class AisDecoder {
public:
    AisDecoder(IAisPublisher* publisher, QString source);

    // Feed one raw NMEA line ("!AIVDM,...*hh"). Reassembles fragments and, when a
    // full message is available, decodes and publishes it.
    void handleSentence(const QString& line);

private:
    void decodePayload(const QString& sixbit);   // dispatch by message type

    IAisPublisher* publisher_ = nullptr;
    QString source_;

    // In-progress multi-fragment messages, keyed by sequential message id.
    struct Fragments { int count = 0; QStringList parts; };
    QHash<int, Fragments> pending_;
};
