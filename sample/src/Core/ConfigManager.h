#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include "StringUtils.h"
#include "FileManager.h"

namespace Core {

class ConfigManager {
public:
    // 获取 exe 同目录下的 config.json 完整路径
    static std::wstring GetConfigPath() {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        WCHAR* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        return std::wstring(exePath) + L"\\config.json";
    }

    // 读取 config.json
    static std::string ReadConfigFile() {
        std::wstring wpath = GetConfigPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return "{}";
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        std::ifstream ifs(path);
        if (!ifs.is_open()) return "{}";
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    // 写入 config.json
    static bool WriteConfigFile(const std::string& content) {
        std::wstring wpath = GetConfigPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return false;
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        std::ofstream ofs(path);
        if (!ofs.is_open()) return false;
        ofs << content;
        return true;
    }

    // 启动时如果 config.json 不存在，生成默认配置
    static void EnsureConfigExists() {
        std::wstring wpath = GetConfigPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return;
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        std::ifstream ifs(path);
        if (ifs.is_open()) return;
        ifs.close();

        nlohmann::json defaultConfig;
        defaultConfig["accounts"] = nlohmann::json::array();
        for (int i = 0; i < 3; i++) {
            defaultConfig["accounts"].push_back("");
        }
        defaultConfig["game"] = {
            {"versions", nlohmann::json::array()},
            {"folderPath", ""},
            {"javaPath", ""},
            {"selectedVersion", ""}
        };
        defaultConfig["ignoredLibraries"] = nlohmann::json::array();
        defaultConfig["ignoredLibraries"].push_back("net.java.jinput:jinput-platform:2.0.5");
        defaultConfig["downloadThreads"] = 64;
        defaultConfig["downloadSettings"] = {
            {"source", "mirror_first"},
            {"versionListSource", "mirror_first"},
            {"maxThreads", 64},
            {"speedLimit", -1}
        };
        WriteConfigFile(defaultConfig.dump(4));
    }

    // 启动时创建 .minecraft 目录框架
    static void EnsureMinecraftDirs() {
        std::wstring base = FileManager::GetExeDir() + L"\\.minecraft";
        std::wstring dirs[] = {
            base,
            base + L"\\assets",
            base + L"\\assets\\indexes",
            base + L"\\assets\\objects",
            base + L"\\libraries",
            base + L"\\versions"
        };
        for (const auto& dir : dirs) {
            CreateDirectoryW(dir.c_str(), nullptr);
        }
    }

    // 检查某个卡片是否有登录信息
    static bool HasAccount(const nlohmann::json& config, int index) {
        if (!config.contains("accounts") || !config["accounts"].is_array()) return false;
        if (index < 0 || index >= (int)config["accounts"].size()) return false;
        const auto& acc = config["accounts"][index];
        return acc.is_string() && !acc.get<std::string>().empty();
    }

    // 检查是否有任意卡片有登录信息
    static bool AnyAccountExists(const nlohmann::json& config) {
        for (int i = 0; i < 3; i++) {
            if (HasAccount(config, i)) return true;
        }
        return false;
    }

    // 获取版本清单路径
    static std::wstring GetManifestPath() {
        return FileManager::GetExeDir() + L"\\.minecraft\\launcher_manifest.json";
    }

    // 读取本地缓存的版本清单
    static std::string ReadLocalManifest() {
        std::wstring wpath = GetManifestPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return "";
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        std::ifstream ifs(path);
        if (!ifs.is_open()) return "";
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    // 写入本地缓存
    static bool WriteLocalManifest(const std::string& content) {
        std::wstring wpath = GetManifestPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return false;
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        std::ofstream ofs(path, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs << content;
        return true;
    }
};

} // namespace Core
