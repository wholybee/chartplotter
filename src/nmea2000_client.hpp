#pragma once
#include <QObject>
#include <QByteArray>
#include <QString>
#include <memory>

class INavDataPublisher;
class IAisPublisher;
class IN2kFrameParser;
class N2kDecoder;
class QTcpSocket;
class QUdpSocket;
class QTimer;
struct N2kFrame;

// Network transport for an NMEA 2000 gateway. Same shape as NmeaTransport in
// the 0183 client; kept distinct so the two clients can evolve independently.
enum class N2kTransport { Tcp, Udp };

// Wire format spoken by the gateway. Only Actisense N2K ASCII today; the
// enum + parser strategy lets us plug in alternatives (Yacht Devices RAW,
// Actisense binary, etc.) without touching the network code.
enum class N2kFormat { ActisenseAscii };

// Reads NMEA 2000 frames from a gateway over TCP or UDP, decodes the PGNs we
// care about, and publishes them through INavDataPublisher / IAisPublisher
// — the same path the NMEA 0183 client uses.
//
// "Decoding" status: while any recognised PGN keeps arriving, isDecoding() is
// true; if none arrive for 5 s it flips false. The UI shows a green dot next
// to the menu item while decoding.
//
// Reassembly: the gateway is expected to deliver fully-reassembled PGNs (the
// "N2K ASCII" format and its binary sibling both do); fast-packet stitching is
// not done here.
class Nmea2000Client : public QObject {
    Q_OBJECT
public:
    Nmea2000Client(INavDataPublisher* nav, IAisPublisher* ais, QObject* parent = nullptr);
    ~Nmea2000Client() override;

    bool isDecoding() const { return decoding_; }

    // Serialize a PGN frame to the active wire format and send it to the gateway
    // (which fragments fast-packet PGNs and puts them on the bus). No-op unless
    // the connection is enabled and up. Used by the navigation-output sender.
    void transmit(const N2kFrame& frame);

    // Whether transmit() would actually send right now: the connection is enabled
    // and the link is up (TCP connected, or a UDP socket exists). Drives the nav
    // display's transmit indicator.
    bool canTransmit() const;

public slots:
    // Apply a new configuration. Tears down any existing connection and, if
    // enabled, starts a fresh one. Safe to call repeatedly.
    void setConfig(N2kTransport transport, N2kFormat format,
                   const QString& host, quint16 port, bool enabled);

signals:
    void decodingChanged(bool on);

private slots:
    void onTcpConnected();
    void onTcpReadyRead();
    void onUdpReadyRead();
    void onStaleTimeout();
    void tryReconnect();

private:
    void start();
    void stop();
    void setDecoding(bool on);
    void feed(const QByteArray& bytes);
    void processLine(QByteArray line);
    void markDecoding();
    void rebuildParser();

    INavDataPublisher* navPub_ = nullptr;
    IAisPublisher*     aisPub_ = nullptr;
    std::unique_ptr<IN2kFrameParser> parser_;
    std::unique_ptr<N2kDecoder>      decoder_;

    N2kTransport transport_ = N2kTransport::Tcp;
    N2kFormat    format_    = N2kFormat::ActisenseAscii;
    QString  host_;
    quint16  port_ = 0;
    bool     enabled_ = false;

    QTcpSocket* tcp_ = nullptr;
    QUdpSocket* udp_ = nullptr;
    QByteArray  buffer_;
    QTimer*     staleTimer_ = nullptr;
    QTimer*     reconnectTimer_ = nullptr;
    bool        decoding_ = false;
};
