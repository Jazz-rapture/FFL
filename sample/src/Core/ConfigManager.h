#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <mutex>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include "StringUtils.h"
#include "FileManager.h"

namespace Core {

class ConfigManager {
public:
    // 账号槽位数量：并行数组（accounts / uuids / accountTypes / accountTokens）都按它对齐
    static constexpr int kAccountSlots = 3;

    // 主页左下角版本卡片最多能固定几个版本（前端也有一份同样的上限，用来提前禁用图钉）
    static constexpr int kMaxPinnedVersions = 8;

    // 长任务现在会丢到独立线程执行（java.scan / game.launch / modrinth.*），
    // 命令之间不再天然串行。config.json 是全服务共用的文件，而写入是
    // "截断再写"，读的那一刻正好有人写到一半就会解析出半截 JSON。
    // 读写各自加锁，保证不会读到写了一半的内容。
    static std::mutex& ConfigMutex() {
        static std::mutex mutex;
        return mutex;
    }

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

        std::lock_guard<std::mutex> lock(ConfigMutex());
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

        std::lock_guard<std::mutex> lock(ConfigMutex());
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
        defaultConfig["uuids"] = nlohmann::json::array();
        defaultConfig["accountTypes"] = nlohmann::json::array();
        defaultConfig["accountTokens"] = nlohmann::json::array();
        for (int i = 0; i < kAccountSlots; i++) {
            defaultConfig["accounts"].push_back("");
            defaultConfig["uuids"].push_back("");
            defaultConfig["accountTypes"].push_back("");
            defaultConfig["accountTokens"].push_back("");
        }
        defaultConfig["game"] = {
            {"versions", nlohmann::json::array()},
            {"folderPath", ""},
            {"javaPath", ""},
            {"javaAuto", true},
            {"selectedVersion", ""},
            // 用哪个账号槽位启动游戏；-1 表示回去找第一个有账号的槽位
            {"selectedAccount", -1},
            // maxMemoryMb = 0 表示按可用物理内存自动分配
            {"maxMemoryMb", 0},
            // windowMode: default（854x480）/ fullscreen / custom
            {"windowMode", "default"},
            {"windowWidth", 854},
            {"windowHeight", 480},
            // versionIsolation: off（所有版本共用 .minecraft）/ modded / all
            {"versionIsolation", "off"},
            // 首次启动写入 options.txt 的游戏语言，用于跳过无障碍引导界面
            {"language", "zh_cn"},
            // 固定到主页左下角版本卡片的版本名（profileName），未固定的版本不出现在主页
            {"pinnedVersions", nlohmann::json::array()}
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
        for (int i = 0; i < kAccountSlots; i++) {
            if (HasAccount(config, i)) return true;
        }
        return false;
    }

    // ---- 账号槽位 ----
    // 三个槽位用并行数组存：accounts（显示名）/ uuids / accountTypes / accountTokens。
    // accountTypes 取 "offline"（离线登录，UUID 由名字算出来）或 "msa"（正版登录）。
    // accountTokens 存的是 DPAPI 加密后的会话（见 CryptoUtils），
    // 明文令牌绝对不落盘——读到 config.json 就等于拿到账号。
    struct AccountSlot {
        int index = -1;
        bool used = false;
        std::string name;      // 显示名：离线是输入的 ID，正版是角色名
        std::string uuid;
        std::string type;      // offline / msa
        std::string token;     // DPAPI 密文，仅 msa 有
    };

    // 补齐所有并行数组到 kAccountSlots 长度
    static void EnsureAccountSlots(nlohmann::json& config) {
        const char* keys[] = {"accounts", "uuids", "accountTypes", "accountTokens"};
        for (const char* key : keys) {
            if (!config.contains(key) || !config[key].is_array()) {
                config[key] = nlohmann::json::array();
            }
            while ((int)config[key].size() < kAccountSlots) {
                config[key].push_back("");
            }
        }
    }

    static AccountSlot ReadAccountSlot(const nlohmann::json& config, int index) {
        AccountSlot slot;
        slot.index = index;
        if (index < 0 || index >= kAccountSlots) return slot;
        if (!config.contains("accounts") || !config["accounts"].is_array() ||
            index >= (int)config["accounts"].size()) {
            return slot;
        }

        const auto& name = config["accounts"][index];
        if (!name.is_string() || name.get<std::string>().empty()) return slot;

        slot.used = true;
        slot.name = name.get<std::string>();
        slot.uuid = ReadSlotString(config, "uuids", index);
        slot.type = ReadSlotString(config, "accountTypes", index);
        slot.token = ReadSlotString(config, "accountTokens", index);
        if (slot.type.empty()) slot.type = "offline";
        return slot;
    }

    // name 为空表示清空该槽位（删除角色 / 管理账户都靠它）
    static void WriteAccountSlot(nlohmann::json& config, int index, const std::string& name,
                                 const std::string& uuid, const std::string& type,
                                 const std::string& token) {
        EnsureAccountSlots(config);
        if (index < 0 || index >= kAccountSlots) return;

        config["accounts"][index] = name;
        config["uuids"][index] = name.empty() ? "" : uuid;
        config["accountTypes"][index] = name.empty() ? "" : type;
        config["accountTokens"][index] = name.empty() ? "" : token;
    }

    static void SetSelectedAccountIndex(nlohmann::json& config, int index) {
        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = nlohmann::json::object();
        }
        config["game"]["selectedAccount"] = index;
    }

    // 启动游戏时用哪个槽位：优先用户选中的，选中项失效时退回到第一个可用槽位
    static int ResolveActiveAccountIndex(const nlohmann::json& config) {
        int selected = -1;
        if (config.contains("game") && config["game"].is_object()) {
            selected = config["game"].value("selectedAccount", -1);
        }
        if (selected >= 0 && HasAccount(config, selected)) return selected;
        for (int i = 0; i < kAccountSlots; i++) {
            if (HasAccount(config, i)) return i;
        }
        return -1;
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

private:
    // 老配置里可能还没有这些数组，缺键 / 越界一律当作空串
    static std::string ReadSlotString(const nlohmann::json& config, const std::string& key, int index) {
        if (!config.contains(key) || !config[key].is_array()) return "";
        if (index < 0 || index >= (int)config[key].size()) return "";
        const auto& value = config[key][index];
        return value.is_string() ? value.get<std::string>() : "";
    }
};

} // namespace Core
