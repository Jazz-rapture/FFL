#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/HttpClient.h"
#include "Core/FileManager.h"
#include "Core/StringUtils.h"
#include "Bridge/WebviewBridge.h"
#include "Services/MavenUtils.h"

namespace Services {

class VersionService {
public:
    VersionService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

    // 供 ModpackService 复用：把装好的版本登记进 config.json（等价于 version.save）
    static nlohmann::json SaveVersionEntry(const nlohmann::json& args) {
        return HandleSaveVersion(args);
    }

    // 供 ModpackService 复用：重新扫描 versions 目录并回写 config.json
    static nlohmann::json ScanInstalledVersions() {
        return HandleGetInstalledVersions();
    }

private:
    void RegisterCommands() {
        // version.list - 获取版本列表
        bridge_.RegisterCommand("version.list", [](const nlohmann::json&) -> nlohmann::json {
            return HandleVersionList();
        });

        // version.save - 保存已下载版本到 config.json
        bridge_.RegisterCommand("version.save", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleSaveVersion(args);
        });

        // version.get - 获取已下载版本列表
        bridge_.RegisterCommand("version.get", [](const nlohmann::json&) -> nlohmann::json {
            return HandleGetVersions();
        });

        // version.getInstalled - 扫描已安装版本并写入配置
        bridge_.RegisterCommand("version.getInstalled", [](const nlohmann::json&) -> nlohmann::json {
            return HandleGetInstalledVersions();
        });

        // version.delete - 删除版本
        bridge_.RegisterCommand("version.delete", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleVersionDelete(args);
        });

        // version.select - 选择游戏版本
        bridge_.RegisterCommand("version.select", [](const nlohmann::json& args) -> nlohmann::json {
            std::string profileName = args.value("profileName", "");

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                config = {{"accounts", nlohmann::json::array()}};
            }

            if (!config.contains("game") || !config["game"].is_object()) {
                config["game"] = {};
            }
            config["game"]["selectedVersion"] = profileName;

            bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", ok}};
        });

        // version.getSelected - 获取已选游戏版本
        bridge_.RegisterCommand("version.getSelected", [](const nlohmann::json&) -> nlohmann::json {
            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                return {{"ok", true}, {"profileName", ""}};
            }

            std::string profileName = "";
            if (config.contains("game") && config["game"].is_object()) {
                profileName = config["game"].value("selectedVersion", "");
            }
            return {{"ok", true}, {"profileName", profileName}};
        });

        // version.togglePinned - 固定 / 取消固定到主页
        // 主页左下角的版本卡片只列固定的版本，列表存在 game.pinnedVersions 里。
        // 不放在 game.versions 的条目内：version.getInstalled 会按磁盘扫描结果
        // 整体重建 game.versions，写在里面的标记会被冲掉。
        bridge_.RegisterCommand("version.togglePinned", [](const nlohmann::json& args) -> nlohmann::json {
            std::string profileName = args.value("profileName", "");
            if (profileName.empty()) {
                return {{"ok", false}, {"error", "Missing profileName"}};
            }

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                config = nlohmann::json::object();
            }

            if (!config.contains("game") || !config["game"].is_object()) {
                config["game"] = nlohmann::json::object();
            }
            if (!config["game"].contains("pinnedVersions") ||
                !config["game"]["pinnedVersions"].is_array()) {
                config["game"]["pinnedVersions"] = nlohmann::json::array();
            }

            bool wasPinned = false;
            nlohmann::json newPinned = nlohmann::json::array();
            for (const auto& item : config["game"]["pinnedVersions"]) {
                if (item.is_string() && item.get<std::string>() == profileName) {
                    wasPinned = true;
                    continue;
                }
                newPinned.push_back(item);
            }

            // 传了 pinned 就按给定值写，没传就在当前状态上取反
            bool pinned = !wasPinned;
            if (args.contains("pinned") && args["pinned"].is_boolean()) {
                pinned = args["pinned"].get<bool>();
            }

            if (pinned && (int)newPinned.size() >= Core::ConfigManager::kMaxPinnedVersions) {
                return {{"ok", false},
                        {"error", std::string("最多只能固定 ") +
                                      std::to_string(Core::ConfigManager::kMaxPinnedVersions) +
                                      " 个版本，请先取消一个"}};
            }
            if (pinned) newPinned.push_back(profileName);

            config["game"]["pinnedVersions"] = newPinned;
            bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", ok}, {"pinned", pinned}};
        });

        // ignorelist.get - 获取忽略库列表
        bridge_.RegisterCommand("ignorelist.get", [](const nlohmann::json&) -> nlohmann::json {
            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(content); } catch (...) { config = {}; }
            nlohmann::json list = nlohmann::json::array();
            if (config.contains("ignoredLibraries") && config["ignoredLibraries"].is_array()) {
                list = config["ignoredLibraries"];
            }
            return {{"ok", true}, {"list", list}};
        });

        // ignorelist.add - 添加库到忽略列表
        bridge_.RegisterCommand("ignorelist.add", [](const nlohmann::json& args) -> nlohmann::json {
            std::string name = args.value("name", "");
            if (name.empty()) return {{"ok", false}, {"error", "Missing library name"}};

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(content); } catch (...) { config = {}; }
            if (!config.contains("ignoredLibraries") || !config["ignoredLibraries"].is_array()) {
                config["ignoredLibraries"] = nlohmann::json::array();
            }
            for (const auto& item : config["ignoredLibraries"]) {
                if (item.is_string() && item.get<std::string>() == name) {
                    return {{"ok", true}, {"message", "Already in ignore list"}};
                }
            }
            config["ignoredLibraries"].push_back(name);
            Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", true}};
        });

        // ignorelist.remove - 从忽略列表移除库
        bridge_.RegisterCommand("ignorelist.remove", [](const nlohmann::json& args) -> nlohmann::json {
            std::string name = args.value("name", "");
            if (name.empty()) return {{"ok", false}, {"error", "Missing library name"}};

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(content); } catch (...) { config = {}; }
            if (!config.contains("ignoredLibraries") || !config["ignoredLibraries"].is_array()) {
                return {{"ok", true}};
            }
            nlohmann::json newArr = nlohmann::json::array();
            for (const auto& item : config["ignoredLibraries"]) {
                if (!(item.is_string() && item.get<std::string>() == name)) {
                    newArr.push_back(item);
                }
            }
            config["ignoredLibraries"] = newArr;
            Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", true}};
        });
    }

    // 获取版本列表
    static nlohmann::json HandleVersionList() {
        std::string content = Core::ConfigManager::ReadLocalManifest();
        if (content.empty()) {
            return {{"ok", false}, {"error", "No local cache available"}};
        }

        nlohmann::json manifest;
        try {
            manifest = nlohmann::json::parse(content);
        } catch (...) {
            return {{"ok", false}, {"error", "Invalid manifest JSON"}};
        }

        if (!manifest.contains("versions") || !manifest["versions"].is_array()) {
            return {{"ok", false}, {"error", "No versions field"}};
        }

        nlohmann::json groups = nlohmann::json::object();
        for (const auto& v : manifest["versions"]) {
            std::string type = v.value("type", "unknown");
            nlohmann::json item;
            item["id"] = v.value("id", "");
            item["type"] = type;
            item["url"] = v.value("url", "");
            item["releaseTime"] = v.value("releaseTime", "");
            groups[type].push_back(item);
        }

        for (auto& [type, arr] : groups.items()) {
            std::sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                return a.value("releaseTime", "") > b.value("releaseTime", "");
            });
        }

        return {{"ok", true}, {"groups", groups}};
    }

    // 保存已下载版本到 config.json
    static nlohmann::json HandleSaveVersion(const nlohmann::json& args) {
        std::string profileName = args.value("profileName", "");
        std::string actualVersionId = args.value("actualVersionId", "");
        std::string mainClass = args.value("mainClass", "");

        if (profileName.empty() || actualVersionId.empty()) {
            return {{"ok", false}, {"error", "Missing profileName or actualVersionId"}};
        }

        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            config = {{"accounts", nlohmann::json::array()}};
        }

        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = {};
        }
        if (!config["game"].contains("versions") || !config["game"]["versions"].is_array()) {
            config["game"]["versions"] = nlohmann::json::array();
        }

        bool found = false;
        for (auto& v : config["game"]["versions"]) {
            if (v.value("profileName", "") == profileName) {
                v["actualVersionId"] = actualVersionId;
                v["mainClass"] = mainClass;
                found = true;
                break;
            }
        }

        if (!found) {
            config["game"]["versions"].push_back({
                {"profileName", profileName},
                {"actualVersionId", actualVersionId},
                {"mainClass", mainClass}
            });
        }

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    // 获取已下载版本列表
    static nlohmann::json HandleGetVersions() {
        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            return {{"ok", true}, {"versions", nlohmann::json::array()}};
        }

        if (!config.contains("game") || !config["game"].is_object() ||
            !config["game"].contains("versions") || !config["game"]["versions"].is_array()) {
            return {{"ok", true}, {"versions", nlohmann::json::array()}};
        }

        return {{"ok", true}, {"versions", config["game"]["versions"]}};
    }

    // 扫描已安装版本
    static nlohmann::json HandleGetInstalledVersions() {
        std::wstring versionsDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\versions";

        nlohmann::json installedVersions = nlohmann::json::array();

        WIN32_FIND_DATAW findData;
        std::wstring searchPath = versionsDir + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) {
            return {{"ok", true}, {"versions", nlohmann::json::array()}};
        }

        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring dirName = findData.cFileName;
            if (dirName == L"." || dirName == L"..") continue;

            // 版本目录里可能不止一个 json —— 整合包实例安装期间会同时存在
            // 原版 JSON（<mc版本>.json）和加载器 JSON（<实例名>.json）。
            // 优先取和目录同名的那个，和 GameLaunchService 启动时挑 JSON 的
            // 规则一致，否则版本列表显示的元数据会和实际启动的版本对不上
            std::vector<std::wstring> jsonCandidates = {
                versionsDir + L"\\" + dirName + L"\\" + dirName + L".json"
            };
            WIN32_FIND_DATAW jsonData;
            HANDLE hJson = FindFirstFileW((versionsDir + L"\\" + dirName + L"\\*.json").c_str(), &jsonData);
            if (hJson != INVALID_HANDLE_VALUE) {
                do {
                    jsonCandidates.push_back(versionsDir + L"\\" + dirName + L"\\" + jsonData.cFileName);
                } while (FindNextFileW(hJson, &jsonData));
                FindClose(hJson);
            }

            for (const std::wstring& jsonPath : jsonCandidates) {
                std::ifstream ifs(jsonPath);
                if (!ifs.is_open()) continue;
                std::stringstream ss;
                ss << ifs.rdbuf();

                try {
                    nlohmann::json vj = nlohmann::json::parse(ss.str());

                    nlohmann::json versionInfo;
                    versionInfo["profileName"] = Core::StringUtils::WideToUtf8(dirName);
                    versionInfo["actualVersionId"] = vj.value("id", "");
                    versionInfo["type"] = vj.value("type", "");
                    versionInfo["releaseTime"] = vj.value("releaseTime", "");
                    versionInfo["mainClass"] = vj.value("mainClass", "");

                    installedVersions.push_back(versionInfo);
                    break;
                } catch (...) {
                    continue;
                }
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);

        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            config = {{"accounts", nlohmann::json::array()}};
        }

        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = {};
        }

        nlohmann::json existingVersions = config["game"].value("versions", nlohmann::json::array());
        nlohmann::json mergedVersions = nlohmann::json::array();

        for (const auto& installed : installedVersions) {
            nlohmann::json entry = installed;

            for (const auto& existing : existingVersions) {
                if (existing.value("profileName", "") == entry["profileName"]) {
                    if (entry.value("mainClass", "").empty() && !existing.value("mainClass", "").empty()) {
                        entry["mainClass"] = existing["mainClass"];
                    }
                    break;
                }
            }

            mergedVersions.push_back(entry);
        }

        config["game"]["versions"] = mergedVersions;
        Core::ConfigManager::WriteConfigFile(config.dump(4));

        // 固定列表只是 profileName 的数组，getInstalled 不会重建它，原样带出去
        nlohmann::json pinned = nlohmann::json::array();
        if (config["game"].contains("pinnedVersions") && config["game"]["pinnedVersions"].is_array()) {
            pinned = config["game"]["pinnedVersions"];
        }

        return {{"ok", true}, {"versions", mergedVersions}, {"pinned", pinned}};
    }

    // 删除版本
    static nlohmann::json HandleVersionDelete(const nlohmann::json& args) {
        std::string profileName = args.value("profileName", "");
        if (profileName.empty()) {
            return {{"ok", false}, {"error", "Missing profileName"}};
        }

        std::wstring versionDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\versions\\" + Core::StringUtils::Utf8ToWide(profileName);
        DWORD attr = GetFileAttributesW(versionDir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!Core::FileManager::RemoveDirectoryRecursive(versionDir)) {
                return {{"ok", false}, {"error", "Failed to delete version folder"}};
            }
        }

        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            return {{"ok", true}};
        }

        if (config.contains("game") && config["game"].is_object() &&
            config["game"].contains("versions") && config["game"]["versions"].is_array()) {
            nlohmann::json newVersions = nlohmann::json::array();
            for (const auto& v : config["game"]["versions"]) {
                if (v.value("profileName", "") != profileName) {
                    newVersions.push_back(v);
                }
            }
            config["game"]["versions"] = newVersions;
        }

        if (config.contains("game") && config["game"].is_object()) {
            if (config["game"].value("selectedVersion", "") == profileName) {
                config["game"]["selectedVersion"] = "";
            }

            // 版本没了，它的固定项也要一起清掉，否则主页卡片会留下点不动的空条目
            if (config["game"].contains("pinnedVersions") && config["game"]["pinnedVersions"].is_array()) {
                nlohmann::json newPinned = nlohmann::json::array();
                for (const auto& item : config["game"]["pinnedVersions"]) {
                    if (item.is_string() && item.get<std::string>() != profileName) {
                        newPinned.push_back(item);
                    }
                }
                config["game"]["pinnedVersions"] = newPinned;
            }
        }

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
