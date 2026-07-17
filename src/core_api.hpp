#pragma once
#include "plugin_api.hpp"
#include <QString>
#include <QHash>
#include <QPointer>
#include <map>
#include <memory>
#include <vector>

class QDialog;

class NavDataStore;
class AisTargetStore;
class RouteStore;
class SideMenu;
class ChartView;
class QWidget;
class DataSourceRegistry;
class ChartSourceRegistry;
class RasterChartSourceRegistry;
class IChartSource;
class IRasterChartSource;

// Concrete ICoreApi: routes plugin calls to the real core objects. Owned by the
// PluginManager; references (does not own) the core objects it bridges to.
class CoreApi : public ICoreApi {
public:
    CoreApi(NavDataStore* store, AisTargetStore* ais, RouteStore* routes,
            SideMenu* menu, ChartView* view,
            DataSourceRegistry* registry, ChartSourceRegistry* chartSources,
            RasterChartSourceRegistry* rasterSources,
            QWidget* dialogParent);
    ~CoreApi() override;

    INavDataPublisher*  navPublisher() override;
    const NavDataStore* navData() const override { return store_; }

    IAisPublisher*        aisPublisher() override;
    const AisTargetStore* aisData() const override { return ais_; }

    RouteStore* routes() override { return routes_; }

    void addMenuAction(const QString& title, std::function<void()> onTriggered) override;
    void addMenuToggle(const QString& title, bool checked,
                       std::function<void(bool)> onToggled) override;

    IPluginSettings* pluginSettings(const QString& pluginId) override;

    void addSettingsPage(ISettingsPageProvider* provider) override;
    void showSettingsPage(ISettingsPageProvider* provider) override;

    IDataSource* registerDataSource(const QString& sourceId, const QString& name,
                                    std::function<void()> onOpenSettings) override;

    void addChartOverlay(IChartOverlay* overlay) override;
    void removeChartOverlay(IChartOverlay* overlay) override;
    void requestChartRepaint() override;

    void registerChartSource(IChartSource* source) override;
    void unregisterChartSource(IChartSource* source) override;

    void registerRasterChartSource(IRasterChartSource* source) override;
    void unregisterRasterChartSource(IRasterChartSource* source) override;

    QWidget* dialogParent() override { return dialogParent_; }

private:
    NavDataStore*       store_ = nullptr;
    AisTargetStore*     ais_ = nullptr;
    RouteStore*         routes_ = nullptr;
    SideMenu*           menu_ = nullptr;
    ChartView*          view_ = nullptr;
    DataSourceRegistry* registry_ = nullptr;
    ChartSourceRegistry* chartSources_ = nullptr;
    RasterChartSourceRegistry* rasterSources_ = nullptr;
    QWidget*            dialogParent_ = nullptr;
    std::vector<std::unique_ptr<IDataSource>>     dataSources_;     // owns handles
    std::map<QString, std::unique_ptr<IPluginSettings>> pluginSettings_;  // by plugin id
    QHash<ISettingsPageProvider*, QPointer<QDialog>> settingsDialogs_;    // single-instance
};
