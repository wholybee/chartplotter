#include "core_api.hpp"
#include "nav_data_store.hpp"
#include "ais_target_store.hpp"
#include "side_menu.hpp"
#include "chart_view.hpp"
#include "chart_source.hpp"   // ChartSourceRegistry
#include "data_sources.hpp"
#include "dialog_chrome.hpp"

#include <QPushButton>
#include <QSettings>
#include <QDialog>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace {
// Handle handed back to a plugin data source: drives its menu item's status dot.
class DataSourceHandle : public IDataSource {
public:
    DataSourceHandle(SideMenu* menu, QPushButton* item) : menu_(menu), item_(item) {}
    void setActive(bool on) override { if (menu_) menu_->setItemDot(item_, on); }
private:
    SideMenu*    menu_ = nullptr;
    QPushButton* item_ = nullptr;
};

// Per-plugin persistent settings, namespaced under plugins/<id>/ in QSettings.
class PluginSettings : public IPluginSettings {
public:
    explicit PluginSettings(const QString& pluginId)
        : prefix_(QStringLiteral("plugins/") + pluginId + QLatin1Char('/')) {}
    void setValue(const QString& key, const QVariant& value) override {
        QSettings().setValue(prefix_ + key, value);
    }
    QVariant value(const QString& key, const QVariant& def) const override {
        return QSettings().value(prefix_ + key, def);
    }
private:
    QString prefix_;
};
} // namespace

CoreApi::CoreApi(NavDataStore* store, AisTargetStore* ais, RouteStore* routes,
                 SideMenu* menu, ChartView* view,
                 DataSourceRegistry* registry, ChartSourceRegistry* chartSources,
                 QWidget* dialogParent)
    : store_(store), ais_(ais), routes_(routes), menu_(menu), view_(view),
      registry_(registry), chartSources_(chartSources),
      dialogParent_(dialogParent) {}

CoreApi::~CoreApi() = default;

INavDataPublisher* CoreApi::navPublisher() {
    return store_;   // NavDataStore implements INavDataPublisher
}

IAisPublisher* CoreApi::aisPublisher() {
    return ais_;     // AisTargetStore implements IAisPublisher
}

void CoreApi::addMenuAction(const QString& title, std::function<void()> onTriggered) {
    menu_->addPluginAction(title, std::move(onTriggered));
}

void CoreApi::addMenuToggle(const QString& title, bool checked,
                            std::function<void(bool)> onToggled) {
    menu_->addPluginToggle(title, checked, std::move(onToggled));
}

IPluginSettings* CoreApi::pluginSettings(const QString& pluginId) {
    auto it = pluginSettings_.find(pluginId);
    if (it == pluginSettings_.end())
        it = pluginSettings_.emplace(pluginId,
                                     std::make_unique<PluginSettings>(pluginId)).first;
    return it->second.get();
}

void CoreApi::addSettingsPage(ISettingsPageProvider* provider) {
    if (!provider) return;
    menu_->addPluginSettingsItem(provider->settingsPageTitle(),
                                 [this, provider] { showSettingsPage(provider); });
}

void CoreApi::showSettingsPage(ISettingsPageProvider* provider) {
    if (!provider) return;
    // Single instance: reuse an already-open page for this provider.
    if (QDialog* existing = settingsDialogs_.value(provider)) {
        existing->show(); existing->raise(); existing->activateWindow();
        return;
    }
    // Host the plugin's content in the shared frameless side-menu dialog chrome
    // (the core owns the chrome; the plugin supplies only the page content). This
    // gives every plugin settings page the same white/blue look as the rest of
    // the app for free.
    auto* dlg = new QDialog(dialogParent_);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(provider->settingsPageTitle());

    // Modeless + self-deleting, single-instance via settingsDialogs_; dismissal
    // must go through close() (closeOnDismiss) so the close event fires, the
    // dialog is destroyed, and its QPointer clears.
    auto* panelCol = dialogchrome::setup(dlg, provider->settingsPageTitle(), true);
    if (QWidget* page = provider->createSettingsPage(dlg))
        panelCol->addWidget(page);

    auto* btnBar = new QWidget;
    auto* footer = new QHBoxLayout(btnBar);
    footer->setContentsMargins(16, 8, 16, 16);
    footer->addStretch(1);
    auto* closeBtn = new QPushButton(QStringLiteral("Close"));
    dialogchrome::styleAccentButton(closeBtn);
    footer->addWidget(closeBtn);
    panelCol->addWidget(btnBar);
    QObject::connect(closeBtn, &QPushButton::clicked, dlg, &QWidget::close);

    dlg->adjustSize();                 // size to the page's content (not resizable)
    dlg->setMinimumWidth(440);

    settingsDialogs_.insert(provider, dlg);   // QPointer clears when destroyed
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

IDataSource* CoreApi::registerDataSource(const QString& sourceId, const QString& name,
                                         std::function<void()> onOpenSettings) {
    QPushButton* item = menu_->addDataSourceItem(name, std::move(onOpenSettings));
    if (registry_) registry_->add(sourceId, name);   // joins Data Priority
    dataSources_.push_back(std::make_unique<DataSourceHandle>(menu_, item));
    return dataSources_.back().get();
}

void CoreApi::addChartOverlay(IChartOverlay* overlay)    { view_->addOverlay(overlay); }
void CoreApi::removeChartOverlay(IChartOverlay* overlay) { view_->removeOverlay(overlay); }
void CoreApi::requestChartRepaint()                      { view_->update(); }

void CoreApi::registerChartSource(IChartSource* source) {
    if (chartSources_) chartSources_->add(source);
}
void CoreApi::unregisterChartSource(IChartSource* source) {
    if (chartSources_) chartSources_->remove(source);
    // If this source is the one currently driving the view, drain its in-flight
    // loads before the plugin frees it (view_ and catalog_ outlive plugin
    // shutdown — see MainWindow's destructor note).
    if (view_) view_->onChartSourceUnregistered(source);
}
