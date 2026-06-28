#include "ais_decoder.hpp"
#include <QChar>
#include <vector>

namespace {

// Unpacked AIS payload: a string of bits with typed field readers.
class BitField {
public:
    explicit BitField(const QString& sixbit) {
        bits_.reserve(sixbit.size() * 6);
        for (QChar qc : sixbit) {
            int v = qc.unicode() - 48;
            if (v > 40) v -= 8;
            if (v < 0 || v > 63) v = 0;          // ignore stray characters
            for (int b = 5; b >= 0; --b) bits_.push_back(char((v >> b) & 1));
        }
    }
    int size() const { return int(bits_.size()); }

    quint64 u(int start, int len) const {        // unsigned, big-endian
        quint64 r = 0;
        for (int i = 0; i < len; ++i) {
            r <<= 1;
            const int idx = start + i;
            if (idx >= 0 && idx < int(bits_.size())) r |= quint64(bits_[idx]);
        }
        return r;
    }
    qint64 i(int start, int len) const {         // two's-complement signed
        quint64 v = u(start, len);
        if (len > 0 && len < 64 && (v & (quint64(1) << (len - 1))))
            v |= ~((quint64(1) << len) - 1);
        return qint64(v);
    }
    QString text(int start, int len) const {     // 6-bit ASCII, trimmed
        static const char* kTable =
            "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
        QString s;
        const int n = len / 6;
        for (int k = 0; k < n; ++k)
            s += QLatin1Char(kTable[int(u(start + k * 6, 6)) & 63]);
        while (!s.isEmpty() && (s.back() == QLatin1Char('@') || s.back() == QLatin1Char(' ')))
            s.chop(1);
        return s;
    }
private:
    std::vector<char> bits_;
};

AisDimensions dims(const BitField& b, int bow, int stern, int port, int stbd) {
    AisDimensions d;
    d.toBow       = double(b.u(bow,   9));
    d.toStern     = double(b.u(stern, 9));
    d.toPort      = double(b.u(port,  6));
    d.toStarboard = double(b.u(stbd,  6));
    return d;
}

// AIS longitude/latitude are 1/10000-minute units (1/600000 degree); the
// "not available" sentinels are 181 deg and 91 deg.
std::optional<double> lonOf(qint64 raw) {
    return raw == 108600000 ? std::nullopt : std::optional<double>(raw / 600000.0);
}
std::optional<double> latOf(qint64 raw) {
    return raw == 54600000 ? std::nullopt : std::optional<double>(raw / 600000.0);
}

// ---- per-message decoders (publish into the store) -------------------------

// type 1/2/3: Class A position report (168 bits).
void positionA(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 137) return;
    AisPositionReport r;
    r.mmsi = quint32(b.u(8, 30));
    r.cls = AisClass::A;
    r.navStatus = AisNavStatus(int(b.u(38, 4)));
    const int rot = int(b.i(42, 8));
    if (rot != -128) { const double a = rot / 4.733; r.rotDegPerMin = (rot < 0 ? -1 : 1) * a * a; }
    const int sog = int(b.u(50, 10));
    if (sog != 1023) r.sogKnots = sog / 10.0;
    r.longitudeDeg = lonOf(b.i(61, 28));
    r.latitudeDeg  = latOf(b.i(89, 27));
    const int cog = int(b.u(116, 12));
    if (cog != 3600) r.cogDegTrue = cog / 10.0;
    const int hdg = int(b.u(128, 9));
    if (hdg != 511) r.headingDegTrue = hdg;
    pub->publishAisPosition(r, src);
}

// type 9: Standard SAR aircraft position report (168 bits). Unlike vessel
// reports, SOG is in whole knots (not 1/10) and altitude replaces the slot a
// Class A uses for nav status; there is no true-heading field.
void positionSarAircraft(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 128) return;
    AisPositionReport r;
    r.mmsi = quint32(b.u(8, 30));
    r.cls = AisClass::SarAircraft;
    const int alt = int(b.u(38, 12));
    if (alt != 4095) r.altitudeMeters = double(alt);     // metres; 4095 = n/a
    const int sog = int(b.u(50, 10));
    if (sog != 1023) r.sogKnots = double(sog);           // whole knots
    r.longitudeDeg = lonOf(b.i(61, 28));
    r.latitudeDeg  = latOf(b.i(89, 27));
    const int cog = int(b.u(116, 12));
    if (cog != 3600) r.cogDegTrue = cog / 10.0;
    pub->publishAisPosition(r, src);
}

// type 5: Class A static & voyage (424 bits, usually 2 fragments).
void staticA(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 240) return;
    AisStaticData d;
    d.mmsi = quint32(b.u(8, 30));
    d.cls = AisClass::A;
    if (const quint64 imo = b.u(40, 30)) d.imoNumber = int(imo);
    d.callSign = b.text(70, 42);
    d.name = b.text(112, 120);
    if (const int t = int(b.u(232, 8))) d.shipType = t;
    d.dimensions = dims(b, 240, 249, 258, 264);
    if (const int dr = int(b.u(294, 8))) d.draughtMeters = dr / 10.0;
    d.destination = b.text(302, 120);
    pub->publishAisStatic(d, src);
}

// type 18: Class B position report (168 bits).
void positionB(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 133) return;
    AisPositionReport r;
    r.mmsi = quint32(b.u(8, 30));
    r.cls = AisClass::B;
    const int sog = int(b.u(46, 10));
    if (sog != 1023) r.sogKnots = sog / 10.0;
    r.longitudeDeg = lonOf(b.i(57, 28));
    r.latitudeDeg  = latOf(b.i(85, 27));
    const int cog = int(b.u(112, 12));
    if (cog != 3600) r.cogDegTrue = cog / 10.0;
    const int hdg = int(b.u(124, 9));
    if (hdg != 511) r.headingDegTrue = hdg;
    pub->publishAisPosition(r, src);
}

// type 19: Class B extended position report (position + static, 312 bits).
void positionBExt(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 139) return;
    AisPositionReport r;
    r.mmsi = quint32(b.u(8, 30));
    r.cls = AisClass::B;
    const int sog = int(b.u(46, 10));
    if (sog != 1023) r.sogKnots = sog / 10.0;
    r.longitudeDeg = lonOf(b.i(57, 28));
    r.latitudeDeg  = latOf(b.i(85, 27));
    const int cog = int(b.u(112, 12));
    if (cog != 3600) r.cogDegTrue = cog / 10.0;
    const int hdg = int(b.u(124, 9));
    if (hdg != 511) r.headingDegTrue = hdg;
    pub->publishAisPosition(r, src);

    AisStaticData d;
    d.mmsi = r.mmsi;
    d.cls = AisClass::B;
    d.name = b.text(143, 120);
    if (const int t = int(b.u(263, 8))) d.shipType = t;
    if (b.size() >= 301) d.dimensions = dims(b, 271, 280, 289, 295);
    pub->publishAisStatic(d, src);
}

// type 24: Class B static data report (Part A = name, Part B = type/callsign/dims).
void static24(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 40) return;
    AisStaticData d;
    d.mmsi = quint32(b.u(8, 30));
    d.cls = AisClass::B;
    const int part = int(b.u(38, 2));
    if (part == 0) {
        d.name = b.text(40, 120);
    } else if (part == 1) {
        if (const int t = int(b.u(40, 8))) d.shipType = t;
        d.callSign = b.text(90, 42);
        if (b.size() >= 162) d.dimensions = dims(b, 132, 141, 150, 156);
    }
    pub->publishAisStatic(d, src);
}

// type 21: Aid-to-Navigation report (272..360 bits). Carries both the aid's
// position (dynamic) and its identity (static), so we publish one of each — like
// the type-19 extended Class B report.
void atonReport(const BitField& b, IAisPublisher* pub, const QString& src) {
    if (b.size() < 272) return;
    const quint32 mmsi = quint32(b.u(8, 30));

    AisPositionReport r;
    r.mmsi = mmsi;
    r.cls = AisClass::AtoN;
    r.longitudeDeg = lonOf(b.i(164, 28));
    r.latitudeDeg  = latOf(b.i(192, 27));
    pub->publishAisPosition(r, src);

    AisStaticData d;
    d.mmsi = mmsi;
    d.cls = AisClass::AtoN;
    d.atonType = int(b.u(38, 5));            // 0 is a valid aid type ("default")
    QString name = b.text(43, 120);
    // Optional name extension (bits 272..), up to 88 bits / 14 chars, appended
    // when the 20-char name field overflows.
    const int extChars = (b.size() - 272) / 6;
    if (extChars > 0) name += b.text(272, extChars * 6);
    d.name = name;
    d.dimensions = dims(b, 219, 228, 237, 243);
    d.atonOffPosition = bool(b.u(259, 1));
    d.atonVirtual     = bool(b.u(269, 1));
    pub->publishAisStatic(d, src);
}
} // namespace

AisDecoder::AisDecoder(IAisPublisher* publisher, QString source)
    : publisher_(publisher), source_(std::move(source)) {}

void AisDecoder::handleSentence(const QString& line) {
    // !AIVDM,fragCount,fragIndex,seqId,channel,payload,fillBits*checksum
    QString body = line;
    const int star = body.indexOf('*');
    if (star >= 0) body.truncate(star);
    const QStringList f = body.split(',');
    if (f.size() < 7) return;

    const int count = f[1].toInt();
    const int index = f[2].toInt();
    const QString payload = f[5];
    if (count <= 1) { decodePayload(payload); return; }

    // Multi-fragment: accumulate by sequential id until all parts are present.
    const int seq = f[3].isEmpty() ? 0 : f[3].toInt();
    Fragments& fr = pending_[seq];
    if (fr.count != count) { fr.count = count; fr.parts = QStringList(count); }
    if (index >= 1 && index <= count) fr.parts[index - 1] = payload;

    for (const QString& p : fr.parts) if (p.isEmpty()) return;   // still waiting
    decodePayload(fr.parts.join(QString()));
    pending_.remove(seq);
}

void AisDecoder::decodePayload(const QString& sixbit) {
    if (sixbit.isEmpty()) return;
    const BitField b(sixbit);
    if (b.size() < 38) return;
    switch (int(b.u(0, 6))) {
        case 1: case 2: case 3: positionA(b, publisher_, source_);    break;
        case 5:                 staticA(b, publisher_, source_);      break;
        case 9:                 positionSarAircraft(b, publisher_, source_); break;
        case 18:                positionB(b, publisher_, source_);    break;
        case 19:                positionBExt(b, publisher_, source_); break;
        case 21:                atonReport(b, publisher_, source_);   break;
        case 24:                static24(b, publisher_, source_);     break;
        default: break;
    }
}
