#include "signalk_plugin.hpp"
#include "signalk_client.hpp"
#include "signalk_decoder.hpp"
#include "touch_spin_box.hpp"
#include "theme.hpp"
#include "dialog_chrome.hpp"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

namespace {
constexpr quint16 kDefaultPort = 80;
const QString kSourceId = QStringLiteral("signalk");
}  // namespace

SignalKPlugin::SignalKPlugin() = default;
SignalKPlugin::~SignalKPlugin() = default;

void SignalKPlugin::initialize(ICoreApi* core) {
    core_     = core;
    settings_ = core_->pluginSettings(kSourceId);

    client_  = std::make_unique<SignalKClient>();
    decoder_ = std::make_unique<SignalKDecoder>(core_->navPublisher(),
                                                 core_->aisPublisher(),
                                                 kSourceId);
    // Each delta frame from the server goes through the decoder, which
    // dispatches to the nav and AIS publishers.
    QObject::connect(client_.get(), &SignalKClient::messageReceived,
                     decoder_.get(), &SignalKDecoder::handleMessage);
    // One-shot snapshot from the REST API at connect time so AIS names cached
    // server-side appear immediately instead of waiting for the next static
    // report (Type 5 every 6 min; Type 24 less frequent for Class B).
    QObject::connect(client_.get(), &SignalKClient::snapshotReceived,
                     decoder_.get(), &SignalKDecoder::handleSnapshot);

    // Data Connections item (with priority entry); clicking opens our page.
    dataSource_ = core_->registerDataSource(kSourceId, QStringLiteral("Signal K"),
                                            [this] { core_->showSettingsPage(this); });

    QObject::connect(client_.get(), &SignalKClient::decodingChanged, client_.get(),
                     [this](bool on) { if (dataSource_) dataSource_->setActive(on); });

    loadConfig();
    client_->setConfig(host_, port_, enabled_);
}

void SignalKPlugin::shutdown() {
    if (dataSource_) dataSource_->setActive(false);
    client_.reset();      // stops the socket
    decoder_.reset();
}

void SignalKPlugin::loadConfig() {
    host_    = settings_->value(QStringLiteral("host")).toString();
    port_    = quint16(settings_->value(QStringLiteral("port"),
                                        int(kDefaultPort)).toUInt());
    enabled_ = settings_->value(QStringLiteral("enabled"), false).toBool();
}

void SignalKPlugin::applyConfig(const QString& host, quint16 port, bool enabled) {
    host_    = host;
    port_    = port;
    enabled_ = enabled;
    settings_->setValue(QStringLiteral("host"), host);
    settings_->setValue(QStringLiteral("port"), int(port));
    settings_->setValue(QStringLiteral("enabled"), enabled);
    if (client_) client_->setConfig(host, port, enabled);
}

QWidget* SignalKPlugin::createSettingsPage(QWidget* parent) {
    const theme::MenuPalette& t = theme::menu();
    auto* page = new QWidget(parent);
    auto* col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    auto* intro = new QLabel(QStringLiteral(
        "Connect to a Signal K server. The plugin opens a WebSocket to "
        "ws://host:port/signalk/v1/stream and decodes navigation values and "
        "AIS targets. Default port is 80 (use 3000 for the signalk-server-node "
        "default)."));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("font-size:14px; color:%1; padding:12px 16px 4px 16px;")
                         .arg(t.actionFg));
    col->addWidget(intro);

    col->addWidget(dialogchrome::sectionHeader(QStringLiteral("Server")));
    auto* body = new QWidget;
    auto* b = new QVBoxLayout(body);
    b->setContentsMargins(16, 4, 16, 12);
    b->setSpacing(8);

    auto cap = [&](const QString& s) {
        auto* l = new QLabel(s);
        l->setWordWrap(true);
        l->setStyleSheet(QStringLiteral("font-size:13px; color:%1;").arg(theme::textMuted()));
        return l;
    };

    // Host.
    b->addWidget(cap(QStringLiteral("Server IP address or hostname:")));
    auto* hostEdit = new QLineEdit(host_);
    hostEdit->setPlaceholderText(QStringLiteral("e.g. 192.168.4.1"));
    dialogchrome::styleLineEdit(hostEdit);
    b->addWidget(hostEdit);

    // Port.
    b->addWidget(cap(QStringLiteral("Port:")));
    auto* portBox = new TouchSpinBox;
    portBox->setRange(1, 65535);
    portBox->setSingleStep(1);
    portBox->setDecimals(0);
    portBox->setValue(port_ > 0 ? double(port_) : double(kDefaultPort));
    b->addWidget(portBox);

    // Enable.
    auto* enableBox = new QCheckBox(QStringLiteral("Enable connection"));
    enableBox->setChecked(enabled_);
    dialogchrome::styleCheckBox(enableBox);
    b->addWidget(enableBox);

    // Actions.
    auto* btnRow = new QHBoxLayout;
    auto* applyBtn = new QPushButton(QStringLiteral("Apply"));
    dialogchrome::styleAccentButton(applyBtn);
    btnRow->addWidget(applyBtn);
    btnRow->addStretch(1);
    b->addLayout(btnRow);
    col->addWidget(body);

    QObject::connect(applyBtn, &QPushButton::clicked, page,
                     [this, hostEdit, portBox, enableBox] {
        applyConfig(hostEdit->text().trimmed(),
                    quint16(portBox->value()),
                    enableBox->isChecked());
    });

    return page;
}
