#include "tauricpp/app.hpp"
#include <Windows.h>
#include <memory>

// Core 层
#include "Core/ConfigManager.h"
#include "Core/FileManager.h"

// Bridge 层
#include "Bridge/WebviewBridge.h"

// Services 层
#include "Services/AccountService.h"
#include "Services/VersionService.h"
#include "Services/DownloadService.h"
#include "Services/JavaService.h"
#include "Services/GameLaunchService.h"
#include "Services/ManifestService.h"
#include "Services/ModLoaderService.h"
#include "Services/ModrinthService.h"

#include <shellapi.h>

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    // 配置窗口
    tauricpp::App::Config config;
    config.window_config.title = "TauriCPP";
    config.window_config.width = 946;
    config.window_config.height = 568;
    config.window_config.center = true;
    config.window_config.devtools = false;
    config.window_config.borderless = true;

    // 初始化核心组件
    Core::ConfigManager::EnsureConfigExists();
    Core::ConfigManager::EnsureMinecraftDirs();
    Services::ManifestService::StartSync();

    // 创建应用
    tauricpp::App app(config);
    Bridge::WebviewBridge bridge(app);

    // 初始化各服务模块
    Services::AccountService accountService(bridge);
    Services::VersionService versionService(bridge);
    Services::DownloadService downloadService(bridge);
    downloadService.SetApp(&app);
    Services::JavaService javaService(bridge);
    Services::ModLoaderService modLoaderService(bridge);
    // 使用 unique_ptr 确保生命周期正确
    auto gameLaunchService = std::make_unique<Services::GameLaunchService>(bridge);
    gameLaunchService->SetApp(&app);
    Services::ModrinthService modrinthService(bridge);

    // shell.open - 在默认浏览器中打开URL
    bridge.RegisterCommand("shell.open", [](const nlohmann::json& args) -> nlohmann::json {
        std::string url = args.value("url", "");
        if (url.empty()) return {{"ok", false}, {"error", "Missing URL"}};
        HINSTANCE result = ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return {{"ok", result > (HINSTANCE)32}};
    });

    // 启动时检查登录状态，通知前端
    app.OnSetup([&app](tauricpp::App& app) {
        std::thread([&app]() {
            for (int attempt = 0; attempt < 20; attempt++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                std::string content = Core::ConfigManager::ReadConfigFile();
                nlohmann::json config;
                try {
                    config = nlohmann::json::parse(content);
                } catch (...) {
                    continue;
                }

                nlohmann::json accounts = nlohmann::json::array();
                for (int i = 0; i < 3; i++) {
                    accounts.push_back(Core::ConfigManager::HasAccount(config, i));
                }

                try {
                    app.GetBridge().Emit("account.init", {{"accounts", accounts}});
                } catch (...) {}

                for (int i = 0; i < 3; i++) {
                    if (Core::ConfigManager::HasAccount(config, i)) return;
                }
            }
        }).detach();
    });

    return app.Run();
}
