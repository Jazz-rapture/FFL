#pragma once
// BackgroundService.h - 启动器背景图（4 张内置 + 用户自定义）
//
// 前端页面跑在 https://tauricpp.app 上，Chromium 不允许它加载 file:// 子资源，
// 所以自定义底图不能直接用本地路径：
//   · 内置图打进前端资源，走 https://tauricpp.app/resoures/background/
//   · 自定义图放在 <启动器目录>\Backgrounds\，由 main.cpp 注册的虚拟主机
//     https://backgrounds.tauricpp.app/ 映射过去（见 tauricpp::Window::Config::folder_hosts）
//
// 选择结果写在 config.json 的 appearance.background：
//   "pic3"                内置，值就是 key
//   "custom:xxx.jpg"      Backgrounds 目录里的文件名

#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "Bridge/WebviewBridge.h"
#include "Core/ConfigManager.h"
#include "Core/FileManager.h"
#include "Core/StringUtils.h"

namespace Services {

class BackgroundService {
public:
    explicit BackgroundService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    Bridge::WebviewBridge& bridge_;

    // 自定义底图的虚拟主机，必须和 main.cpp 里注册的 folder_hosts 一致
    static constexpr const char* kCustomHost = "https://backgrounds.tauricpp.app/";
    // 内置底图在前端资源里的根
    static constexpr const char* kBuiltinRoot = "https://tauricpp.app/resoures/background/";

    struct Builtin {
        const char* key;
        const char* name;
        const char* file;
    };

    // 内置底图：key 写进配置，name 是界面上显示的名字
    static const std::vector<Builtin>& Builtins() {
        static const std::vector<Builtin> list = {
            {"pic1", "流萤幻界", "pic1.svg"},
            {"pic3", "碧波清岛", "pic3.png"},
            {"pic4", "暮光尘境", "pic4.png"},
            {"pic6", "春樱牧歌", "pic6.png"},
        };
        return list;
    }

    static std::wstring BackgroundDir() {
        return Core::FileManager::GetExeDir() + L"\\Backgrounds";
    }

    // WebView2 是 Chromium 内核，能解这几种；.svg 留给内置图用
    static bool IsSupportedImage(const std::string& fileName) {
        std::string lower = fileName;
        for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

        static const char* kExts[] = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".avif"};
        for (const char* ext : kExts) {
            const std::string suffix(ext);
            if (lower.size() > suffix.size() &&
                lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        }
        return false;
    }

    // 文件名放进 URL 前做百分号编码：空格、# 之类原样拼进去会被当成路径分隔或锚点
    static std::string UrlEncode(const std::string& raw) {
        static const char* kHex = "0123456789ABCDEF";
        std::string out;
        out.reserve(raw.size() * 3);
        for (unsigned char c : raw) {
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                    c == '.' || c == '~';
            if (unreserved) {
                out.push_back(static_cast<char>(c));
                continue;
            }
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
        return out;
    }

    // Backgrounds 目录里的可用图片（顺带把目录建出来）
    static std::vector<std::string> ListCustomFiles() {
        std::vector<std::string> files;
        const std::wstring dir = BackgroundDir();
        Core::FileManager::CreateDirRecursive(dir);

        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return files;

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string name = Core::StringUtils::WideToUtf8(findData.cFileName);
            if (!IsSupportedImage(name)) continue;
            files.push_back(name);
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);

        return files;
    }

    static nlohmann::json CustomEntry(const std::string& fileName) {
        // 去掉扩展名当显示名，文件名太长时界面自己截断
        std::string display = fileName;
        const size_t dot = display.find_last_of('.');
        if (dot != std::string::npos && dot > 0) display.resize(dot);

        return {{"key", "custom:" + fileName},
                {"name", display},
                {"file", fileName},
                {"url", std::string(kCustomHost) + UrlEncode(fileName)}};
    }

    static nlohmann::json BuiltinEntry(const Builtin& item) {
        return {{"key", item.key},
                {"name", item.name},
                {"url", std::string(kBuiltinRoot) + item.file}};
    }

    // 把配置里的 key 解析成 {key, name, url}。
    // 自定义文件被删掉时退回第一张内置图 —— 否则用户会看到一片纯渐变，
    // 还得自己猜到是文件没了
    static nlohmann::json ResolveKey(const std::string& key) {
        if (key.rfind("custom:", 0) == 0) {
            const std::string fileName = key.substr(7);
            const std::wstring path = BackgroundDir() + L"\\" +
                                      Core::StringUtils::Utf8ToWide(fileName);
            if (!fileName.empty() && IsSupportedImage(fileName) &&
                Core::FileManager::FileExists(path)) {
                return CustomEntry(fileName);
            }
        } else {
            for (const auto& item : Builtins()) {
                if (key == item.key) return BuiltinEntry(item);
            }
        }
        return BuiltinEntry(Builtins().front());
    }

    static std::string ReadSavedKey() {
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            return "";
        }
        if (!config.contains("appearance") || !config["appearance"].is_object()) return "";
        const auto& appearance = config["appearance"];
        if (!appearance.contains("background") || !appearance["background"].is_string()) return "";
        return appearance["background"].get<std::string>();
    }

    static bool WriteSavedKey(const std::string& key) {
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            config = nlohmann::json::object();
        }
        if (!config.contains("appearance") || !config["appearance"].is_object()) {
            config["appearance"] = nlohmann::json::object();
        }
        config["appearance"]["background"] = key;
        return Core::ConfigManager::WriteConfigFile(config.dump(4));
    }

    void RegisterCommands() {
        // background.list - 内置 + 自定义底图清单，以及当前选中的那张
        bridge_.RegisterCommand("background.list", [](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json builtins = nlohmann::json::array();
            for (const auto& item : Builtins()) builtins.push_back(BuiltinEntry(item));

            nlohmann::json custom = nlohmann::json::array();
            for (const auto& fileName : ListCustomFiles()) custom.push_back(CustomEntry(fileName));

            return {{"ok", true},
                    {"current", ResolveKey(ReadSavedKey())},
                    {"builtins", builtins},
                    {"custom", custom},
                    {"folder", Core::StringUtils::WideToUtf8(BackgroundDir())}};
        });

        // background.select - 选定一张底图并写进配置
        bridge_.RegisterCommand("background.select", [](const nlohmann::json& args) -> nlohmann::json {
            const std::string key = args.value("key", "");
            if (key.empty()) return {{"ok", false}, {"error", "缺少背景标识"}};

            // 只接受能解析出来的 key：内置 key 必须对得上表，
            // 自定义文件必须真的在 Backgrounds 里
            nlohmann::json resolved = ResolveKey(key);
            if (resolved.value("key", "") != key) {
                return {{"ok", false}, {"error", "背景不存在，可能该文件已被删除"}};
            }
            if (!WriteSavedKey(key)) {
                return {{"ok", false}, {"error", "保存背景设置失败"}};
            }
            return {{"ok", true}, {"current", resolved}};
        });

        // background.openFolder - 打开 Backgrounds 目录，让用户往里放图
        bridge_.RegisterCommand("background.openFolder", [](const nlohmann::json&) -> nlohmann::json {
            const std::wstring dir = BackgroundDir();
            Core::FileManager::CreateDirRecursive(dir);
            HINSTANCE result = ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr,
                                             SW_SHOWNORMAL);
            return {{"ok", result > (HINSTANCE)32}, {"folder", Core::StringUtils::WideToUtf8(dir)}};
        });
    }
};

} // namespace Services
