#include "nmea2000_client.hpp"
#include "actisense_ascii_parser.hpp"
#include "n2k_frame.hpp"
#include "n2k_decoder.hpp"

#include <QTcpSocket>
#include <QUdpSocket>
#include <QTimer>
#include <QHostAddress>
#include <QDateTime>

namespace {
constexpr int kStaleMs     = 5000;
constexpr int kReconnectMs = 3000;
constexpr const char* kSource = "nmea2000";
} // namespace

Nmea2000Client::Nmea2000Client(INavDataPublisher* nav, IAisPublisher* ais,
                               QObject* parent)
    : QObject(parent), navPub_(nav), aisPub_(ais) {
    staleTimer_ = new QTimer(this);
    staleTimer_->setSingleShot(true);
    staleTimer_->setInterval(kStaleMs);
    connect(staleTimer_, &QTimer::timeout, this, &Nmea2000Client::onStaleTimeout);

    reconnectTimer_ = new QTimer(this);
    reconnectTimer_->setInterval(kReconnectMs);
    connect(reconnectTimer_, &QTimer::timeout, this, &Nmea2000Client::tryReconnect);

    decoder_ = std::make_unique<N2kDecoder>(navPub_, aisPub_, QString::fromLatin1(kSource));
    rebuildParser();
}

Nmea2000Client::~Nmea2000Client() { stop(); }

void Nmea2000Client::rebuildParser() {
    switch (format_) {
        case N2kFormat::ActisenseAscii:
        default:
            parser_ = std::make_unique<ActisenseAsciiParser>();
            break;
    }
}

void Nmea2000Client::setConfig(N2kTransport transport, N2kFormat format,
                               const QString& host, quint16 port, bool enabled) {
    const bool formatChanged = (format != format_);
    transport_ = transport;
    format_    = format;
    host_      = host;
    port_      = port;
    enabled_   = enabled;
    if (formatChanged) rebuildParser();
    stop();
    if (enabled_) start();
}

void Nmea2000Client::start() {
    buffer_.clear();
    if (transport_ == N2kTransport::Tcp) {
        tcp_ = new QTcpSocket(this);
        connect(tcp_, &QTcpSocket::connected, this, &Nmea2000Client::onTcpConnected);
        connect(tcp_, &QTcpSocket::readyRead, this, &Nmea2000Client::onTcpReadyRead);
        connect(tcp_, &QTcpSocket::disconnected, this, [this] {
            setDecoding(false);
            if (enabled_) reconnectTimer_->start();
        });
        connect(tcp_, &QAbstractSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) {
            setDecoding(false);
            if (enabled_ && !reconnectTimer_->isActive()) reconnectTimer_->start();
        });
        tcp_->connectToHost(host_, port_);
    } else {
        udp_ = new QUdpSocket(this);
        connect(udp_, &QUdpSocket::readyRead, this, &Nmea2000Client::onUdpReadyRead);
        udp_->bind(QHostAddress::AnyIPv4, port_,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    }
}

void Nmea2000Client::stop() {
    reconnectTimer_->stop();
    staleTimer_->stop();
    if (tcp_) { tcp_->abort(); tcp_->deleteLater(); tcp_ = nullptr; }
    if (udp_) { udp_->close(); udp_->deleteLater(); udp_ = nullptr; }
    buffer_.clear();
    setDecoding(false);
}

void Nmea2000Client::tryReconnect() {
    if (!enabled_ || transport_ != N2kTransport::Tcp || !tcp_) return;
    if (tcp_->state() == QAbstractSocket::UnconnectedState)
        tcp_->connectToHost(host_, port_);
}

void Nmea2000Client::onTcpConnected() { reconnectTimer_->stop(); }

void Nmea2000Client::onTcpReadyRead() {
    if (tcp_) feed(tcp_->readAll());
}

void Nmea2000Client::onUdpReadyRead() {
    while (udp_ && udp_->hasPendingDatagrams()) {
        QByteArray dg;
        dg.resize(int(udp_->pendingDatagramSize()));
        udp_->readDatagram(dg.data(), dg.size());
        for (const QByteArray& line : dg.split('\n'))
            processLine(line);
    }
}

void Nmea2000Client::feed(const QByteArray& bytes) {
    buffer_.append(bytes);
    int nl;
    while ((nl = buffer_.indexOf('\n')) >= 0) {
        processLine(buffer_.left(nl));
        buffer_.remove(0, nl + 1);
    }
    // N2K ASCII lines are short (~50-100 bytes); 8 KB is well above the
    // longest legitimate frame so anything bigger is a garbage stream.
    if (buffer_.size() > 8192) buffer_.clear();
}

void Nmea2000Client::processLine(QByteArray line) {
    while (!line.isEmpty() && (line.endsWith('\r') || line.endsWith(' ')))
        line.chop(1);
    if (line.isEmpty() || !parser_) return;
    N2kFrame frame;
    if (!parser_->parse(line, frame, QDateTime::currentDateTimeUtc())) return;
    if (decoder_ && decoder_->decode(frame)) markDecoding();
}

namespace {
// Serialize a frame to one Actisense "N2K ASCII" line, the same format the
// parser reads: "A<hhmmss.ddd> <SSDDP> <PGN> <hex…>". The gateway reassembles
// fast-packet PGNs from the full payload we provide here.
QByteArray serializeActisenseAscii(const N2kFrame& f) {
    auto hex = [](quint32 v, int width) {
        return QByteArray::number(v, 16).rightJustified(width, '0').toUpper();
    };
    QByteArray out;
    out += 'A';
    out += QDateTime::currentDateTimeUtc().time()
               .toString(QStringLiteral("HHmmss.zzz")).toLatin1();
    out += ' ';
    out += hex(f.src, 2);
    out += hex(f.dst, 2);
    out += hex(f.prio & 0x07, 1);
    out += ' ';
    out += hex(f.pgn, 5);
    out += ' ';
    for (char b : f.data) out += hex(quint8(b), 2);
    out += "\r\n";
    return out;
}
} // namespace

bool Nmea2000Client::canTransmit() const {
    if (!enabled_) return false;
    if (transport_ == N2kTransport::Tcp)
        return tcp_ && tcp_->state() == QAbstractSocket::ConnectedState;
    return udp_ != nullptr;
}

void Nmea2000Client::transmit(const N2kFrame& frame) {
    if (!enabled_) return;
    const QByteArray line = serializeActisenseAscii(frame);
    if (transport_ == N2kTransport::Tcp) {
        if (tcp_ && tcp_->state() == QAbstractSocket::ConnectedState)
            tcp_->write(line);
    } else if (udp_) {
        // Send to the configured gateway address, or broadcast if none is set.
        QHostAddress dst;
        if (host_.isEmpty() || !dst.setAddress(host_)) dst = QHostAddress::Broadcast;
        udp_->writeDatagram(line, dst, port_);
    }
}

void Nmea2000Client::markDecoding() {
    setDecoding(true);
    staleTimer_->start();
}

void Nmea2000Client::setDecoding(bool on) {
    if (on == decoding_) return;
    decoding_ = on;
    emit decodingChanged(on);
}

void Nmea2000Client::onStaleTimeout() { setDecoding(false); }
