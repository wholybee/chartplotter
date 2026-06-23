#include "nmea2000_plugin.hpp"
#include "n2k_nav_sender.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QRadioButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

Nmea2000Plugin::Nmea2000Plugin() = default;
Nmea2000Plugin::~Nmea2000Plugin() = default;

void Nmea2000Plugin::initialize(ICoreApi* core) {
    core_ = core;
    settings_ = core_->pluginSettings(QStringLiteral("nmea2000"));

    // Hooks into the same nav + AIS publisher contracts the 0183 client uses,
    // so per-value source/timestamp arbitration, AIS target store updates,
    // and Data Priority all work without any extra wiring.
    client_ = std::make_unique<Nmea2000Client>(core_->navPublisher(),
                                               core_->aisPublisher());

    dataSource_ = core_->registerDataSource(QStringLiteral("nmea2000"),
                                            QStringLiteral("NMEA 2000"),
                                            [this] { core_->showSettingsPage(this); });

    QObject::connect(client_.get(), &Nmea2000Client::decodingChanged, client_.get(),
                     [this](bool on) { if (dataSource_) dataSource_->setActive(on); });

    // While a route is being navigated, transmit PGN 129283/129284/129285 from
    // the nav store out this connection (skipped if the navigation data itself
    // came from NMEA 2000, to prevent a feedback loop).
    navSender_ = std::make_unique<N2kNavSender>(core_->navData(), client_.get(),
                                                QStringLiteral("nmea2000"));

    loadConfig();
    client_->setConfig(transport_, format_, host_, port_, enabled_);
}

void Nmea2000Plugin::shutdown() {
    if (dataSource_) dataSource_->setActive(false);
    navSender_.reset();   // stop transmitting before the client goes away
    client_.reset();
}

void Nmea2000Plugin::loadConfig() {
    transport_ = (settings_->value(QStringLiteral("transport"), QStringLiteral("tcp")).toString()
                      == QLatin1String("udp")) ? N2kTransport::Udp : N2kTransport::Tcp;
    // Only one format today; the stored string lets us add more without a
    // schema change.
    const QString fmt = settings_->value(QStringLiteral("format"),
                                         QStringLiteral("actisense-ascii")).toString();
    format_ = (fmt == QLatin1String("actisense-ascii")) ? N2kFormat::ActisenseAscii
                                                       : N2kFormat::ActisenseAscii;
    host_     = settings_->value(QStringLiteral("host")).toString();
    port_     = quint16(settings_->value(QStringLiteral("port"), 2598).toUInt());
    enabled_  = settings_->value(QStringLiteral("enabled"), false).toBool();
}

void Nmea2000Plugin::applyConfig(N2kTransport transport, N2kFormat format,
                                 const QString& host, quint16 port, bool enabled) {
    transport_ = transport;
    format_    = format;
    host_      = host;
    port_      = port;
    enabled_   = enabled;
    settings_->setValue(QStringLiteral("transport"),
                        transport == N2kTransport::Udp ? QStringLiteral("udp")
                                                       : QStringLiteral("tcp"));
    settings_->setValue(QStringLiteral("format"),
                        format == N2kFormat::ActisenseAscii
                            ? QStringLiteral("actisense-ascii")
                            : QStringLiteral("actisense-ascii"));
    settings_->setValue(QStringLiteral("host"), host);
    settings_->setValue(QStringLiteral("port"), int(port));
    settings_->setValue(QStringLiteral("enabled"), enabled);
    if (client_) client_->setConfig(transport, format, host, port, enabled);
}

QWidget* Nmea2000Plugin::createSettingsPage(QWidget* parent) {
    const theme::MenuPalette& t = theme::menu();
    auto* page = new QWidget(parent);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* intro = new QLabel(QStringLiteral(
        "Connect to an NMEA 2000 gateway (e.g. Actisense W2K-1) that streams "
        "PGN data over the network."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:14px; color:%1; padding:12px 16px 4px 16px;")
                         .arg(t.actionFg));
    col->addWidget(intro);

    auto cap = [&](const QString& s) {
        auto* l = new QLabel(s);
        l->setWordWrap(true);
        l->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(theme::textMuted()));
        return l;
    };

    // Wire format. Only one option today; laid out so a second radio drops in
    // alongside without rework.
    col->addWidget(dialogchrome::sectionHeader(QStringLiteral("Data Format")));
    auto* fmtBody = new QWidget;
    auto* fmtCol = new QVBoxLayout(fmtBody);
    fmtCol->setContentsMargins(16, 4, 16, 8);
    auto* actisenseAscii = new QRadioButton(QStringLiteral("Actisense N2K ASCII"));
    dialogchrome::styleRadio(actisenseAscii);
    actisenseAscii->setChecked(format_ == N2kFormat::ActisenseAscii);
    fmtCol->addWidget(actisenseAscii);
    col->addWidget(fmtBody);

    // Transport.
    col->addWidget(dialogchrome::sectionHeader(QStringLiteral("Connection Type")));
    auto* typeBody = new QWidget;
    auto* typeRow = new QHBoxLayout(typeBody);
    typeRow->setContentsMargins(16, 4, 16, 8);
    auto* tcp = new QRadioButton(QStringLiteral("TCP"));
    auto* udp = new QRadioButton(QStringLiteral("UDP"));
    for (QRadioButton* r : {tcp, udp}) dialogchrome::styleRadio(r);
    (transport_ == N2kTransport::Udp ? udp : tcp)->setChecked(true);
    typeRow->addWidget(tcp);
    typeRow->addWidget(udp);
    typeRow->addStretch(1);
    col->addWidget(typeBody);

    // Server.
    col->addWidget(dialogchrome::sectionHeader(QStringLiteral("Server")));
    auto* body = new QWidget;
    auto* b = new QVBoxLayout(body);
    b->setContentsMargins(16, 4, 16, 12);
    b->setSpacing(8);

    // Host (TCP only).
    auto* hostRow = new QWidget;
    auto* hostCol = new QVBoxLayout(hostRow);
    hostCol->setContentsMargins(0, 0, 0, 0);
    hostCol->setSpacing(4);
    hostCol->addWidget(cap(QStringLiteral("Gateway IP address:")));
    auto* hostEdit = new QLineEdit(host_);
    hostEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.4.1"));
    dialogchrome::styleLineEdit(hostEdit);
    hostCol->addWidget(hostEdit);
    b->addWidget(hostRow);
    hostRow->setVisible(tcp->isChecked());
    QObject::connect(tcp, &QRadioButton::toggled, hostRow,
                     [hostRow](bool on) { hostRow->setVisible(on); });

    // Port.
    b->addWidget(cap(QStringLiteral("Port:")));
    auto* portBox = new TouchSpinBox;
    portBox->setRange(1, 65535);
    portBox->setSingleStep(1);
    portBox->setDecimals(0);
    portBox->setValue(port_ > 0 ? double(port_) : 2598.0);
    b->addWidget(portBox);

    // Enable.
    auto* enableBox = new QCheckBox(QStringLiteral("Enable connection"));
    enableBox->setChecked(enabled_);
    dialogchrome::styleCheckBox(enableBox);
    b->addWidget(enableBox);

    // Apply.
    auto* btnRow = new QHBoxLayout;
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"));
    dialogchrome::styleAccentButton(applyBtn);
    btnRow->addWidget(applyBtn);
    btnRow->addStretch(1);
    b->addLayout(btnRow);
    col->addWidget(body);

    QObject::connect(applyBtn, &QPushButton::clicked, page,
                     [this, tcp, udp, actisenseAscii, hostEdit, portBox, enableBox] {
        const N2kTransport t = udp->isChecked() ? N2kTransport::Udp : N2kTransport::Tcp;
        const N2kFormat f = actisenseAscii->isChecked() ? N2kFormat::ActisenseAscii
                                                        : N2kFormat::ActisenseAscii;
        applyConfig(t, f, hostEdit->text().trimmed(),
                    quint16(portBox->value()), enableBox->isChecked());
    });

    return page;
}
