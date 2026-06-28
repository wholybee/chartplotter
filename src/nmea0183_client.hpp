#pragma once
#include <QObject>
#include <QString>
#include <QByteArray>

class INavDataPublisher;
struct NavValueMeta;
class QTcpSocket;
class QUdpSocket;
class QTimer;

// Network transport for the NMEA 0183 gateway.
enum class NmeaTransport { Tcp, Udp };

// Reads NMEA 0183 sentences from a WiFi gateway over TCP or UDP, decodes the
// position sentences we care about (RMC, GLL), and publishes them into the
// NavDataStore through the INavDataPublisher contract.
//
// "Decoding" status: while valid position sentences keep arriving, isDecoding()
// is true; if none arrive for the timeout (5 s) it flips false. The UI shows a
// green dot next to the menu item while decoding.
//
// TCP connects to host:port and reassembles the byte stream into lines. UDP
// binds to the port and reads datagrams (IP is irrelevant for a listener).
// Reconnection is attempted periodically while enabled.
class Nmea0183Client : public QObject {
    Q_OBJECT
public:
    explicit Nmea0183Client(INavDataPublisher* publisher, QObject* parent = nullptr);
    ~Nmea0183Client() override;

    bool isDecoding() const { return decoding_; }

    // Whether transmit() would actually put bytes on the wire right now: the link
    // is up (TCP connected, or a bound UDP socket exists). Drives the nav
    // display's transmit indicator, which must reflect real link state rather
    // than just the loop guard.
    bool canTransmit() const;

public slots:
    // Apply a new configuration. Tears down any existing connection and, if
    // enabled, starts a fresh one. Safe to call repeatedly.
    void setConfig(NmeaTransport transport, const QString& host,
                   quint16 port, bool enabled);

    // Send a fully-formed NMEA 0183 sentence (including the leading '$' and
    // trailing CR/LF) out on the active connection. No-op when not connected.
    // Used to transmit generated navigation sentences (APB/RMB/RMC).
    void transmit(const QByteArray& sentence);

signals:
    // True while valid position sentences are arriving (within the timeout).
    void decodingChanged(bool on);
    // Every raw line received (after CR/LF trimming), for the debug window.
    void sentenceReceived(const QString& line);
    // A line we transmitted (trimmed), for the debug window.
    void sentenceTransmitted(const QString& line);
    // A checksum-valid AIS sentence (!AIVDM / !AIVDO), forwarded for decoding.
    void aisSentence(const QString& line);

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
    void feed(const QByteArray& bytes);      // assemble + dispatch lines (TCP)
    void processLine(QByteArray line);       // one raw sentence
    void handleSentence(const QString& sentence);
    NavValueMeta meta() const;               // source + now() for a publish
    void markDecoding();                     // mark active + restart the timeout

    void parseRmc(const QStringList& f);     // position, COG/SOG, variation
    void parseGll(const QStringList& f);     // position
    void parseGga(const QStringList& f);     // position
    void parseVtg(const QStringList& f);     // COG/SOG
    void parseHdt(const QStringList& f);     // heading (true)
    void parseHdg(const QStringList& f);     // heading (mag) + variation
    void parseVhw(const QStringList& f);     // heading + water speed
    void parseDbt(const QStringList& f);     // depth below transducer
    void parseDpt(const QStringList& f);     // depth + offset
    void parseMwv(const QStringList& f);     // wind angle/speed (apparent or true)
    void parseMwd(const QStringList& f);     // true wind direction + speed
    void parseVwr(const QStringList& f);     // relative (apparent) wind
    void parseVwt(const QStringList& f);     // true wind (rel. to bow)

    INavDataPublisher* publisher_ = nullptr;

    NmeaTransport transport_ = NmeaTransport::Tcp;
    QString  host_;
    quint16  port_ = 0;
    bool     enabled_ = false;

    QTcpSocket* tcp_ = nullptr;
    QUdpSocket* udp_ = nullptr;
    QByteArray  buffer_;                      // partial TCP line carry-over
    QTimer*     staleTimer_ = nullptr;        // fires when data stops (5 s)
    QTimer*     reconnectTimer_ = nullptr;    // retry a dropped/failed TCP link
    bool        decoding_ = false;
};
