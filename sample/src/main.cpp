#include "tauricpp/app.hpp"
#include <Windows.h>
#include <memory>

#include "tauricpp/clipboard.hpp"

// Core 层
#include "Core/ConfigManager.h"
#include "Core/FileManager.h"

// Bridge 层
#include "Bridge/WebviewBridge.h"

// Services 层
#include "Services/AccountService.h"
#include "Services/BackgroundService.h"
#include "Services/MsaAuthService.h"
#include "Services/YggdrasilAuthService.h"
#include "Services/VersionService.h"
#include "Services/DownloadService.h"
#include "Services/JavaService.h"
#include "Services/GameLaunchService.h"
#include "Services/ManifestService.h"
#include "Services/ModLoaderService.h"
#include "Services/ModpackService.h"
#include "Services/ModrinthService.h"
#include "Services/MultiplayerService.h"

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
    // 窗口客户区的底色 = 前端 body 的 background-color（css/style.css）。
    // 它既是 WebView2 起不来、或兜底计时器触发时用户看到的颜色，
    // 也是透明窗口背后垫着的颜色，所以得和页面底色一致，否则会看到一次跳变
    config.window_config.bg_color = RGB(0x6a, 0x6c, 0xc4);
    // 等页面渲染出来再显示窗口（窗口以"可见但全透明"的方式存在）
    config.window_config.reveal_when_ready = true;
    // 自定义背景图目录映射给前端：页面是 https://tauricpp.app，加载不了 file://，
    // 只能走这个虚拟主机（见 Services/BackgroundService.h）
    config.window_config.folder_hosts.push_back(
        {"backgrounds.tauricpp.app", Core::FileManager::GetExeDir() + L"\\Backgrounds"});

    // 初始化核心组件
    Core::ConfigManager::EnsureConfigExists();
    Core::ConfigManager::EnsureMinecraftDirs();
    Services::ManifestService::StartSync();

    // 创建应用
    tauricpp::App app(config);
    Bridge::WebviewBridge bridge(app);

    // 初始化各服务模块
    Services::AccountService accountService(bridge);
    Services::MsaAuthService msaAuthService(bridge);
    Services::YggdrasilAuthService yggdrasilAuthService(bridge);
    Services::VersionService versionService(bridge);
    Services::DownloadService downloadService(bridge);
    downloadService.SetApp(&app);
    Services::JavaService javaService(bridge);
    Services::ModLoaderService modLoaderService(bridge);
    // 整合包安装：复用上面两个服务的下载与加载器安装实现
    Services::ModpackService modpackService(bridge, downloadService, modLoaderService);
    modpackService.SetApp(&app);
    // 使用 unique_ptr 确保生命周期正确
    auto gameLaunchService = std::make_unique<Services::GameLaunchService>(bridge);
    gameLaunchService->SetApp(&app);
    Services::ModrinthService modrinthService(bridge);
    modrinthService.SetApp(&app);
    // 联机（内网穿透）：令牌来自用户输入，隧道由本地 frpc 拉起
    Services::MultiplayerService multiplayerService(bridge);
    // 启动器背景图（内置 4 张 + Backgrounds 目录里的自定义图）
    Services::BackgroundService backgroundService(bridge);

    // shell.open - 在默认浏览器中打开URL
    bridge.RegisterCommand("shell.open", [](const nlohmann::json& args) -> nlohmann::json {
        std::string url = args.value("url", "");
        if (url.empty()) return {{"ok", false}, {"error", "Missing URL"}};
        HINSTANCE result = ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return {{"ok", result > (HINSTANCE)32}};
    });

    // clipboard.write - 写入系统剪贴板（联机页复制连接地址用）
    bridge.RegisterCommand("clipboard.write", [](const nlohmann::json& args) -> nlohmann::json {
        std::string text = args.value("text", "");
        if (text.empty()) return {{"ok", false}, {"error", "Missing text"}};
        return {{"ok", tauricpp::Clipboard::WriteText(text)}};
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
                for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
                    accounts.push_back(Core::ConfigManager::HasAccount(config, i));
                }

                try {
                    app.GetBridge().Emit("account.init", {{"accounts", accounts}});
                } catch (...) {}

                for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
                    if (Core::ConfigManager::HasAccount(config, i)) return;
                }
            }
        }).detach();
    });

    return app.Run();
}
