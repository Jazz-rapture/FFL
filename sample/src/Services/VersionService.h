#pragma once
#include <string>
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

            std::wstring jsonSearch = versionsDir + L"\\" + dirName + L"\\*.json";
            WIN32_FIND_DATAW jsonData;
            HANDLE hJson = FindFirstFileW(jsonSearch.c_str(), &jsonData);
            if (hJson == INVALID_HANDLE_VALUE) continue;

            do {
                std::wstring jsonName = jsonData.cFileName;
                std::wstring jsonPath = versionsDir + L"\\" + dirName + L"\\" + jsonName;

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
            } while (FindNextFileW(hJson, &jsonData));

            FindClose(hJson);
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

        return {{"ok", true}, {"versions", mergedVersions}};
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
        }

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
