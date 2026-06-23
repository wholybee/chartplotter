#include "nmea0183_plugin.hpp"
#include "nmea0183_debug_window.hpp"
#include "nmea0183_nav_sender.hpp"
#include "ais_decoder.hpp"
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
#include <QSettings>

Nmea0183Plugin::Nmea0183Plugin() = default;
Nmea0183Plugin::~Nmea0183Plugin() = default;

void Nmea0183Plugin::initialize(ICoreApi* core) {
    core_ = core;
    settings_ = core_->pluginSettings(QStringLiteral("nmea0183"));

    client_ = std::make_unique<Nmea0183Client>(core_->navPublisher());

    // AIS rides the same connection: forward !AIVDM/!AIVDO sentences from the
    // client to a decoder that publishes into the core AIS target store.
    ais_ = std::make_unique<AisDecoder>(core_->aisPublisher(), QStringLiteral("nmea0183"));
    QObject::connect(client_.get(), &Nmea0183Client::aisSentence, client_.get(),
                     [this](const QString& s) { if (ais_) ais_->handleSentence(s); });

    // Register as a data source: Data Connections item + Data Priority entry,
    // clicking opens our settings page.
    dataSource_ = core_->registerDataSource(QStringLiteral("nmea0183"),
                                            QStringLiteral("NMEA 0183"),
                                            [this] { core_->showSettingsPage(this); });

    // The green dot follows the decoding status.
    QObject::connect(client_.get(), &Nmea0183Client::decodingChanged, client_.get(),
                     [this](bool on) { if (dataSource_) dataSource_->setActive(on); });

    // While a route is being navigated, generate APB/RMB/RMC from the nav store
    // and transmit them back out this connection (skipped if the navigation data
    // itself originated from NMEA 0183, to prevent a feedback loop).
    navSender_ = std::make_unique<Nmea0183NavSender>(core_->navData(), client_.get(),
                                                     QStringLiteral("nmea0183"));

    loadConfig();
    client_->setConfig(transport_, host_, port_, enabled_);
}

void Nmea0183Plugin::shutdown() {
    if (dataSource_) dataSource_->setActive(false);
    navSender_.reset();   // stop transmitting before the client goes away
    client_.reset();      // stops the socket; disconnects everything
    ais_.reset();
    if (debug_) debug_->deleteLater();
}

void Nmea0183Plugin::loadConfig() {
    // One-time migration from the old core Settings keys ("nmea0183/*") so a
    // previously configured gateway keeps working after the refactor.
    if (!settings_->value(QStringLiteral("transport")).isValid()) {
        QSettings legacy;
        if (legacy.contains(QStringLiteral("nmea0183/transport"))) {
            settings_->setValue(QStringLiteral("transport"),
                                legacy.value(QStringLiteral("nmea0183/transport")));
            settings_->setValue(QStringLiteral("host"),
                                legacy.value(QStringLiteral("nmea0183/host")));
            settings_->setValue(QStringLiteral("port"),
                                legacy.value(QStringLiteral("nmea0183/port"), 10110));
            settings_->setValue(QStringLiteral("enabled"),
                                legacy.value(QStringLiteral("nmea0183/enabled"), false));
        }
    }
    transport_ = (settings_->value(QStringLiteral("transport"), QStringLiteral("tcp")).toString()
                      == QLatin1String("udp")) ? NmeaTransport::Udp : NmeaTransport::Tcp;
    host_     = settings_->value(QStringLiteral("host")).toString();
    port_     = quint16(settings_->value(QStringLiteral("port"), 10110).toUInt());
    enabled_  = settings_->value(QStringLiteral("enabled"), false).toBool();
}

void Nmea0183Plugin::applyConfig(NmeaTransport transport, const QString& host,
                                 quint16 port, bool enabled) {
    transport_ = transport;
    host_ = host;
    port_ = port;
    enabled_ = enabled;
    settings_->setValue(QStringLiteral("transport"),
                        transport == NmeaTransport::Udp ? QStringLiteral("udp")
                                                        : QStringLiteral("tcp"));
    settings_->setValue(QStringLiteral("host"), host);
    settings_->setValue(QStringLiteral("port"), int(port));
    settings_->setValue(QStringLiteral("enabled"), enabled);
    if (client_) client_->setConfig(transport, host, port, enabled);
}

void Nmea0183Plugin::showDebugWindow() {
    if (!debug_) {
        debug_ = new Nmea0183DebugWindow(core_->dialogParent());
        QObject::connect(client_.get(), &Nmea0183Client::sentenceReceived,
                         debug_.data(), &Nmea0183DebugWindow::appendLine);
        // Transmitted sentences render in a distinct colour (with a TX marker).
        QObject::connect(client_.get(), &Nmea0183Client::sentenceTransmitted,
                         debug_.data(), &Nmea0183DebugWindow::appendTxLine);
    }
    debug_->show();
    debug_->raise();
    debug_->activateWindow();
}

QWidget* Nmea0183Plugin::createSettingsPage(QWidget* parent) {
    const theme::MenuPalette& t = theme::menu();
    auto* page = new QWidget(parent);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* intro = new QLabel(QStringLiteral(
        "Connect to a WiFi gateway that broadcasts NMEA 0183 data over the network."));
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

    // Connection type.
    col->addWidget(dialogchrome::sectionHeader(QStringLiteral("Connection Type")));
    auto* typeBody = new QWidget;
    auto* typeRow = new QHBoxLayout(typeBody);
    typeRow->setContentsMargins(16, 4, 16, 8);
    auto* tcp = new QRadioButton(QStringLiteral("TCP"));
    auto* udp = new QRadioButton(QStringLiteral("UDP"));
    for (QRadioButton* r : {tcp, udp}) dialogchrome::styleRadio(r);
    (transport_ == NmeaTransport::Udp ? udp : tcp)->setChecked(true);
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
    portBox->setValue(port_ > 0 ? double(port_) : 10110.0);
    b->addWidget(portBox);

    // Enable.
    auto* enableBox = new QCheckBox(QStringLiteral("Enable connection"));
    enableBox->setChecked(enabled_);
    dialogchrome::styleCheckBox(enableBox);
    b->addWidget(enableBox);

    // Actions.
    auto* btnRow = new QHBoxLayout;
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"));
    auto* rawBtn   = new QPushButton(QStringLiteral("Show Raw Data…"));
    dialogchrome::styleAccentButton(applyBtn);
    dialogchrome::styleOutlinedButton(rawBtn);
    btnRow->addWidget(applyBtn);
    btnRow->addWidget(rawBtn);
    btnRow->addStretch(1);
    b->addLayout(btnRow);
    col->addWidget(body);

    QObject::connect(applyBtn, &QPushButton::clicked, page,
                     [this, tcp, udp, hostEdit, portBox, enableBox] {
        const NmeaTransport t = udp->isChecked() ? NmeaTransport::Udp : NmeaTransport::Tcp;
        applyConfig(t, hostEdit->text().trimmed(),
                    quint16(portBox->value()), enableBox->isChecked());
    });
    QObject::connect(rawBtn, &QPushButton::clicked, page, [this] { showDebugWindow(); });

    return page;
}
