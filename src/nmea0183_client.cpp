#include "nmea0183_client.hpp"
#include "nav_data_store.hpp"

#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QDateTime>
#include <QStringList>
#include <cmath>
#include <optional>
#include <utility>

namespace {
constexpr int kStaleMs     = 5000;   // no valid sentence for this long -> not decoding
constexpr int kReconnectMs = 3000;   // retry a failed/dropped TCP connection

// Verify the "*HH" XOR checksum if present. NMEA xors every byte between '$'/'!'
// and '*'. Sentences without a checksum are accepted (some gateways omit it).
bool checksumOk(const QByteArray& s) {
    const int star = s.lastIndexOf('*');
    if (star < 0) return true;                       // no checksum supplied
    if (star + 3 > s.size()) return false;           // "*HH" doesn't fit
    unsigned char x = 0;
    for (int i = 1; i < star; ++i) x ^= static_cast<unsigned char>(s[i]);
    bool ok = false;
    const int given = s.mid(star + 1, 2).toInt(&ok, 16);
    return ok && given == x;
}

// "ddmm.mmmm" + hemisphere -> signed decimal degrees. Works for both latitude
// (dd) and longitude (ddd) because the minutes are always two integer digits:
// degrees = floor(value / 100), minutes = value - degrees*100.
std::optional<double> parseCoord(const QString& field, const QString& hemi) {
    if (field.isEmpty()) return std::nullopt;
    bool ok = false;
    const double raw = field.toDouble(&ok);
    if (!ok) return std::nullopt;
    const double deg = std::floor(raw / 100.0);
    double v = deg + (raw - deg * 100.0) / 60.0;
    const QString h = hemi.toUpper();
    if (h == QLatin1String("S") || h == QLatin1String("W")) v = -v;
    return v;
}

std::optional<double> parseNum(const QString& field) {
    if (field.isEmpty()) return std::nullopt;
    bool ok = false;
    const double v = field.toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

// Wrap an angle into [0, 360).
double normDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

// Convert a speed to knots given the NMEA unit letter (N=knots, K=km/h, M=m/s).
double toKnots(double value, const QString& unit) {
    const QString u = unit.toUpper();
    if (u == QLatin1String("K")) return value / 1.852;       // km/h -> kn
    if (u == QLatin1String("M")) return value * 1.9438445;   // m/s  -> kn
    return value;                                            // N (or unspecified)
}
} // namespace

Nmea0183Client::Nmea0183Client(INavDataPublisher* publisher, QObject* parent)
    : QObject(parent), publisher_(publisher) {
    staleTimer_ = new QTimer(this);
    staleTimer_->setSingleShot(true);
    staleTimer_->setInterval(kStaleMs);
    connect(staleTimer_, &QTimer::timeout, this, &Nmea0183Client::onStaleTimeout);

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(kReconnectMs);
    connect(reconnectTimer_, &QTimer::timeout, this, &Nmea0183Client::tryReconnect);
}

Nmea0183Client::~Nmea0183Client() { stop(); }

void Nmea0183Client::setConfig(NmeaTransport transport, const QString& host,
                               quint16 port, bool enabled) {
    transport_ = transport;
    host_ = host;
    port_ = port;
    enabled_ = enabled;
    stop();
    if (enabled_) start();
}

void Nmea0183Client::start() {
    buffer_.clear();
    if (transport_ == NmeaTransport::Tcp) {
        tcp_ = new QTcpSocket(this);
        connect(tcp_, &QTcpSocket::connected,  this, &Nmea0183Client::onTcpConnected);
        connect(tcp_, &QTcpSocket::readyRead,  this, &Nmea0183Client::onTcpReadyRead);
        // Drop -> retry while still enabled.
        connect(tcp_, &QTcpSocket::disconnected, this, [this] {
            setDecoding(false);
            if (enabled_) reconnectTimer_->start();
        });
        connect(tcp_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            setDecoding(false);
            if (enabled_ && !reconnectTimer_->isActive()) reconnectTimer_->start();
        });
        tcp_->connectToHost(host_, port_);
    } else {
        udp_ = new QUdpSocket(this);
        connect(udp_, &QUdpSocket::readyRead, this, &Nmea0183Client::onUdpReadyRead);
        // Share so multiple listeners on the box can coexist.
        udp_->bind(QHostAddress::AnyIPv4, port_,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    }
}

void Nmea0183Client::stop() {
    reconnectTimer_->stop();
    staleTimer_->stop();
    if (tcp_) { tcp_->abort(); tcp_->deleteLater(); tcp_ = nullptr; }
    if (udp_) { udp_->close(); udp_->deleteLater(); udp_ = nullptr; }
    buffer_.clear();
    setDecoding(false);
}

bool Nmea0183Client::canTransmit() const {
    if (transport_ == NmeaTransport::Tcp)
        return tcp_ && tcp_->state() == QAbstractSocket::ConnectedState;
    return udp_ != nullptr;   // a bound UDP socket can always send/broadcast
}

void Nmea0183Client::transmit(const QByteArray& sentence) {
    if (sentence.isEmpty()) return;
    bool sent = false;
    if (transport_ == NmeaTransport::Tcp) {
        if (tcp_ && tcp_->state() == QAbstractSocket::ConnectedState) {
            tcp_->write(sentence);
            sent = true;
        }
    } else if (udp_) {
        // UDP listener has no fixed peer: send to the configured host if it is a
        // literal address, otherwise broadcast on the port.
        QHostAddress dst;
        if (host_.isEmpty() || !dst.setAddress(host_)) dst = QHostAddress::Broadcast;
        udp_->writeDatagram(sentence, dst, port_);
        sent = true;
    }
    if (sent) {
        QByteArray trimmed = sentence;
        while (!trimmed.isEmpty() &&
               (trimmed.endsWith('\r') || trimmed.endsWith('\n'))) trimmed.chop(1);
        emit sentenceTransmitted(QString::fromLatin1(trimmed));
    }
}

void Nmea0183Client::tryReconnect() {
    if (!enabled_ || transport_ != NmeaTransport::Tcp || !tcp_) return;
    if (tcp_->state() == QAbstractSocket::UnconnectedState)
        tcp_->connectToHost(host_, port_);
}

void Nmea0183Client::onTcpConnected() {
    reconnectTimer_->stop();
}

void Nmea0183Client::onTcpReadyRead() {
    if (tcp_) feed(tcp_->readAll());
}

void Nmea0183Client::onUdpReadyRead() {
    while (udp_ && udp_->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize(int(udp_->pendingDatagramSize()));
        udp_->readDatagram(dg.data(), dg.size());
        // A datagram may hold several sentences separated by CR/LF.
        for (const QByteArray& line : dg.split('\n'))
            processLine(line);
    }
}

// Reassemble a TCP byte stream into newline-delimited sentences, carrying any
// partial trailing line until the rest arrives.
void Nmea0183Client::feed(const QByteArray& bytes) {
    buffer_.append(bytes);
    int nl;
    while ((nl = buffer_.indexOf('\n')) >= 0) {
        processLine(buffer_.left(nl));
        buffer_.remove(0, nl + 1);
    }
    if (buffer_.size() > 4096) buffer_.clear();   // guard against a garbage stream
}

void Nmea0183Client::processLine(QByteArray line) {
    while (!line.isEmpty() && (line.endsWith('\r') || line.endsWith('\n') ||
                              line.endsWith(' '))) line.chop(1);
    if (line.isEmpty()) return;
    // Surface the raw line first so the debug window shows everything received,
    // including noise or sentences we don't decode.
    emit sentenceReceived(QString::fromLatin1(line));
    if (line.front() != '$' && line.front() != '!') return;
    if (!checksumOk(line)) return;
    handleSentence(QString::fromLatin1(line));
}

void Nmea0183Client::handleSentence(const QString& sentence) {
    // Strip the checksum, then split on commas. Address is "$ttSSS"; we match on
    // the 3-char sentence type (SSS), ignoring the talker id (tt).
    QString body = sentence;
    const int star = body.indexOf('*');
    if (star >= 0) body.truncate(star);
    const QStringList f = body.split(',');
    if (f.isEmpty() || f[0].size() < 4) return;
    const QString type = f[0].right(3).toUpper();
    // AIS rides the same connection; forward it (we don't decode it here) and
    // count it as activity so the link is considered "decoding".
    if (type == QLatin1String("VDM") || type == QLatin1String("VDO")) {
        emit aisSentence(sentence);
        markDecoding();
        return;
    }
    if      (type == QLatin1String("RMC")) parseRmc(f);
    else if (type == QLatin1String("GLL")) parseGll(f);
    else if (type == QLatin1String("GGA")) parseGga(f);
    else if (type == QLatin1String("VTG")) parseVtg(f);
    else if (type == QLatin1String("HDT")) parseHdt(f);
    else if (type == QLatin1String("HDG")) parseHdg(f);
    else if (type == QLatin1String("VHW")) parseVhw(f);
    else if (type == QLatin1String("DBT")) parseDbt(f);
    else if (type == QLatin1String("DPT")) parseDpt(f);
    else if (type == QLatin1String("MWV")) parseMwv(f);
    else if (type == QLatin1String("MWD")) parseMwd(f);
    else if (type == QLatin1String("VWR")) parseVwr(f);
    else if (type == QLatin1String("VWT")) parseVwt(f);
}

NavValueMeta Nmea0183Client::meta() const {
    NavValueMeta m;
    m.source = QStringLiteral("nmea0183");
    m.timestampUtc = QDateTime::currentDateTimeUtc();
    return m;
}

void Nmea0183Client::markDecoding() {
    setDecoding(true);
    staleTimer_->start();
}

// $--RMC,time,status,lat,N/S,lon,E/W,sog,cog,date,magvar,E/W,...
void Nmea0183Client::parseRmc(const QStringList& f) {
    if (f.size() < 9) return;
    if (f[2].toUpper() != QLatin1String("A")) return;      // V = void / no fix
    const auto lat = parseCoord(f[3], f[4]);
    const auto lon = parseCoord(f[5], f[6]);
    if (!lat || !lon) return;

    const NavValueMeta m = meta();
    publisher_->publishOwnshipPosition(*lat, *lon, m);

    const auto sog = parseNum(f[7]);
    const auto cog = parseNum(f[8]);
    if (sog && cog) publisher_->publishCogSog(*cog, *sog, m);

    if (f.size() >= 12) {                                   // magnetic variation
        if (auto var = parseNum(f[10]))
            publisher_->publishVariation(
                f[11].toUpper() == QLatin1String("W") ? -*var : *var, m);
    }
    markDecoding();
}

// $--GLL,lat,N/S,lon,E/W,time,status,mode
void Nmea0183Client::parseGll(const QStringList& f) {
    if (f.size() < 5) return;
    if (f.size() >= 7 && f[6].toUpper() != QLatin1String("A")) return;  // status if present
    const auto lat = parseCoord(f[1], f[2]);
    const auto lon = parseCoord(f[3], f[4]);
    if (!lat || !lon) return;
    publisher_->publishOwnshipPosition(*lat, *lon, meta());
    markDecoding();
}

// $--GGA,time,lat,N/S,lon,E/W,quality,numSV,HDOP,alt,M,...
void Nmea0183Client::parseGga(const QStringList& f) {
    if (f.size() < 6) return;
    if (f.size() >= 7 && f[6] == QLatin1String("0")) return; // fix quality 0 = no fix
    const auto lat = parseCoord(f[2], f[3]);
    const auto lon = parseCoord(f[4], f[5]);
    if (!lat || !lon) return;
    publisher_->publishOwnshipPosition(*lat, *lon, meta());
    markDecoding();
}

// $--VTG,cogTrue,T,cogMag,M,sog,N,sogKmh,K,mode
void Nmea0183Client::parseVtg(const QStringList& f) {
    if (f.size() < 6) return;
    const auto cog = parseNum(f[1]);
    const auto sog = parseNum(f[5]);
    if (!cog || !sog) return;
    publisher_->publishCogSog(*cog, *sog, meta());
    markDecoding();
}

// $--HDT,heading,T
void Nmea0183Client::parseHdt(const QStringList& f) {
    if (f.size() < 2) return;
    const auto h = parseNum(f[1]);
    if (!h) return;
    publisher_->publishHeading(normDeg(*h), std::nullopt, meta());
    markDecoding();
}

// $--HDG,magHeading,deviation,E/W,variation,E/W
void Nmea0183Client::parseHdg(const QStringList& f) {
    if (f.size() < 2) return;
    const auto mag = parseNum(f[1]);
    if (!mag) return;
    const NavValueMeta m = meta();

    std::optional<double> var;
    if (f.size() >= 6) {
        if (auto v = parseNum(f[4]))
            var = (f[5].toUpper() == QLatin1String("W")) ? -*v : *v;
    }
    // True = magnetic + variation (east positive), when variation is available.
    std::optional<double> trueHdg;
    if (var) trueHdg = normDeg(*mag + *var);
    publisher_->publishHeading(trueHdg, normDeg(*mag), m);
    if (var) publisher_->publishVariation(*var, m);
    markDecoding();
}

// $--VHW,headingTrue,T,headingMag,M,speedKn,N,speedKmh,K
void Nmea0183Client::parseVhw(const QStringList& f) {
    if (f.size() < 6) return;
    const NavValueMeta m = meta();
    const auto ht = parseNum(f[1]);
    const auto hm = parseNum(f[3]);
    if (ht || hm)
        publisher_->publishHeading(ht ? std::optional<double>(normDeg(*ht)) : std::nullopt,
                                   hm ? std::optional<double>(normDeg(*hm)) : std::nullopt, m);
    std::optional<double> spd = parseNum(f[5]);
    if (!spd && f.size() >= 8) { if (auto k = parseNum(f[7])) spd = *k / 1.852; }
    if (spd) publisher_->publishWaterSpeed(*spd, m);
    markDecoding();
}

// $--DBT,depthFeet,f,depthMetres,M,depthFathoms,F
void Nmea0183Client::parseDbt(const QStringList& f) {
    if (f.size() < 4) return;
    std::optional<double> metres = parseNum(f[3]);
    if (!metres) { if (auto ft = parseNum(f[1])) metres = *ft * 0.3048; }
    if (!metres) return;
    publisher_->publishDepth(*metres, meta());
    markDecoding();
}

// $--DPT,depthMetres,offset,maxRange
void Nmea0183Client::parseDpt(const QStringList& f) {
    if (f.size() < 2) return;
    const auto depth = parseNum(f[1]);
    if (!depth) return;
    double d = *depth;
    if (f.size() >= 3) { if (auto off = parseNum(f[2])) d += *off; }  // to water datum
    publisher_->publishDepth(d, meta());
    markDecoding();
}

// $--MWV,angle,reference,speed,units,status   (reference R=apparent, T=true)
void Nmea0183Client::parseMwv(const QStringList& f) {
    if (f.size() < 6) return;
    if (f[5].toUpper() != QLatin1String("A")) return;      // V = invalid
    const auto angle = parseNum(f[1]);
    const auto speed = parseNum(f[3]);
    if (!angle || !speed) return;
    const double kn = toKnots(*speed, f[4]);
    const double a  = normDeg(*angle);
    if (f[2].toUpper() == QLatin1String("T"))
        publisher_->publishTrueWind(kn, a, meta());
    else
        publisher_->publishApparentWind(kn, a, meta());
    markDecoding();
}

// $--MWD,dirTrue,T,dirMag,M,speedKn,N,speedMs,M
void Nmea0183Client::parseMwd(const QStringList& f) {
    if (f.size() < 6) return;
    const auto dir = parseNum(f[1]);
    std::optional<double> kn = parseNum(f[5]);
    if (!kn && f.size() >= 8) { if (auto ms = parseNum(f[7])) kn = *ms * 1.9438445; }
    if (!dir || !kn) return;
    publisher_->publishTrueWindDirection(normDeg(*dir), *kn, meta());
    markDecoding();
}

// $--VWR,angle,L/R,speedKn,N,speedMs,M,speedKmh,K   (relative/apparent wind)
// $--VWT,angle,L/R,speedKn,N,speedMs,M,speedKmh,K   (true wind, rel. to bow)
static std::optional<std::pair<double, double>> parseVwrLike(const QStringList& f) {
    if (f.size() < 4) return std::nullopt;
    const auto a = parseNum(f[1]);
    if (!a) return std::nullopt;
    const double angle = (f[2].toUpper() == QLatin1String("L")) ? (360.0 - *a) : *a;
    std::optional<double> kn = parseNum(f[3]);
    if (!kn && f.size() >= 6) { if (auto ms = parseNum(f[5])) kn = *ms * 1.9438445; }
    if (!kn && f.size() >= 8) { if (auto kmh = parseNum(f[7])) kn = *kmh / 1.852; }
    if (!kn) return std::nullopt;
    return std::make_pair(std::fmod(angle + 360.0, 360.0), *kn);
}

void Nmea0183Client::parseVwr(const QStringList& f) {
    if (auto r = parseVwrLike(f)) {
        publisher_->publishApparentWind(r->second, r->first, meta());
        markDecoding();
    }
}

void Nmea0183Client::parseVwt(const QStringList& f) {
    if (auto r = parseVwrLike(f)) {
        publisher_->publishTrueWind(r->second, r->first, meta());
        markDecoding();
    }
}

void Nmea0183Client::setDecoding(bool on) {
    if (on == decoding_) return;
    decoding_ = on;
    emit decodingChanged(on);
}

void Nmea0183Client::onStaleTimeout() {
    setDecoding(false);
}
