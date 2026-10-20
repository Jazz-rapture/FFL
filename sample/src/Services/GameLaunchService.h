#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <regex>
#include <Windows.h>
#include <shlobj.h>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/HttpClient.h"
#include "Core/FileManager.h"
#include "Core/HashUtils.h"
#include "Core/StringUtils.h"
#include "Core/DownloadSourceManager.h"
#include "Bridge/WebviewBridge.h"
#include "Services/MavenUtils.h"
#include "Services/MsaAuthService.h"
#include "Services/YggdrasilAuthService.h"
#include "Core/AuthlibInjector.h"
#include "Core/ZipExtractor.h"

namespace Services {

class GameLaunchService {
public:
    GameLaunchService(Bridge::WebviewBridge& bridge) : bridge_(bridge), g_app_ptr(nullptr) {
        RegisterCommands();
    }

    void SetApp(tauricpp::App* app) { g_app_ptr = app; }

    // 当前选中版本的游戏目录，以及 mod / 资源包 / 光影包 各自的落点。
    // 前端的「另存为」用它当默认目录，语义与 PCL 的 PathIndie 一致：
    // 开了版本隔离就落在 versions 下的版本文件夹，否则是 .minecraft 本身
    static nlohmann::json GetResourcePaths() {
        nlohmann::json config = ReadConfig();

        GameSettings settings = ReadGameSettings(config);
        std::string profileName;
        bool isModded = false;
        std::wstring gameDir;
        ResolveSelectedVersion(config, settings, profileName, isModded, gameDir);

        // 加载器版本的 id 是 profileName（可能是 "1.20.1-forge-47.2.0" 甚至用户改名后的
        // 任意字符串），真正的游戏版本在 inheritsFrom 里。前端拿它标记「所选版本」
        nlohmann::json vj = ReadVersionJson(profileName);
        std::string gameVersion = profileName;
        if (vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string()) {
            gameVersion = vj["inheritsFrom"].get<std::string>();
        } else if (vj.contains("id") && vj["id"].is_string()) {
            gameVersion = vj["id"].get<std::string>();
        }

        std::string base = Core::StringUtils::WideToUtf8(gameDir);
        if (!base.empty() && base.back() != '\\') base += "\\";

        return {
            {"ok", true},
            {"instanceName", profileName},
            {"gameVersion", gameVersion},
            {"loaders", DetectLoaders(profileName)},
            {"gameDir", base},
            {"paths", {
                {"mod", base + "mods"},
                {"resourcepack", base + "resourcepacks"},
                {"shader", base + "shaderpacks"}
            }}
        };
    }

    // 这个 mod 文件和当前实例兼不兼容：游戏版本必须对得上；两边都认得加载器时还要对得上
    // （实例是 Fabric 就装不了 Forge 的 mod）
    static bool IsFileCompatibleWithInstance(const std::string& projectType,
                                             const std::vector<std::string>& fileGameVersions,
                                             const std::vector<std::string>& fileLoaders,
                                             const std::string& instanceVersion,
                                             const std::vector<std::string>& instanceLoaders) {
        if (instanceVersion.empty() || fileGameVersions.empty()) return false;

        bool versionMatch = false;
        for (const auto& gv : fileGameVersions) {
            if (gv == instanceVersion) { versionMatch = true; break; }
        }
        if (!versionMatch) return false;

        if (projectType != "mod") return true;
        if (instanceLoaders.empty()) return true;

        bool hasRealLoader = false;
        for (const auto& fl : fileLoaders) {
            // minecraft 是每个文件都带的占位加载器，不算真实加载器
            if (fl == "minecraft") continue;
            hasRealLoader = true;
            for (const auto& il : instanceLoaders) {
                if (fl == il) return true;
            }
        }
        // 文件没声明任何真实加载器（只有 minecraft 占位）时无法判断，按通过处理
        return !hasRealLoader;
    }

private:
    tauricpp::App* g_app_ptr;
    Bridge::WebviewBridge& bridge_;

    void RegisterCommands() {
        // game.launch - 启动游戏（异步）
        bridge_.RegisterCommand("game.launch", [this](const nlohmann::json& args) -> nlohmann::json {
            // 在后台线程执行启动逻辑，避免阻塞主线程
            std::thread([this, args]() {
                nlohmann::json result = HandleGameLaunch(args);
                // 通知前端启动结果
                nlohmann::json event = {{"phase", "launch"}, {"result", result}};
                if (g_app_ptr) {
                    try {
                        g_app_ptr->GetBridge().Emit("game.launch.result", event);
                    } catch (...) {}
                }
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // config.getGameSettings - 读取游戏内存 / 窗口 / 版本隔离
        bridge_.RegisterCommand("config.getGameSettings", [](const nlohmann::json&) -> nlohmann::json {
            return HandleGetGameSettings();
        });

        // config.setGameSettings - 写入游戏内存 / 窗口 / 版本隔离
        bridge_.RegisterCommand("config.setGameSettings", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleSetGameSettings(args);
        });

        // game.getResourcePaths - 当前版本的游戏目录与各类资源落点（另存为默认目录）
        bridge_.RegisterCommand("game.getResourcePaths", [](const nlohmann::json&) -> nlohmann::json {
            return GetResourcePaths();
        });

        // game.listMods - 某个版本下某个目录里的文件与启用状态。
        // category 取 mods / resourcepacks / shaderpacks，缺省按 mods
        bridge_.RegisterCommand("game.listMods", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleListMods(args);
        });

        // game.toggleMod - 启用/停用某个文件（改扩展名），category 同上
        bridge_.RegisterCommand("game.toggleMod", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleToggleMod(args);
        });
    }

    // 发送启动进度事件
    void EmitLaunchProgress(const std::string& status, int pid = 0, int exitCode = -1) {
        nlohmann::json event = {{"phase", "launch"}, {"status", status}};
        if (pid > 0) event["pid"] = pid;
        if (exitCode >= 0) event["exitCode"] = exitCode;
        if (g_app_ptr) {
            try {
                g_app_ptr->GetBridge().Emit("game.launch.progress", event);
            } catch (...) {}
        }
    }

    // 启动过程中的提醒（前端收到后弹右上角消息）
    void EmitLaunchNotice(const std::string& title, const std::string& content,
                          const std::string& type) {
        if (!g_app_ptr) return;
        try {
            g_app_ptr->GetBridge().Emit("game.launch.notice",
                                        {{"title", title}, {"content", content}, {"type", type}});
        } catch (...) {}
    }

    // ==================== 缺失文件自动补齐 ====================

    // 一个待补文件：落盘位置 + 候选下载地址 + 期望的 SHA-1
    struct RepairItem {
        std::wstring path;
        std::vector<std::string> urls;
        std::string sha1;
        std::string name;    // 日志和提示里显示的名字
        // 资源对象（贴图 / 音效）缺了不致命：游戏照样起得来，只是贴图变默认。
        // 库、natives、核心 jar 缺了根本启动不了
        bool fatal = true;
    };

    // 补齐进度：前端拿它更新按钮上的"补齐文件 3/37"
    void EmitRepairProgress(int done, int total) {
        if (!g_app_ptr) return;
        try {
            g_app_ptr->GetBridge().Emit("game.launch.progress",
                                        {{"phase", "launch"}, {"status", "repairing"},
                                         {"done", done}, {"total", total}});
        } catch (...) {}
    }

    // 并发补齐：每个文件按候选地址逐个试，落盘前校验 SHA-1。
    // 返回补成功的数量，失败项追加进 failures
    int RepairMissingFiles(std::vector<RepairItem> items, std::vector<RepairItem>& failures) {
        const int total = static_cast<int>(items.size());
        if (total == 0) return 0;

        int threadCount = 8;
        try {
            nlohmann::json config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
            if (config.contains("downloadThreads") && config["downloadThreads"].is_number_integer()) {
                threadCount = config["downloadThreads"].get<int>();
            }
        } catch (...) {}
        threadCount = (std::max)(1, (std::min)(threadCount, 16));
        threadCount = (std::min)(threadCount, total);

        std::atomic<int> next{0};
        std::atomic<int> done{0};
        std::atomic<int> repaired{0};
        std::mutex resultMtx;

        auto worker = [&]() {
            for (;;) {
                const int index = next.fetch_add(1);
                if (index >= total) return;
                RepairItem& item = items[static_cast<size_t>(index)];

                // 目录可能整个还不存在（比如 assets\objects\ab）
                const size_t slash = item.path.find_last_of(L'\\');
                if (slash != std::wstring::npos) {
                    Core::FileManager::CreateDirRecursive(item.path.substr(0, slash));
                }

                bool ok = false;
                for (const auto& url : item.urls) {
                    if (url.empty()) continue;
                    Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
                    if (res.status != 200 || res.body.empty()) continue;
                    // 有 SHA-1 就必须对得上：宁可报缺失，也不要落一个坏文件进 libraries
                    if (!item.sha1.empty() &&
                        Core::HashUtils::ComputeSHA1(res.body) != item.sha1) {
                        continue;
                    }
                    if (!Core::FileManager::SaveFile(item.path, res.body)) continue;
                    ok = true;
                    break;
                }

                if (ok) {
                    repaired.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> lock(resultMtx);
                    failures.push_back(item);
                }

                const int finished = done.fetch_add(1) + 1;
                // 进度节流：资源对象动辄上千个，逐个发事件会把 JS 队列刷爆。
                // 最多发 50 次，最后一个一定发（保证能看到走完）
                const int step = (std::max)(1, total / 50);
                if (finished == total || finished % step == 0) {
                    EmitRepairProgress(finished, total);
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(threadCount));
        for (int i = 0; i < threadCount; i++) threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        return repaired.load();
    }

    // 扫描一遍缺失文件，把每一项的落盘路径和候选下载地址一起收出来。
    // 地址先用版本 JSON 里给的，再补上镜像规则推出的做兜底
    void ScanMissingFiles(const nlohmann::json& mergedLibs, const nlohmann::json& config,
                          const std::wstring& libsBase, const std::wstring& versionJar,
                          const std::wstring& indexesDir, const std::wstring& objectsDir,
                          const nlohmann::json& vj, const nlohmann::json& parentVj,
                          const nlohmann::json& assetIndexObj,
                          std::vector<RepairItem>& out, std::vector<std::string>& names) {
        auto addUrl = [](RepairItem& item, const std::string& url) {
            if (url.empty()) return;
            for (const auto& existing : item.urls) {
                if (existing == url) return;
            }
            item.urls.push_back(url);
        };

        // ---- 依赖库 ----
        for (const auto& lib : mergedLibs) {
            const std::string name = lib.value("name", "");
            if (!MavenUtils::CheckRulesMatch(lib)) continue;
            if (MavenUtils::IsLibraryIgnored(name, config)) continue;

            Models::MavenCoord coord = MavenUtils::ParseMavenName(name);
            if (coord.groupId.empty() || coord.artifactId.empty() || coord.version.empty()) continue;

            const bool isNativeJar = !coord.classifier.empty() &&
                                     (coord.classifier.find("natives") != std::string::npos);

            std::string mavenPath = MavenUtils::BuildMavenPath(coord);
            std::string downloadUrl;
            std::string sha1;

            if (lib.contains("downloads") && lib["downloads"].is_object()) {
                if (isNativeJar && lib["downloads"].contains("classifiers") &&
                    lib["downloads"]["classifiers"].is_object() &&
                    lib["downloads"]["classifiers"].contains(coord.classifier)) {
                    const auto& info = lib["downloads"]["classifiers"][coord.classifier];
                    downloadUrl = info.value("url", "");
                    sha1 = info.value("sha1", "");
                    if (info.contains("path")) mavenPath = info["path"].get<std::string>();
                } else if (!isNativeJar && lib["downloads"].contains("artifact") &&
                           lib["downloads"]["artifact"].is_object()) {
                    const auto& artifact = lib["downloads"]["artifact"];
                    downloadUrl = artifact.value("url", "");
                    sha1 = artifact.value("sha1", "");
                    if (artifact.contains("path")) mavenPath = artifact["path"].get<std::string>();
                }
            }

            const bool isForgeDep = (mavenPath.find("net/minecraftforge/") != std::string::npos) ||
                                    (downloadUrl.find("maven.minecraftforge.net") != std::string::npos);

            std::string localPath = mavenPath;
            std::replace(localPath.begin(), localPath.end(), '/', '\\');
            const std::wstring jarPath = libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localPath);

            // 存在但 SHA-1 对不上也算缺：补齐流程会把坏文件覆盖掉
            bool missing = (GetFileAttributesW(jarPath.c_str()) == INVALID_FILE_ATTRIBUTES);
            if (!missing && !sha1.empty() &&
                Core::HashUtils::ComputeFileSHA1(jarPath) != sha1) {
                WriteLaunchLog("损坏: " + name + "（SHA-1 不符，重新补齐）");
                missing = true;
            }

            if (missing) {
                RepairItem item;
                item.path = jarPath;
                item.sha1 = sha1;
                item.name = name;
                item.fatal = true;
                addUrl(item, downloadUrl);
                if (downloadUrl.empty() && lib.contains("url") && lib["url"].is_string()) {
                    std::string baseUrl = lib["url"].get<std::string>();
                    if (!baseUrl.empty() && baseUrl.back() != '/') baseUrl += '/';
                    addUrl(item, baseUrl + mavenPath);
                }
                // 镜像兜底：官方 maven + BMCLAPI / qlu
                const auto mirrors = isForgeDep
                    ? Core::DownloadSourceManager::GetForgeDownloadUrls(mavenPath)
                    : Core::DownloadSourceManager::GetLibraryMirrorUrls(mavenPath);
                for (const auto& url : mirrors) addUrl(item, url);

                names.push_back(name);
                out.push_back(std::move(item));
            }

            // ---- natives 分类器 jar ----
            if (isNativeJar) continue;
            if (!lib.contains("natives") || !lib["natives"].is_object()) continue;
            if (!lib.contains("downloads") || !lib["downloads"].is_object()) continue;
            if (!lib["downloads"].contains("classifiers") || !lib["downloads"]["classifiers"].is_object()) continue;

            std::string classifierKey;
#if defined(_WIN32)
            if (lib["natives"].contains("windows")) classifierKey = lib["natives"]["windows"].get<std::string>();
#elif defined(__APPLE__)
            if (lib["natives"].contains("osx")) classifierKey = lib["natives"]["osx"].get<std::string>();
#else
            if (lib["natives"].contains("linux")) classifierKey = lib["natives"]["linux"].get<std::string>();
#endif
            if (classifierKey.empty() || !lib["downloads"]["classifiers"].contains(classifierKey)) continue;

            const auto& ci = lib["downloads"]["classifiers"][classifierKey];
            const std::string nativeUrl = ci.value("url", "");
            const std::string nativeSha1 = ci.value("sha1", "");
            std::string nativePath = ci.value("path", "");
            if (nativePath.empty()) {
                nativePath = MavenUtils::BuildMavenPath(coord);
                const size_t lastSlash = nativePath.find_last_of('/');
                if (lastSlash == std::string::npos) continue;
                std::string classifier = classifierKey;
                const size_t archPos = classifier.find("${arch}");
                if (archPos != std::string::npos) classifier.replace(archPos, 7, "64");
                nativePath = nativePath.substr(0, lastSlash + 1) + coord.artifactId + "-" +
                             coord.version + "-" + classifier + ".jar";
            }

            std::string localNativePath = nativePath;
            std::replace(localNativePath.begin(), localNativePath.end(), '/', '\\');
            const std::wstring nativeJarPath =
                libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localNativePath);

            bool nativeMissing = (GetFileAttributesW(nativeJarPath.c_str()) == INVALID_FILE_ATTRIBUTES);
            if (!nativeMissing && !nativeSha1.empty() &&
                Core::HashUtils::ComputeFileSHA1(nativeJarPath) != nativeSha1) {
                nativeMissing = true;
            }
            if (!nativeMissing) continue;

            RepairItem nativeItem;
            nativeItem.path = nativeJarPath;
            nativeItem.sha1 = nativeSha1;
            nativeItem.name = name + " (natives)";
            nativeItem.fatal = true;
            addUrl(nativeItem, nativeUrl);
            for (const auto& url : Core::DownloadSourceManager::GetLibraryMirrorUrls(nativePath)) {
                addUrl(nativeItem, url);
            }
            names.push_back(nativeItem.name);
            out.push_back(std::move(nativeItem));
        }

        // ---- 游戏核心 jar ----
        if (GetFileAttributesW(versionJar.c_str()) == INVALID_FILE_ATTRIBUTES) {
            RepairItem item;
            item.path = versionJar;
            item.name = Core::StringUtils::WideToUtf8(versionJar);
            item.fatal = true;
            // 加载器版本自己没有 downloads.client，取继承的原版那份
            nlohmann::json client;
            if (vj.contains("downloads") && vj["downloads"].is_object() &&
                vj["downloads"].contains("client")) {
                client = vj["downloads"]["client"];
            } else if (parentVj.contains("downloads") && parentVj["downloads"].is_object() &&
                       parentVj["downloads"].contains("client")) {
                client = parentVj["downloads"]["client"];
            }
            if (client.is_object()) {
                addUrl(item, client.value("url", ""));
                item.sha1 = client.value("sha1", "");
            }
            names.push_back(item.name);
            out.push_back(std::move(item));
        }

        // ---- 资源索引与资源对象 ----
        const std::string assetIndexId = assetIndexObj.value("id", "");
        if (assetIndexId.empty()) return;

        const std::wstring indexPath =
            indexesDir + L"\\" + Core::StringUtils::Utf8ToWide(assetIndexId) + L".json";
        if (GetFileAttributesW(indexPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            RepairItem item;
            item.path = indexPath;
            item.name = "assets/indexes/" + assetIndexId + ".json";
            item.fatal = true;
            addUrl(item, assetIndexObj.value("url", ""));
            item.sha1 = assetIndexObj.value("sha1", "");
            names.push_back(item.name);
            out.push_back(std::move(item));
            // 索引不在就枚举不了对象，这一轮到此为止，下一轮再查
            return;
        }

        std::ifstream indexFile(indexPath);
        if (!indexFile.is_open()) return;
        nlohmann::json assetIndex;
        try {
            indexFile >> assetIndex;
        } catch (...) {
            WriteLaunchLog("警告: 资源索引解析失败，跳过资源对象检查");
            return;
        }
        if (!assetIndex.contains("objects") || !assetIndex["objects"].is_object()) return;

        int missingObjects = 0;
        for (const auto& entry : assetIndex["objects"].items()) {
            if (!entry.value().is_object() || !entry.value().contains("hash")) continue;
            const std::string hash = entry.value()["hash"].get<std::string>();
            if (hash.size() < 2) continue;

            const std::string prefix = hash.substr(0, 2);
            const std::wstring objPath = objectsDir + L"\\" + Core::StringUtils::Utf8ToWide(prefix) +
                                         L"\\" + Core::StringUtils::Utf8ToWide(hash);
            if (GetFileAttributesW(objPath.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

            missingObjects++;
            RepairItem item;
            item.path = objPath;
            item.sha1 = hash;
            item.name = "assets/objects/" + prefix + "/" + hash;
            // 资源对象缺了游戏照样能起（贴图退回默认），所以不是致命项
            item.fatal = false;
            for (const auto& url : Core::DownloadSourceManager::GetAssetsDownloadUrls(prefix, hash)) {
                addUrl(item, url);
            }
            // 名字只留前 20 条：动辄上千个，逐条塞进日志和提示会把两边都撑爆
            if (missingObjects <= 20) names.push_back(item.name);
            out.push_back(std::move(item));
        }
        WriteLaunchLog("资源对象缺失: " + std::to_string(missingObjects) + " 个");
    }

    // 校验 + 自动补齐。返回 true 表示可以继续启动。
    // 补不上时：库 / natives / 核心 jar 缺失拒绝启动，只有资源对象缺失才放行
    bool EnsureGameFiles(const nlohmann::json& mergedLibs, const nlohmann::json& config,
                         const std::wstring& minecraftDir, const std::wstring& versionJar,
                         const nlohmann::json& vj, const nlohmann::json& parentVj,
                         const nlohmann::json& assetIndexObj,
                         std::string& error, nlohmann::json& missingOut) {
        const std::wstring libsBase = minecraftDir + L"\\libraries";
        const std::wstring indexesDir = minecraftDir + L"\\assets\\indexes";
        const std::wstring objectsDir = minecraftDir + L"\\assets\\objects";

        std::vector<RepairItem> missing;
        std::vector<std::string> missingNames;
        bool announced = false;

        // 最多补两轮。第一轮很可能只是把资源索引补上 —— 索引不在就枚举不了
        // 资源对象，那几千个对象要到第二轮扫描才会暴露出来，所以不能只补一轮。
        // 每轮都是"先扫、缺了就补"，最后一轮只扫不补，拿到的就是最终的缺失清单
        for (int attempt = 0; attempt <= 2; attempt++) {
            missing.clear();
            missingNames.clear();
            ScanMissingFiles(mergedLibs, config, libsBase, versionJar, indexesDir, objectsDir,
                             vj, parentVj, assetIndexObj, missing, missingNames);

            if (missing.empty()) {
                WriteLaunchLog("--- 文件完整性检查通过 ---");
                return true;
            }
            if (attempt == 2) break;   // 补过两轮还缺，不再试，直接判失败

            WriteLaunchLog("--- 缺失 " + std::to_string(missing.size()) + " 个文件（第 " +
                           std::to_string(attempt + 1) + " 轮补齐） ---");
            for (size_t i = 0; i < missingNames.size() && i < 40; i++) {
                WriteLaunchLog("  - " + missingNames[i]);
            }
            // 只提示一次：别让用户看到两条一样的通知
            if (!announced) {
                announced = true;
                EmitLaunchNotice("正在补齐缺失文件",
                                 "检测到缺失文件，正在自动下载，补完会自动继续启动",
                                 "info");
            }

            std::vector<RepairItem> failures;
            const int repaired = RepairMissingFiles(missing, failures);
            WriteLaunchLog("补齐结果: 成功 " + std::to_string(repaired) + " / " +
                           std::to_string(missing.size()));
            for (size_t i = 0; i < failures.size() && i < 20; i++) {
                WriteLaunchLog("  补齐失败: " + failures[i].name);
            }
        }

        WriteLaunchLog("--- 仍有 " + std::to_string(missing.size()) + " 个文件补齐失败 ---");

        missingOut = nlohmann::json::array();
        for (const auto& name : missingNames) missingOut.push_back(name);

        bool hasFatal = false;
        for (const auto& item : missing) {
            if (item.fatal) {
                hasFatal = true;
                break;
            }
        }

        if (!hasFatal) {
            // 只缺资源对象：贴图 / 音效会退回默认，但游戏能玩，没必要拦着
            EmitLaunchNotice("部分资源文件缺失",
                             "有 " + std::to_string(missing.size()) +
                             " 个资源文件没能补齐，游戏会以默认贴图启动",
                             "warning");
            return true;
        }

        error = "缺少 " + std::to_string(missing.size()) + " 个文件且自动补齐失败：";
        for (size_t i = 0; i < missingNames.size() && i < 3; i++) {
            if (i > 0) error += "、";
            error += missingNames[i];
        }
        if (missingNames.size() > 3) error += " 等";
        error += "。请检查网络后重试，详情见 .minecraft\\logs\\launch_*.log";
        return false;
    }

    // 1.19 起客户端才启用聊天签名（enforce-secure-profile / 档案公钥都是那一版加的）。
    // 只认 "1.x" 形式的正式版本号：快照（如 24w14a）解析不出来就当不需要提示，
    // 宁可漏提示，也不要对老版本乱提示
    static bool IsChatSigningVersion(const std::string& versionId) {
        if (versionId.size() < 4 || versionId[0] != '1' || versionId[1] != '.') return false;

        size_t i = 2;
        if (i >= versionId.size() || !isdigit((unsigned char)versionId[i])) return false;
        int minor = 0;
        while (i < versionId.size() && isdigit((unsigned char)versionId[i])) {
            minor = minor * 10 + (versionId[i] - '0');
            i++;
        }
        return minor >= 19;
    }

    // 启动命令里带着访问令牌，写日志前先抹掉：
    // 日志是明文文件，令牌留在里面等于把账号摊在磁盘上
    static std::string RedactCommandSecrets(const std::string& cmd) {
        const std::string key = "--accessToken";
        std::string out = cmd;
        size_t pos = 0;
        while ((pos = out.find(key, pos)) != std::string::npos) {
            size_t cursor = pos + key.size();
            while (cursor < out.size() && out[cursor] == ' ') cursor++;
            size_t end = out.find(' ', cursor);
            if (end == std::string::npos) end = out.size();
            if (end > cursor) {
                out.replace(cursor, end - cursor, "***");
                pos = cursor + 3;
            } else {
                pos = cursor;
            }
        }
        return out;
    }

    // 运行 `java -version` 取主版本号（失败返回 0）
    static int ProbeJavaMajorVersion(const std::string& javaPath) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        HANDLE hRead = nullptr;
        HANDLE hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return 0;

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;   // java -version 的输出走 stderr
        si.hStdInput = nullptr;

        std::wstring cmd = L"\"" + Core::StringUtils::Utf8ToWide(javaPath) + L"\" -version";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(hWrite);
        if (!started) {
            CloseHandle(hRead);
            return 0;
        }

        std::string output;
        char buf[1024];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            output += buf;
        }
        CloseHandle(hRead);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // java version "1.8.0_461" / java version "21.0.6" / openjdk version "17.0.9"
        size_t pos = output.find("version \"");
        if (pos == std::string::npos) return 0;
        pos += 9;
        size_t end = output.find('"', pos);
        if (end == std::string::npos) return 0;
        std::string version = output.substr(pos, end - pos);

        std::istringstream ss(version);
        std::string token;
        int major = 0;
        if (!std::getline(ss, token, '.')) return 0;
        try {
            major = std::stoi(token);
        } catch (...) {
            return 0;
        }
        if (major == 1 && std::getline(ss, token, '.')) {
            try {
                major = std::stoi(token);   // 1.8.0_461 -> 8
            } catch (...) {
                return 0;
            }
        }
        return major;
    }

    // 带缓存地获取 Java 主版本号
    static int GetJavaMajorVersion(const std::string& javaPath) {
        static std::mutex cacheMtx;
        static std::map<std::string, int> cache;
        {
            std::lock_guard<std::mutex> lock(cacheMtx);
            auto it = cache.find(javaPath);
            if (it != cache.end()) return it->second;
        }
        int major = ProbeJavaMajorVersion(javaPath);
        {
            std::lock_guard<std::mutex> lock(cacheMtx);
            cache[javaPath] = major;
        }
        return major;
    }

    // 定位游戏核心 jar（必须加入 classpath）。
    // 命名约定与 client.download / version.download 保持一致（都按 actualVersionId 落盘）：
    //   原版版本：      versions/<profileName>/<actualVersionId>.jar
    //   模组加载器版本：游戏核心就是继承的原版客户端 jar
    //                   versions/<profileName>/<inheritsFrom>.jar
    // 例：profileName = "1.12.2-forge-14.23.5.2864" 时，原版 jar 叫 1.12.2.jar。
    // 注意 Forge ≤1.12.2 是 LaunchWrapper 方案，必须在 classpath 上看到原版（混淆）jar，
    // 否则 FML 反混淆 patcher 拿不到类数据，会误报 "your vanilla jar may be corrupt"。
    // 都找不到时退化为扫描版本目录（与版本 JSON 的查找方式一致），返回按约定推导的路径。
    static std::wstring ResolveGameJarPath(const std::wstring& minecraftDir, const std::wstring& versionDir,
                                           const std::string& profileName, const std::string& actualVersionId,
                                           const nlohmann::json& vj) {
        std::string jarName = profileName;
        if (vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string()) {
            jarName = vj["inheritsFrom"].get<std::string>();
        } else if (!actualVersionId.empty()) {
            jarName = actualVersionId;
        }

        std::wstring direct = versionDir + L"\\" + Core::StringUtils::Utf8ToWide(jarName) + L".jar";
        if (Core::FileManager::FileExists(direct)) return direct;

        WIN32_FIND_DATAW fd;
        // 兜底 1：同版本目录下的任意 jar（版本目录通常只放一个游戏核心 jar）
        HANDLE hFind = FindFirstFileW((versionDir + L"\\*.jar").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            std::wstring found = versionDir + L"\\" + fd.cFileName;
            FindClose(hFind);
            return found;
        }

        // 兜底 2：扫描其它版本目录（加载器版本可能是给另一处安装的原版建的）
        std::wstring versionsDir = minecraftDir + L"\\versions";
        hFind = FindFirstFileW((versionsDir + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::wstring dir = fd.cFileName;
                if (dir == L"." || dir == L"..") continue;
                std::wstring candidate = versionsDir + L"\\" + dir + L"\\" +
                                         Core::StringUtils::Utf8ToWide(jarName) + L".jar";
                if (Core::FileManager::FileExists(candidate)) {
                    FindClose(hFind);
                    return candidate;
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        return direct;
    }

    // 判断 arguments 条目是否适用于当前平台。
    // 比 MavenUtils::CheckRulesMatch 更完整：额外处理 os.version、os.arch 与 features。
    // 这一步很关键——原版 JSON 里有 os.name=osx 的 -XstartOnFirstThread，
    // 在 Windows 上带上它会让 JVM 直接拒绝启动。
    //
    // features 按官方语义逐个键比对：启动器上报的值决定了哪些可选参数生效。
    // 我们只支持 has_custom_resolution（有目标窗口尺寸时为真），
    // is_demo_user 与 has_quick_plays_support / is_quick_play_* 一律为假
    // ——试玩和 Quick Play 都不是启动器自动开启的功能。
    static bool CheckArgRules(const nlohmann::json& entry, bool customResolution) {
        if (!entry.contains("rules") || !entry["rules"].is_array() || entry["rules"].empty()) {
            return true;
        }

        std::string currentOs;
        std::string currentArch;
#if defined(_WIN32)
        currentOs = "windows";
#elif defined(__APPLE__)
        currentOs = "osx";
#else
        currentOs = "linux";
#endif
#if defined(_M_X64) || defined(__x86_64__)
        currentArch = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
        currentArch = "arm64";
#elif defined(_M_IX86) || defined(__i386__)
        currentArch = "x86";
#else
        currentArch = "unknown";
#endif

        bool allowed = false;
        for (const auto& rule : entry["rules"]) {
            std::string action = rule.value("action", "allow");

            if (rule.contains("os") && rule["os"].is_object()) {
                std::string ruleOs = rule["os"].value("name", "");
                if (!ruleOs.empty() && ruleOs != currentOs) continue;
                std::string ruleArch = rule["os"].value("arch", "");
                if (!ruleArch.empty() && ruleArch != currentArch) continue;
                // os.version 是正则（原版用它把 -Dos.name=Windows 10 限定在 Win10+）
                std::string ruleVersion = rule["os"].value("version", "");
                if (!ruleVersion.empty() && !OsVersionMatches(ruleVersion)) continue;
            }
            if (rule.contains("features") && rule["features"].is_object()) {
                if (!FeaturesMatch(rule["features"], customResolution)) continue;
            }

            if (action == "allow") {
                allowed = true;
            } else if (action == "disallow") {
                allowed = false;
            }
        }
        return allowed;
    }

    // features 里的每个键都必须与启动器上报的值一致，规则才算命中；
    // 不认识的键按"未启用"处理，避免把来路不明的可选参数加进去
    static bool FeaturesMatch(const nlohmann::json& features, bool customResolution) {
        for (auto it = features.begin(); it != features.end(); ++it) {
            bool required = it.value().is_boolean() ? it.value().get<bool>() : true;
            if (it.key() == "has_custom_resolution") {
                if (required != customResolution) return false;
            } else if (required) {
                return false;
            }
        }
        return true;
    }

    static bool OsVersionMatches(const std::string& pattern) {
        std::string version = RuntimeOsVersion();
        try {
            return std::regex_search(version, std::regex(pattern));
        } catch (...) {
            return false;
        }
    }

    // 形如 "10.0.26100"；用于匹配 JSON 里的 os.version 规则
    static std::string RuntimeOsVersion() {
        static std::string cached = [] {
            OSVERSIONINFOEXW info = { sizeof(OSVERSIONINFOEXW) };
            typedef LONG(WINAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                auto rtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
                if (rtlGetVersion && rtlGetVersion((PRTL_OSVERSIONINFOW)&info) == 0) {
                    return std::to_string(info.dwMajorVersion) + "." +
                           std::to_string(info.dwMinorVersion) + "." +
                           std::to_string(info.dwBuildNumber);
                }
            }
            return std::string("0.0.0");
        }();
        return cached;
    }

    // ---- 启动设置（内存 / 窗口 / 版本隔离 / 游戏语言）----
    struct GameSettings {
        int maxMemoryMb;                // 0 = 按可用物理内存自动分配
        std::string windowMode;         // default / fullscreen / custom
        int windowWidth;
        int windowHeight;
        std::string versionIsolation;   // off / modded / all
        std::string language;           // 写入 options.txt 的游戏语言，如 zh_cn
    };

    // 语言代码会被原样拼进 options.txt，只接受 Minecraft 用的 `<lang>_<region>` 小写形式；
    // 空串 / 换行 / 路径之类的非法值一律退回 zh_cn，避免把 options.txt 写坏
    static std::string SanitizeLanguage(const std::string& language) {
        if (language.size() < 3 || language.size() > 16) return "zh_cn";
        if (language.find('_') == std::string::npos) return "zh_cn";
        for (char c : language) {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') continue;
            return "zh_cn";
        }
        return language;
    }

    // 读 config.json，坏文件一律当空对象，调用方不用再各自 try/catch
    static nlohmann::json ReadConfig() {
        try {
            return nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    static GameSettings ReadGameSettings(const nlohmann::json& config) {
        GameSettings settings{0, "default", 854, 480, "off", "zh_cn"};
        if (!config.contains("game") || !config["game"].is_object()) return settings;

        const nlohmann::json& game = config["game"];
        settings.maxMemoryMb = game.value("maxMemoryMb", 0);
        settings.windowMode = game.value("windowMode", std::string("default"));
        settings.windowWidth = game.value("windowWidth", 854);
        settings.windowHeight = game.value("windowHeight", 480);
        settings.versionIsolation = game.value("versionIsolation", std::string("off"));
        settings.language = game.value("language", std::string("zh_cn"));
        settings.language = SanitizeLanguage(settings.language);

        if (settings.maxMemoryMb < 0) settings.maxMemoryMb = 0;
        if (settings.windowWidth < 100) settings.windowWidth = 854;
        if (settings.windowHeight < 100) settings.windowHeight = 480;
        if (settings.windowMode != "fullscreen" && settings.windowMode != "custom") {
            settings.windowMode = "default";
        }
        if (settings.versionIsolation != "modded" && settings.versionIsolation != "all") {
            settings.versionIsolation = "off";
        }
        return settings;
    }

    // 只有开了隔离，该版本的存档 / 配置 / 模组才落到自己的版本文件夹里；
    // 关掉时所有版本共用 .minecraft（改动前的行为）
    static bool ShouldIsolateVersion(const std::string& mode, bool isModdedVersion) {
        if (mode == "all") return true;
        if (mode == "modded") return isModdedVersion;
        return false;
    }

    // 这个实例有没有真正玩过：saves 目录里有存档才算。
    // 游戏只要启动过一次就会建出空的 saves，所以空目录和不存在一样按“没玩过”处理。
    static bool HasSaveContent(const std::wstring& gameDir) {
        std::wstring savesDir = gameDir + L"\\saves";
        DWORD attr = GetFileAttributesW(savesDir.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return false;

        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((savesDir + L"\\*").c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return false;

        bool hasContent = false;
        do {
            std::wstring name = findData.cFileName;
            if (name != L"." && name != L"..") {
                hasContent = true;
                break;
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
        return hasContent;
    }

    // 首次启动的无障碍引导界面（"Welcome to Minecraft" 旁白欢迎页）只认 options.txt 里的
    // lang / onboardAccessibility 两个键，启动参数改不了它：lang 缺失时默认 en_us，
    // onboardAccessibility 缺失时默认 true（= 弹出引导页）。
    //
    // “首次启动”判定沿用 PCL（#3844）的思路，并放宽到两条件任一成立即可：
    //   · options.txt 里没有 lang 行；或
    //   · 这个实例还没产生过存档（整合包往往自带了 options.txt，那不是玩家选的语言）
    // 判定成立时按启动器设置写入 lang 并补上 onboardAccessibility:false；不成立说明玩家
    // 已经在游戏里自己选过语言，整份文件一个字都不动。
    // 改写时逐行原地替换，其它键与原顺序保持不变，也不新增 version 行（那是游戏自己的
    // 数据版本号，写错会让它走错迁移分支）。
    static void EnsureGameOptionsFile(const std::string& gameDirUtf8, const std::string& language) {
        std::wstring gameDir = Core::StringUtils::Utf8ToWide(gameDirUtf8);
        std::wstring path = gameDir + L"\\options.txt";

        std::string existing;
        if (Core::FileManager::FileExists(path)) {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return;
            std::stringstream buffer;
            buffer << in.rdbuf();
            in.close();
            existing = buffer.str();
        }

        // 按 \n 切行，\r 留在行尾，回写时才能原样保住 CRLF
        std::vector<std::string> lines;
        {
            std::stringstream in(existing);
            std::string line;
            while (std::getline(in, line)) lines.push_back(line);
        }

        auto findKey = [&lines](const std::string& key) -> size_t {
            for (size_t i = 0; i < lines.size(); i++) {
                std::string line = lines[i];
                if (!line.empty() && line.back() == '\r') line.pop_back();
                // 必须是 "key:" 整键匹配，避免把 langXXX 当成 lang
                if (line.size() > key.size() && line.compare(0, key.size(), key) == 0 &&
                    line[key.size()] == ':') {
                    return i;
                }
            }
            return lines.size();
        };

        const bool firstLaunch = findKey("lang") == lines.size() || !HasSaveContent(gameDir);
        if (!firstLaunch) return;

        auto upsert = [&lines, &findKey](const std::string& key, const std::string& value) {
            size_t index = findKey(key);
            if (index == lines.size()) {
                lines.push_back(key + ":" + value);
                return;
            }
            std::string eol = (!lines[index].empty() && lines[index].back() == '\r') ? "\r" : "";
            lines[index] = key + ":" + value + eol;
        };

        upsert("lang", language);
        upsert("onboardAccessibility", "false");

        std::string content;
        for (const std::string& line : lines) content += line + "\n";

        if (Core::FileManager::SaveFile(path, content)) {
            WriteLaunchLog("已写入 options.txt: lang=" + language + ", onboardAccessibility=false (跳过无障碍引导界面)");
        } else {
            WriteLaunchLog("警告: options.txt 写入失败: " + Core::StringUtils::WideToUtf8(path));
        }
    }

    static int CountModFiles(const std::wstring& gameDir) {
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((gameDir + L"\\mods\\*").c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return 0;

        int count = 0;
        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

            std::wstring name = findData.cFileName;
            for (wchar_t& c : name) {
                if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
            }
            if (name.size() >= 4 && (name.compare(name.size() - 4, 4, L".jar") == 0 ||
                                     name.compare(name.size() - 4, 4, L".zip") == 0)) {
                count++;
            } else if (name.size() >= 8 && name.compare(name.size() - 8, 8, L".litemod") == 0) {
                count++;
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
        return count;
    }

    // 自动内存：沿用 PCL 的分阶段分配 —— 先无条件满足"勉强能跑"的档位，
    // 剩下的可用内存再按 70% / 40% / 15% 的递减比例投入更高的档位，
    // 这样内存越多给得越多，但永远不会把可用内存吃干。
    static int ComputeAutoMemoryMb(bool isModdedVersion, const std::wstring& gameDir) {
        MEMORYSTATUSEX status = { sizeof(MEMORYSTATUSEX) };
        if (!GlobalMemoryStatusEx(&status)) return 2048;

        double available = status.ullAvailPhys / 1024.0 / 1024.0 / 1024.0;

        double minimum, target1, target2, target3;
        if (isModdedVersion) {
            double mods = CountModFiles(gameDir);
            minimum = 0.5 + mods / 150.0;
            target1 = 1.5 + mods / 90.0;
            target2 = 2.7 + mods / 50.0;
            target3 = 4.5 + mods / 25.0;
        } else {
            minimum = 0.5;
            target1 = 1.5;
            target2 = 2.5;
            target3 = 4.0;
        }

        double give = 0;
        double delta = target1;
        give += (std::min)(available, delta);
        available -= delta;

        if (available >= 0.1) {
            delta = target2 - target1;
            give += (std::min)(available * 0.7, delta);
            available -= delta / 0.7;
        }
        if (available >= 0.1) {
            delta = target3 - target2;
            give += (std::min)(available * 0.4, delta);
            available -= delta / 0.4;
        }
        if (available >= 0.1) {
            give += (std::min)(available * 0.15, target3);
        }

        give = std::round((std::max)(give, minimum) * 10) / 10;
        int mb = (int)std::llround(give * 1024);
        return mb < 512 ? 512 : mb;
    }

    static int TotalPhysicalMemoryMb() {
        MEMORYSTATUSEX status = { sizeof(MEMORYSTATUSEX) };
        if (!GlobalMemoryStatusEx(&status)) return 0;
        return (int)(status.ullTotalPhys / 1024 / 1024);
    }

    // 版本 JSON 里的文件路径（版本隔离与加载器识别都要读它）
    static std::wstring GetVersionJsonPath(const std::string& profileName) {
        std::wstring wide = Core::StringUtils::Utf8ToWide(profileName);
        return Core::FileManager::GetExeDir() + L"\\.minecraft\\versions\\" + wide + L"\\" + wide + L".json";
    }

    static nlohmann::json ReadVersionJson(const std::string& profileName) {
        if (profileName.empty()) return nlohmann::json::object();
        std::ifstream versionFile(GetVersionJsonPath(profileName));
        if (!versionFile.is_open()) return nlohmann::json::object();
        std::stringstream ss;
        ss << versionFile.rdbuf();
        try {
            return nlohmann::json::parse(ss.str());
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    // 整合包安装出来的实例会被静态合并成单文件版本 JSON（对齐 PCL 的 MergeJson），
    // 里面没有 inheritsFrom。安装器会在合并结果里打上这个键，说明「这份 JSON 是
    // 原版 + 加载器合成出来的」，否则加载器分支（classpath 去重、加载器专属
    // JVM 参数、自动内存）会因为找不到 inheritsFrom 而整段跳过，Forge / Fabric
    // 直接起不来。手工安装的加载器版本仍然带 inheritsFrom，两种都要认。
    static constexpr const char* kDerivedKey = "__tauricppDerived";

    // 带 inheritsFrom 或带合并标记的版本 JSON，就是模组加载器版本
    // （Forge / NeoForge / Fabric / Quilt）
    static bool IsDerivedProfileJson(const nlohmann::json& versionJson) {
        if (versionJson.contains("inheritsFrom") && versionJson["inheritsFrom"].is_string()) return true;
        return versionJson.contains(kDerivedKey) && versionJson[kDerivedKey].is_boolean() &&
               versionJson[kDerivedKey].get<bool>();
    }

    static bool IsModdedProfile(const std::string& profileName) {
        return IsDerivedProfileJson(ReadVersionJson(profileName));
    }

    // 整合包实例强制版本隔离的标记键。
    // 全局的 game.versionIsolation 是给用户自己调的开关，整合包不能依赖它：
    // 一个整合包实例必须独占自己的 mods / saves / 配置，否则装完就会被别的
    // 版本污染（反过来它也会污染别人）。所以由安装方在版本 JSON 里打上这个键，
    // 隔离判定时与全局开关取"或"。
    // 版本 JSON 是启动器侧的元数据，游戏只读自己 jar 里的那份，多一个键它看不见。
    static constexpr const char* kForceIsolationKey = "__tauricppForceIsolation";

    static bool IsForceIsolated(const nlohmann::json& versionJson) {
        return versionJson.contains(kForceIsolationKey) &&
               versionJson[kForceIsolationKey].is_boolean() &&
               versionJson[kForceIsolationKey].get<bool>();
    }

    // 从版本 JSON 的 libraries 里认出加载器，用于判断 mod 和这个实例兼不兼容。
    // 加载器版本自己只声明增量，真正的 loader 库就在它自己的 JSON 里
    static nlohmann::json DetectLoaders(const std::string& profileName) {
        nlohmann::json result = nlohmann::json::array();
        nlohmann::json vj = ReadVersionJson(profileName);
        if (!vj.contains("libraries") || !vj["libraries"].is_array()) return result;

        struct { const char* prefix; const char* name; } known[] = {
            {"net.fabricmc:fabric-loader", "fabric"},
            {"net.minecraftforge:forge", "forge"},
            {"net.neoforged:neoforge", "neoforge"},
            {"org.quiltmc:quilt-loader", "quilt"},
        };
        for (const auto& lib : vj["libraries"]) {
            std::string name = lib.value("name", "");
            for (const auto& k : known) {
                if (name.rfind(k.prefix, 0) == 0) {
                    bool exists = false;
                    for (const auto& added : result) {
                        if (added == k.name) { exists = true; break; }
                    }
                    if (!exists) result.push_back(k.name);
                }
            }
        }
        return result;
    }

    // 任意版本应用版本隔离后的游戏目录（不含结尾反斜杠）
    static std::wstring ResolveGameDirForVersion(const std::string& profileName, const GameSettings& settings) {
        std::wstring minecraftDir = Core::FileManager::GetExeDir() + L"\\.minecraft";
        if (profileName.empty()) return minecraftDir;

        std::wstring versionDir = minecraftDir + L"\\versions\\" + Core::StringUtils::Utf8ToWide(profileName);
        nlohmann::json vj = ReadVersionJson(profileName);
        const bool modded = IsDerivedProfileJson(vj);
        return (ShouldIsolateVersion(settings.versionIsolation, modded) || IsForceIsolated(vj))
                   ? versionDir : minecraftDir;
    }

    // 当前选中版本是不是模组版本、它的游戏目录在哪 —— 自动内存和隔离都要按这个算
    static void ResolveSelectedVersion(const nlohmann::json& config, const GameSettings& settings,
                                       std::string& profileName, bool& isModded, std::wstring& gameDir) {
        profileName = "";
        if (config.contains("game") && config["game"].is_object()) {
            profileName = config["game"].value("selectedVersion", "");
        }

        isModded = IsModdedProfile(profileName);
        gameDir = ResolveGameDirForVersion(profileName, settings);
    }

    // ==================== 版本文件管理（模组 / 资源包 / 光影包） ====================

    // 前端三个分页各自对应的目录名。未知值一律当 mods —— 这个参数来自前端，
    // 拼进路径之前必须先归一化，不能直接拿它当目录名用
    static std::wstring ContentFolder(const std::string& category) {
        if (category == "resourcepacks") return L"resourcepacks";
        if (category == "shaderpacks") return L"shaderpacks";
        return L"mods";
    }

    static bool EndsWithStr(const std::string& lower, const std::string& suffix) {
        return lower.size() > suffix.size() &&
               lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // 每个目录里算「可管理文件」的扩展名。资源包 / 光影包只认 .zip，
    // 模组除了 .jar 还有整合包偶尔塞的 .zip 和 LiteLoader 的 .litemod
    static bool IsContentFile(const std::string& lowerBase, const std::string& category) {
        if (category == "resourcepacks" || category == "shaderpacks") {
            return EndsWithStr(lowerBase, ".zip");
        }
        return EndsWithStr(lowerBase, ".jar") || EndsWithStr(lowerBase, ".zip") ||
               EndsWithStr(lowerBase, ".litemod");
    }

    // 列出某个版本某个目录里的文件与启用状态。
    // 停用的文件是把扩展名换成 .disabled（加载器只认原扩展名，改掉就不会被读）
    static nlohmann::json HandleListMods(const nlohmann::json& args) {
        std::string profileName = args.value("version", "");
        if (profileName.empty()) return {{"ok", false}, {"error", "Missing version"}};
        const std::string category = args.value("category", "mods");

        GameSettings settings = ReadGameSettings(ReadConfig());
        std::wstring gameDir = ResolveGameDirForVersion(profileName, settings);
        std::wstring dir = gameDir + L"\\" + ContentFolder(category);

        nlohmann::json files = nlohmann::json::array();
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string name = Core::StringUtils::WideToUtf8(findData.cFileName);

                std::string lower = name;
                for (auto& c : lower) c = (char)tolower(c);
                // 先把停用后缀剥掉再判扩展名，"xxx.jar.disabled" 也算可管理文件
                const bool disabled = EndsWithStr(lower, ".disabled");
                const std::string base = disabled ? lower.substr(0, lower.size() - 9) : lower;
                if (!IsContentFile(base, category)) continue;

                long long size = ((long long)findData.nFileSizeHigh << 32) | findData.nFileSizeLow;
                std::string enabledName = disabled ? name.substr(0, name.size() - 9) : name;
                files.push_back({
                    {"name", name},
                    {"enabledName", enabledName},
                    {"enabled", !disabled},
                    {"size", size}
                });
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }

        return {
            {"ok", true},
            {"files", files},
            {"dir", Core::StringUtils::WideToUtf8(dir)},
            // 没开隔离时多个版本共用同一个 .minecraft\xxx，界面要能说清楚这一点
            {"shared", gameDir == (Core::FileManager::GetExeDir() + L"\\.minecraft")}
        };
    }

    // 切换启用状态：启用 → 去掉 .disabled，停用 → 加上 .disabled
    static nlohmann::json HandleToggleMod(const nlohmann::json& args) {
        std::string profileName = args.value("version", "");
        std::string name = args.value("name", "");
        bool enable = args.value("enable", true);
        if (profileName.empty() || name.empty()) return {{"ok", false}, {"error", "Missing version or name"}};
        const std::string category = args.value("category", "mods");

        GameSettings settings = ReadGameSettings(ReadConfig());
        std::wstring dir = ResolveGameDirForVersion(profileName, settings) + L"\\" +
                           ContentFolder(category);
        std::wstring from = dir + L"\\" + Core::StringUtils::Utf8ToWide(name);

        const std::wstring suffix = L".disabled";
        bool currentlyDisabled = from.size() > suffix.size() &&
            from.compare(from.size() - suffix.size(), suffix.size(), suffix) == 0;

        std::wstring to;
        if (enable) {
            if (!currentlyDisabled) return {{"ok", true}, {"name", name}, {"enabled", true}};  // 已经是启用态
            to = from.substr(0, from.size() - suffix.size());
        } else {
            if (currentlyDisabled) return {{"ok", true}, {"name", name}, {"enabled", false}};
            to = from + suffix;
        }

        if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return {{"ok", false}, {"error", "文件不存在，可能已被移动或删除"}};
        }
        // 目标已存在就先删掉，否则 MoveFileW 会失败（例如 .disabled 和 .jar 同时存在）
        if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(to.c_str());
        }
        if (!MoveFileW(from.c_str(), to.c_str())) {
            return {{"ok", false}, {"error", "文件被占用，改名失败"}};
        }

        std::string newName = Core::StringUtils::WideToUtf8(to.substr(to.find_last_of(L'\\') + 1));
        return {{"ok", true}, {"name", newName}, {"enabled", enable}};
    }

    // 前端 bridge: 读取游戏设置（内存 / 窗口 / 版本隔离），附带算好的自动内存供界面显示
    static nlohmann::json HandleGetGameSettings() {
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            config = nlohmann::json::object();
        }

        GameSettings settings = ReadGameSettings(config);
        std::string profileName;
        bool isModded = false;
        std::wstring gameDir;
        ResolveSelectedVersion(config, settings, profileName, isModded, gameDir);

        return {
            {"ok", true},
            {"maxMemoryMb", settings.maxMemoryMb},
            {"autoMemoryMb", ComputeAutoMemoryMb(isModded, gameDir)},
            {"totalMemoryMb", TotalPhysicalMemoryMb()},
            {"windowMode", settings.windowMode},
            {"windowWidth", settings.windowWidth},
            {"windowHeight", settings.windowHeight},
            {"versionIsolation", settings.versionIsolation},
            {"language", settings.language},
            {"selectedVersion", profileName},
            {"moddedVersion", isModded}
        };
    }

    static nlohmann::json HandleSetGameSettings(const nlohmann::json& args) {
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            config = nlohmann::json::object();
        }
        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = nlohmann::json::object();
        }

        // 先读回当前值再逐项覆盖：这样越界输入会被 ReadGameSettings 夹回合法范围
        GameSettings settings = ReadGameSettings(config);
        if (args.contains("maxMemoryMb")) settings.maxMemoryMb = args["maxMemoryMb"].get<int>();
        if (args.contains("windowMode")) settings.windowMode = args["windowMode"].get<std::string>();
        if (args.contains("windowWidth")) settings.windowWidth = args["windowWidth"].get<int>();
        if (args.contains("windowHeight")) settings.windowHeight = args["windowHeight"].get<int>();
        if (args.contains("versionIsolation")) settings.versionIsolation = args["versionIsolation"].get<std::string>();
        if (args.contains("language")) settings.language = SanitizeLanguage(args["language"].get<std::string>());

        if (settings.maxMemoryMb != 0) {
            if (settings.maxMemoryMb < 512) settings.maxMemoryMb = 512;
            int total = TotalPhysicalMemoryMb();
            if (total > 0 && settings.maxMemoryMb > total) settings.maxMemoryMb = total - 512;
            if (settings.maxMemoryMb < 512) settings.maxMemoryMb = 512;
        }

        config["game"]["maxMemoryMb"] = settings.maxMemoryMb;
        config["game"]["windowMode"] = settings.windowMode;
        config["game"]["windowWidth"] = settings.windowWidth;
        config["game"]["windowHeight"] = settings.windowHeight;
        config["game"]["versionIsolation"] = settings.versionIsolation;
        config["game"]["language"] = settings.language;

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    // 版本 JSON 里 ${...} 占位符的取值
    struct ArgValues {
        std::string username;
        std::string uuid;
        std::string accessToken;   // 正版是 Minecraft 访问令牌，离线是 "0"
        std::string userType;      // msa（正版）/ mojang（离线）
        std::string xuid;          // 只有正版账号有
        std::string gameDir;
        std::string assetsDir;
        std::string assetIndex;
        std::string versionName;
        std::string versionType;
        std::string resolutionWidth;
        std::string resolutionHeight;
        std::string primaryJar;
        std::string gameAssetsDir;
        std::string librariesDir;
    };

    // 把 arguments.game / minecraftArguments 里的占位符替换成实际值。
    // 替换完值为空的参数（离线账号的 clientId / xuid）连同它的 flag 一起丢掉：
    // 空值会被 JVM 当成"下一个参数"吃掉，宁可少传一个可选参数，也不能让后面的参数整体错位。
    static void SubstituteGameArgs(std::vector<std::string>& args, const ArgValues& values) {
        auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
            if (from.empty()) return;
            size_t pos = 0;
            while ((pos = str.find(from, pos)) != std::string::npos) {
                str.replace(pos, from.length(), to);
                pos += to.length();
            }
        };

        for (std::string& arg : args) {
            replaceAll(arg, "${auth_player_name}", values.username);
            replaceAll(arg, "${auth_uuid}", values.uuid);
            replaceAll(arg, "${auth_access_token}", values.accessToken);
            replaceAll(arg, "${auth_session}", values.accessToken);
            replaceAll(arg, "${game_directory}", values.gameDir);
            replaceAll(arg, "${assets_root}", values.assetsDir);
            replaceAll(arg, "${assets_index_name}", values.assetIndex);
            replaceAll(arg, "${version_name}", values.versionName);
            replaceAll(arg, "${user_properties}", "{}");
            replaceAll(arg, "${user_type}", values.userType);
            replaceAll(arg, "${version_type}", values.versionType);
            replaceAll(arg, "${resolution_width}", values.resolutionWidth);
            replaceAll(arg, "${resolution_height}", values.resolutionHeight);
            replaceAll(arg, "${primary_jar}", values.primaryJar);
            replaceAll(arg, "${game_assets}", values.gameAssetsDir);
            replaceAll(arg, "${library_directory}", values.librariesDir);
            replaceAll(arg, "${libraries_directory}", values.librariesDir);
            replaceAll(arg, "${clientid}", "");
            replaceAll(arg, "${auth_xuid}", values.xuid);
            replaceAll(arg, "${launcher_name}", "TauriCPP");
            replaceAll(arg, "${launcher_version}", "1.0");

            // 剩下没人认领的占位符整段抹掉
            size_t pos = 0;
            while ((pos = arg.find("${", pos)) != std::string::npos) {
                size_t end = arg.find("}", pos);
                if (end == std::string::npos) break;
                arg.erase(pos, end - pos + 1);
            }

            // 替换后可能只剩空白（取值被整个抹掉），trim 掉免得被当成带空格的参数
            while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) arg.erase(0, 1);
            while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();
        }

        std::vector<std::string> cleaned;
        for (size_t i = 0; i < args.size(); i++) {
            if (args[i].empty()) continue;
            if (i + 1 < args.size() && args[i + 1].empty() && args[i][0] == '-') {
                i++;
                continue;
            }
            cleaned.push_back(args[i]);
        }
        args = cleaned;
    }

    // argfile 里带着 --accessToken，正版账号放的是真实令牌，
    // JVM 读完参数就不需要它了。先覆写再删除，别让它一直躺在磁盘上——
    // 令牌只加密存在 config.json 里，这里又被明文写回去就前功尽弃了。
    static void RemoveLaunchArgfile(const std::wstring& argfilePath) {
        if (argfilePath.empty()) return;

        HANDLE hFile = CreateFileW(argfilePath.c_str(), GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(hFile, &size) && size.QuadPart > 0) {
                std::vector<char> filler((size_t)size.QuadPart, ' ');
                DWORD written = 0;
                WriteFile(hFile, filler.data(), (DWORD)filler.size(), &written, nullptr);
                FlushFileBuffers(hFile);
            }
            CloseHandle(hFile);
        }
        DeleteFileW(argfilePath.c_str());
    }

    // 日志写入
    static void WriteLaunchLog(const std::string& message) {        std::wstring logDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\logs";
        CreateDirectoryW(logDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char logName[64];
        snprintf(logName, sizeof(logName), "launch_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);
        std::wstring logPath = logDir + L"\\" + Core::StringUtils::Utf8ToWide(std::string(logName));

        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

        std::string logLine = timestamp + message + "\n";

        HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(hFile, logLine.c_str(), (DWORD)logLine.size(), &written, nullptr);
            CloseHandle(hFile);
        }
    }

    // 启动游戏
    nlohmann::json HandleGameLaunch(const nlohmann::json& args) {
        std::string profileName = args.value("profileName", "");
        WriteLaunchLog("========== 游戏启动开始 ==========");
        WriteLaunchLog("版本: " + profileName);

        if (profileName.empty()) {
            WriteLaunchLog("错误: 未指定版本");
            return {{"ok", false}, {"error", "Missing profileName"}};
        }

        // 读取配置
        std::string configContent = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(configContent);
        } catch (...) {
            WriteLaunchLog("错误: 配置文件解析失败");
            return {{"ok", false}, {"error", "Invalid config"}};
        }

        // 获取 Java 路径
        std::string javaPath = "";
        if (config.contains("game") && config["game"].is_object()) {
            javaPath = config["game"].value("javaPath", "");
        }
        WriteLaunchLog("Java 路径: " + javaPath);
        if (javaPath.empty()) {
            return {{"ok", false}, {"error", "No Java selected"}};
        }

        DWORD javaAttr = GetFileAttributesW(Core::StringUtils::Utf8ToWide(javaPath).c_str());
        if (javaAttr == INVALID_FILE_ATTRIBUTES) {
            return {{"ok", false}, {"error", "Java file not found: " + javaPath}};
        }

        // 探测 Java 主版本：不同版本对 JVM 参数的兼容性差异很大。
        // 例如 --enable-native-access（JDK 16 引入）对 Java 8/11 是未知选项，
        // 加上它会让 JVM 直接报 "Unrecognized option" 拒绝启动（1.12.2 等版本用的正是 Java 8）。
        int javaMajor = GetJavaMajorVersion(javaPath);
        if (javaMajor > 0) {
            WriteLaunchLog("Java 主版本: " + std::to_string(javaMajor));
        } else {
            WriteLaunchLog("警告: 无法探测 Java 版本, 将使用最保守的启动参数");
        }
        // 阈值取 17（而非引入该选项的 16）以留出余量：漏掉该参数最多多一条警告，
        // 错加参数却会让 JVM 完全无法启动。探测失败时同样按不支持处理。
        const bool supportsNativeAccess = javaMajor >= 17;

        // 获取账号信息。
        // 正版账号必须带上真实的 UUID 与访问令牌，且令牌过期时先刷新——
        // 拿离线 UUID 或 "0" 令牌进服会被服务端判定为无效会话。
        const int accountIndex = Core::ConfigManager::ResolveActiveAccountIndex(config);
        if (accountIndex < 0) {
            WriteLaunchLog("错误: 没有已登录的账号");
            return {{"ok", false}, {"error", "No account logged in"}};
        }

        // 会话处理按账号类型分流：正版刷微软令牌，外置登录校验/刷新 Yggdrasil 令牌
        const Core::ConfigManager::AccountSlot activeSlot =
            Core::ConfigManager::ReadAccountSlot(config, accountIndex);
        const bool isYggdrasilAccount = activeSlot.type == "yggdrasil";

        nlohmann::json account = isYggdrasilAccount
            ? Services::YggdrasilAuthService::EnsureFreshSession(accountIndex)
            : Services::MsaAuthService::EnsureFreshSession(accountIndex);
        if (!account.value("ok", false)) {
            std::string reason = account.value("error", std::string("账号不可用"));
            WriteLaunchLog("错误: 账号不可用: " + reason);
            return {{"ok", false}, {"error", reason}};
        }

        const std::string username = account.value("name", std::string());
        const std::string uuid = account.value("uuid", std::string());
        const std::string accessToken = account.value("accessToken", std::string());
        const std::string xuid = account.value("xuid", std::string());
        const bool isMsaAccount = account.value("type", std::string("offline")) == "msa";
        // 外置登录由注入的 authlib-injector 代理认证，game 参数里的 userType 仍是 mojang，
        // 但 UUID 和令牌必须是第三方服务器给出的真实值
        const std::string yggdrasilApiRoot = account.value("apiRoot", std::string());
        // 离线账号沿用原来的 "mojang" + "0" 令牌，正版和外置登录才换成真实值
        const std::string userType = isMsaAccount ? "msa" : "mojang";
        WriteLaunchLog("账号: " + username + ", UUID: " + uuid + ", 登录方式: " +
                       (isMsaAccount ? "正版" : (isYggdrasilAccount ? "外置登录" : "离线")));

        // 读取版本 JSON
        std::wstring minecraftDir = Core::FileManager::GetExeDir() + L"\\.minecraft";
        std::wstring versionDir = minecraftDir + L"\\versions\\" + Core::StringUtils::Utf8ToWide(profileName);

        // 版本 JSON 路径：优先 <profileName>.json。
        // 加载器版本目录里同时存在原版 JSON（<inheritsFrom>.json）和加载器 JSON
        // （<profileName>.json），直接按 *.json 取第一个会依赖文件名的字典序
        // （"1.12.2-forge-x.json" < "1.12.2.json"），重命名为中文等名字后就会
        // 误取原版 JSON 从而启动成原版。因此先精确匹配，再退回到扫描。
        std::wstring versionJsonPath = versionDir + L"\\" +
                                      Core::StringUtils::Utf8ToWide(profileName) + L".json";
        if (GetFileAttributesW(versionJsonPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileW((versionDir + L"\\*.json").c_str(), &findData);
            if (hFind != INVALID_HANDLE_VALUE) {
                versionJsonPath = versionDir + L"\\" + findData.cFileName;
                FindClose(hFind);
            }
        }

        std::ifstream vjFile(versionJsonPath);
        if (!vjFile.is_open()) {
            return {{"ok", false}, {"error", "Version JSON not found"}};
        }
        std::stringstream vjSS;
        vjSS << vjFile.rdbuf();
        // 读完立刻关掉：ifstream 默认不带 FILE_SHARE_DELETE，句柄留到启动结束会让
        // 版本目录在整个启动期间（尤其长时间校验/下载时）一直删不掉
        vjFile.close();
        nlohmann::json vj;
        try {
            vj = nlohmann::json::parse(vjSS.str());
        } catch (...) {
            return {{"ok", false}, {"error", "Invalid version JSON"}};
        }

        std::string mainClass = vj.value("mainClass", "");
        if (mainClass.empty()) {
            return {{"ok", false}, {"error", "No mainClass"}};
        }
        WriteLaunchLog("主类: " + mainClass);

        std::string actualVersionId = vj.value("id", profileName);
        // 判断 LWJGL 版本必须用真实的游戏版本号：加载器版本的 id 是 profileName
        // （可能是 "1.20.1-forge-47.2.0" 甚至用户改名后的任意字符串），
        // 真正的游戏版本在 inheritsFrom 里。
        std::string gameVersionId = actualVersionId;
        if (vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string()) {
            gameVersionId = vj["inheritsFrom"].get<std::string>();
        }
        bool isLwjgl2 = MavenUtils::IsVersionBelow1_13(gameVersionId);
        if (isLwjgl2) {
            WriteLaunchLog("检测到 LWJGL 2.x 版本: " + gameVersionId);
        }

        // 外置登录 + 1.19 及以上：客户端会做聊天签名，用的是账号的档案公钥。
        // 第三方验证服务器多半没有实现 feature.enable_profile_key，账号拿不到公钥，
        // 于是进了 enforce-secure-profile=true 的服务器就发不出消息。这里提前讲清楚，
        // 免得用户以为是启动器坏了
        if (isYggdrasilAccount && IsChatSigningVersion(gameVersionId)) {
            EmitLaunchNotice(
                "聊天签名提示",
                "当前版本（" + gameVersionId + "）开启聊天签名，而外置登录的验证服务器可能未实现 "
                "feature.enable_profile_key。在开启 enforce-secure-profile 的服务器里可能无法发送"
                "聊天消息。建议请服务器管理员关闭 enforce-secure-profile，或先确认该验证服务器已"
                "支持聊天签名。",
                "warning");
        }

        // 处理 inheritsFrom: 从原版 JSON 读取 assetIndex 等信息
        // 标准布局是 versions/<inheritsFrom>/<inheritsFrom>.json（PCL / HMCL / 官启
        // 都按这个找前置版本）；早期版本我们把原版 JSON 塞在实例目录里，
        // 下面第二个路径就是给那种老实例留的兼容回退
        nlohmann::json parentVj;
        if (vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string()) {
            std::string parentId = vj["inheritsFrom"].get<std::string>();
            WriteLaunchLog("版本继承自: " + parentId);

            std::vector<std::wstring> candidates = {
                minecraftDir + L"\\versions\\" + Core::StringUtils::Utf8ToWide(parentId) + L"\\" +
                    Core::StringUtils::Utf8ToWide(parentId) + L".json",
                versionDir + L"\\" + Core::StringUtils::Utf8ToWide(parentId) + L".json",
            };

            std::wstring parentJsonPath;
            for (const auto& candidate : candidates) {
                std::ifstream parentFile(candidate);
                if (!parentFile.is_open()) continue;
                std::stringstream parentSS;
                parentSS << parentFile.rdbuf();
                parentFile.close();
                try {
                    parentVj = nlohmann::json::parse(parentSS.str());
                    parentJsonPath = candidate;
                    WriteLaunchLog("原版 JSON 加载成功: " + Core::StringUtils::WideToUtf8(candidate));
                    break;
                } catch (...) {
                    WriteLaunchLog("警告: 原版 JSON 解析失败: " + Core::StringUtils::WideToUtf8(candidate));
                }
            }
            if (parentJsonPath.empty()) {
                WriteLaunchLog("警告: 找不到前置版本 " + parentId +
                               "（应当在 versions\\" + parentId + "\\ 里）");
            }
        }

        // ==================== 构建合并依赖列表 ====================
        // 合并当前版本 JSON 和原版 JSON (inheritsFrom) 的 libraries
        nlohmann::json mergedLibs = nlohmann::json::array();
        std::map<std::string, bool> seenLibNames;

        // 添加原版依赖 (如果存在 inheritsFrom)
        if (parentVj.contains("libraries") && parentVj["libraries"].is_array()) {
            for (const auto& lib : parentVj["libraries"]) {
                std::string name = lib.value("name", "");
                if (!name.empty() && !seenLibNames.count(name)) {
                    mergedLibs.push_back(lib);
                    seenLibNames[name] = true;
                }
            }
            WriteLaunchLog("原版依赖: " + std::to_string(mergedLibs.size()) + " 个");
        }

        // 添加当前版本依赖 (覆盖同名依赖)
        if (vj.contains("libraries") && vj["libraries"].is_array()) {
            for (const auto& lib : vj["libraries"]) {
                std::string name = lib.value("name", "");
                if (name.empty()) continue;
                if (seenLibNames.count(name)) {
                    // 替换已存在的同名依赖 (当前版本优先)
                    for (auto& existing : mergedLibs) {
                        if (existing.value("name", "") == name) {
                            existing = lib;
                            break;
                        }
                    }
                } else {
                    mergedLibs.push_back(lib);
                    seenLibNames[name] = true;
                }
            }
        }

        WriteLaunchLog("合并后依赖: " + std::to_string(mergedLibs.size()) + " 个");

        // ==================== 启动前文件完整性校验 + 自动补齐 ====================
        // 缺什么就补什么：以前只校验不补，缺一个文件用户就得回下载页把整个版本重下一遍。
        // 补不上、且缺的是库 / natives / 核心 jar 才拒绝启动；只缺资源对象就放行。
        std::wstring libsBase = minecraftDir + L"\\libraries";
        const std::wstring versionJar =
            ResolveGameJarPath(minecraftDir, versionDir, profileName, actualVersionId, vj);

        // assetIndex 来源: 当前版本 JSON 或原版 JSON (inheritsFrom)
        nlohmann::json assetIndexObj;
        if (vj.contains("assetIndex") && vj["assetIndex"].is_object()) {
            assetIndexObj = vj["assetIndex"];
        } else if (parentVj.contains("assetIndex") && parentVj["assetIndex"].is_object()) {
            assetIndexObj = parentVj["assetIndex"];
            WriteLaunchLog("使用原版 JSON 的 assetIndex");
        }

        std::string fileError;
        nlohmann::json missingDetail = nlohmann::json::array();
        if (!EnsureGameFiles(mergedLibs, config, minecraftDir, versionJar, vj, parentVj,
                             assetIndexObj, fileError, missingDetail)) {
            WriteLaunchLog("错误: " + fileError);
            return {{"ok", false},
                    {"missingCount", static_cast<int>(missingDetail.size())},
                    {"missing", missingDetail},
                    {"error", fileError}};
        }

        WriteLaunchLog("库文件: OK");
        WriteLaunchLog("Natives: OK");
        WriteLaunchLog("游戏核心: OK");
        WriteLaunchLog("资源文件: OK");

        // 构建类路径 (使用合并后的依赖列表)
        std::vector<std::string> classpath;
        std::vector<std::wstring> nativeJars;

        for (const auto& lib : mergedLibs) {
                if (!MavenUtils::CheckRulesMatch(lib)) continue;

                std::string name = lib.value("name", "");
                if (MavenUtils::IsLibraryIgnored(name, config)) continue;

                Models::MavenCoord coord = MavenUtils::ParseMavenName(name);
                if (coord.groupId.empty() || coord.artifactId.empty() || coord.version.empty()) continue;

                bool isNativeJar = !coord.classifier.empty() &&
                                   (coord.classifier.find("natives") != std::string::npos);

                std::string mavenPath = MavenUtils::BuildMavenPath(coord);

                if (!isNativeJar) {
                    if (lib.contains("downloads") && lib["downloads"].is_object() &&
                        lib["downloads"].contains("artifact") && lib["downloads"]["artifact"].is_object()) {
                        if (lib["downloads"]["artifact"].contains("path")) {
                            mavenPath = lib["downloads"]["artifact"]["path"].get<std::string>();
                        }
                    } else if (lib.contains("downloadUrl") && !lib["downloadUrl"].get<std::string>().empty()) {
                        mavenPath = lib.value("path", mavenPath);
                    }
                }

                std::string localPath = mavenPath;
                std::replace(localPath.begin(), localPath.end(), '/', '\\');
                std::wstring jarPath = libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localPath);

                if (GetFileAttributesW(jarPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    if (isNativeJar) {
                        std::wstring jarNameLower = jarPath;
                        std::transform(jarNameLower.begin(), jarNameLower.end(), jarNameLower.begin(), ::towlower);

                        if (jarNameLower.find(L"arm64") == std::wstring::npos &&
                            jarNameLower.find(L"-x86.") == std::wstring::npos &&
                            jarNameLower.find(L"-x86-") == std::wstring::npos) {
                            nativeJars.push_back(jarPath);
                        }
                    } else {
                        classpath.push_back(Core::StringUtils::WideToUtf8(jarPath));
                    }
                }

                if (!isNativeJar && lib.contains("natives") && lib["natives"].is_object()) {
                    std::string classifierKey;
#if defined(_WIN32)
                    if (lib["natives"].contains("windows")) classifierKey = lib["natives"]["windows"].get<std::string>();
#elif defined(__APPLE__)
                    if (lib["natives"].contains("osx")) classifierKey = lib["natives"]["osx"].get<std::string>();
#else
                    if (lib["natives"].contains("linux")) classifierKey = lib["natives"]["linux"].get<std::string>();
#endif

                    if (!classifierKey.empty() && lib.contains("downloads") && lib["downloads"].is_object() &&
                        lib["downloads"].contains("classifiers") && lib["downloads"]["classifiers"].is_object() &&
                        lib["downloads"]["classifiers"].contains(classifierKey)) {

                        const auto& ci = lib["downloads"]["classifiers"][classifierKey];
                        std::string nativePath = ci.value("path", "");

                        if (nativePath.empty()) {
                            nativePath = MavenUtils::BuildMavenPath(coord);
                            size_t lastSlash = nativePath.find_last_of('/');
                            if (lastSlash != std::string::npos) {
                                std::string classifier = classifierKey;
                                size_t archPos = classifier.find("${arch}");
                                if (archPos != std::string::npos) classifier.replace(archPos, 7, "64");
                                nativePath = nativePath.substr(0, lastSlash + 1) +
                                           coord.artifactId + "-" + coord.version + "-" + classifier + ".jar";
                            }
                        }

                        if (!nativePath.empty()) {
                            std::string localNativePath = nativePath;
                            std::replace(localNativePath.begin(), localNativePath.end(), '/', '\\');
                            std::wstring nativeJarPath = libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localNativePath);
                            if (GetFileAttributesW(nativeJarPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                                std::wstring jarNameLower = nativeJarPath;
                                std::transform(jarNameLower.begin(), jarNameLower.end(), jarNameLower.begin(), ::towlower);

                                if (jarNameLower.find(L"arm64") == std::wstring::npos &&
                                    jarNameLower.find(L"-x86.") == std::wstring::npos &&
                                    jarNameLower.find(L"-x86-") == std::wstring::npos) {
                                    bool alreadyAdded = false;
                                    for (const auto& nj : nativeJars) {
                                        if (nj == nativeJarPath) { alreadyAdded = true; break; }
                                    }
                                    if (!alreadyAdded) {
                                        nativeJars.push_back(nativeJarPath);
                                    }
                                }
                            }
                        }
                    }
                }
            }

        // 现代 Forge / NeoForge 用 BootstrapLauncher + 模块层：打过补丁的客户端
        // （forge-*-client.jar，由安装器的 processors 生成）已经在 libraries 里、
        // 由 Forge 以 `minecraft` 模块提供 net.minecraft.* 。
        // 这时再把原版 client jar 塞进 classpath，它会被当成自动模块 `_1._20._1`，
        // 和 Forge 的 minecraft 模块重复导出同一个包，启动直接崩在：
        //   java.lang.module.ResolutionException: Modules minecraft and _1._20_1
        //   export package net.minecraft.client to module ...
        // 而老版 Forge（LaunchWrapper）和 Fabric / Quilt 都必须在 classpath 上
        // 看到原版 jar，所以只对 BootstrapLauncher 排除。
        const bool needsVanillaJarOnClasspath =
            mainClass.find("bootstraplauncher") == std::string::npos;

        // 添加版本 JAR
        if (needsVanillaJarOnClasspath &&
            GetFileAttributesW(versionJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
            classpath.push_back(Core::StringUtils::WideToUtf8(versionJar));
        }

        WriteLaunchLog("类路径: " + std::to_string(classpath.size()) + " 个 JAR");
        WriteLaunchLog("Natives JAR: " + std::to_string(nativeJars.size()) + " 个");

        // 解压 natives - 使用 PowerShell
        std::wstring nativesDir = versionDir + L"\\natives";
        CreateDirectoryW(nativesDir.c_str(), nullptr);

        int totalExtracted = 0;
        int totalFailed = 0;

        for (const auto& nativeJar : nativeJars) {
            std::wstring jarNameLower = nativeJar;
            std::transform(jarNameLower.begin(), jarNameLower.end(), jarNameLower.begin(), ::towlower);

            if (jarNameLower.find(L"arm64") != std::wstring::npos ||
                jarNameLower.find(L"-x86.") != std::wstring::npos ||
                jarNameLower.find(L"-x86-") != std::wstring::npos) {
                continue;
            }

            WriteLaunchLog("解压 Natives JAR: " + Core::StringUtils::WideToUtf8(nativeJar));

            // 使用 C++ 原生解压
            Core::ZipExtractor::ExtractResult extractResult = Core::ZipExtractor::ExtractNativeLibraries(
                nativeJar, nativesDir,
                [](const std::wstring& filename) -> bool {
                    // 转换为小写
                    std::wstring lower = filename;
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                    
                    // 只解压动态库文件
                    return (lower.find(L".dll") != std::wstring::npos ||
                            lower.find(L".so") != std::wstring::npos ||
                            lower.find(L".dylib") != std::wstring::npos ||
                            lower.find(L".jnilib") != std::wstring::npos);
                }
            );

            if (extractResult.success) {
                totalExtracted += extractResult.filesExtracted;
                WriteLaunchLog("  成功解压 " + std::to_string(extractResult.filesExtracted) + " 个文件");
            } else {
                totalFailed++;
                WriteLaunchLog("  解压失败: " + extractResult.errorMessage);
            }
        }

        WriteLaunchLog("Natives 解压完成: 成功 " + std::to_string(totalExtracted) + " 个文件, 失败 " + std::to_string(totalFailed) + " 个 JAR");

        // LWJGL 2.x 兼容：重命名 64 位 DLL
        if (isLwjgl2) {
            struct { const wchar_t* from; const wchar_t* to; } renames[] = {
                {L"lwjgl64.dll", L"lwjgl.dll"},
                {L"OpenAL64.dll", L"OpenAL.dll"},
            };
            for (const auto& r : renames) {
                std::wstring srcPath = nativesDir + L"\\" + r.from;
                std::wstring dstPath = nativesDir + L"\\" + r.to;
                DWORD srcAttr = GetFileAttributesW(srcPath.c_str());
                if (srcAttr != INVALID_FILE_ATTRIBUTES) {
                    DWORD dstAttr = GetFileAttributesW(dstPath.c_str());
                    if (dstAttr != INVALID_FILE_ATTRIBUTES) {
                        DeleteFileW(dstPath.c_str());
                    }
                    MoveFileW(srcPath.c_str(), dstPath.c_str());
                }
            }
        }

        // 构建类路径字符串
        std::string classpathStr;
        for (size_t i = 0; i < classpath.size(); i++) {
            if (i > 0) classpathStr += ";";
            classpathStr += classpath[i];
        }

        // 带 inheritsFrom 或合并标记的版本 JSON 说明这是模组加载器版本（Forge / Fabric /
        // Quilt / NeoForge）。它们的 launch 完全依赖自己声明的 arguments.jvm
        // （--module-path / -DignoreList / -DlibraryDirectory / -DFabricMcEmu 等）。
        // 纯原版版本 JSON 两样都没有，因此下面的加载器分支不会影响原版启动流程。
        bool isDerivedVersion = IsDerivedProfileJson(vj);
        if (isDerivedVersion) {
            WriteLaunchLog("检测到模组加载器版本, 继承自: " +
                           (vj.contains("inheritsFrom") ? vj.value("inheritsFrom", std::string())
                                                        : std::string("(已合并)")));
        }

        GameSettings settings = ReadGameSettings(config);
        // 我们总有一个目标窗口尺寸（默认 854x480 / 自定义 / 全屏），
        // 因此 has_custom_resolution 一律上报为真，让原版 JSON 里那对
        // --width/--height 规则命中；否则窗口尺寸就完全交给游戏自己的 options.txt
        const bool customResolution = true;

        std::string assetsDirPath = Core::StringUtils::WideToUtf8(minecraftDir) + "\\assets";
        // 版本隔离：开启后该版本的存档 / 配置 / 模组都落在自己的版本文件夹里，
        // 关闭时所有版本共用 .minecraft。整合包实例自带强制隔离标记，不受全局开关影响
        const bool isolated = ShouldIsolateVersion(settings.versionIsolation, isDerivedVersion) ||
                              IsForceIsolated(vj);
        std::string gameDir = isolated ? Core::StringUtils::WideToUtf8(versionDir)
                                       : Core::StringUtils::WideToUtf8(minecraftDir);
        if (isolated) {
            WriteLaunchLog("版本隔离已开启, 游戏目录: " + gameDir);
        }
        // 游戏目录里没有 options.txt 时首次启动会弹英文无障碍引导页，启动前先补上语言设置
        EnsureGameOptionsFile(gameDir, settings.language);
        // assetIndex: 优先从当前版本 JSON 读取, 否则从原版 JSON 读取
        std::string assetIndex = vj.contains("assetIndex") ? vj["assetIndex"].value("id", "") : "";
        if (assetIndex.empty() && parentVj.contains("assetIndex")) {
            assetIndex = parentVj["assetIndex"].value("id", "");
            WriteLaunchLog("assetIndex 从原版 JSON 读取: " + assetIndex);
        }

        // 游戏参数：父版本（原版）在前、当前版本在后，两侧都要收。
        // 加载器版本自己只声明增量（Forge 的 --launchTarget / --fml.*），
        // --username / --gameDir / --userType 这些全在原版 JSON 里，不合并就会丢。
        std::vector<std::string> gameArgList;
        auto collectGameArgs = [&gameArgList, customResolution](const nlohmann::json& src) {
            if (!src.is_object() || !src.contains("arguments") || !src["arguments"].is_object()) return;
            const nlohmann::json& args = src["arguments"];
            if (!args.contains("game") || !args["game"].is_array()) return;

            for (const auto& arg : args["game"]) {
                if (arg.is_string()) {
                    gameArgList.push_back(arg.get<std::string>());
                } else if (arg.is_object()) {
                    if (!CheckArgRules(arg, customResolution)) continue;
                    if (!arg.contains("value")) continue;

                    if (arg["value"].is_string()) {
                        gameArgList.push_back(arg["value"].get<std::string>());
                    } else if (arg["value"].is_array()) {
                        for (const auto& v : arg["value"]) {
                            if (v.is_string()) gameArgList.push_back(v.get<std::string>());
                        }
                    }
                }
            }
        };
        collectGameArgs(parentVj);
        collectGameArgs(vj);

        if (gameArgList.empty()) {
            // 旧版（1.12.2 及更早）只有 minecraftArguments 一个字符串，加载器版本会
            // 在原版串的基础上追加 --tweakClass，所以优先用当前版本的
            std::string legacy;
            if (vj.contains("minecraftArguments") && vj["minecraftArguments"].is_string()) {
                legacy = vj["minecraftArguments"].get<std::string>();
            } else if (parentVj.contains("minecraftArguments") && parentVj["minecraftArguments"].is_string()) {
                legacy = parentVj["minecraftArguments"].get<std::string>();
            }
            std::stringstream legacySS(legacy);
            std::string token;
            while (legacySS >> token) gameArgList.push_back(token);
        }

        auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = str.find(from, pos)) != std::string::npos) {
                str.replace(pos, from.length(), to);
                pos += to.length();
            }
        };

        // ==================== 模组加载器版本的 JVM 参数 ====================
        // 版本 JSON 的 arguments.jvm 使用启动器占位符，这里按官方启动器的语义替换。
        // ${classpath} 需要 classpath 字符串，留到写 argfile 时再替换。
        std::string libsDirStr = Core::StringUtils::WideToUtf8(libsBase);
        std::string nativesDirStr = Core::StringUtils::WideToUtf8(nativesDir);

        auto substituteLoaderValue = [&](std::string& value) {
            replaceAll(value, "${library_directory}", libsDirStr);
            replaceAll(value, "${classpath_separator}", ";");
            replaceAll(value, "${version_name}", profileName);
            replaceAll(value, "${natives_directory}", nativesDirStr);
            replaceAll(value, "${game_directory}", gameDir);
            replaceAll(value, "${assets_root}", assetsDirPath);
            replaceAll(value, "${assets_index_name}", assetIndex);
            replaceAll(value, "${launcher_name}", "launcher");
            replaceAll(value, "${launcher_version}", "1.0");
        };

        // JDK 的 @argfile 按空白字符分词（换行等价于空格），并且引号内的反斜杠是转义符。
        // 因此：含空白的参数必须用双引号包裹，并把每个反斜杠翻倍；
        //       不含空白的参数不加引号，反斜杠保持字面值（Windows 路径的常规情况）。
        auto appendArgfileArg = [](std::string& out, const std::string& arg) {
            if (!arg.empty() && arg.find_first_of(" \t\r\n#\"") == std::string::npos) {
                out += arg + "\n";
                return;
            }
            std::string escaped;
            for (char c : arg) {
                if (c == '\\') {
                    escaped += "\\\\";
                } else if (c == '"') {
                    escaped += "\\\"";
                } else {
                    escaped += c;
                }
            }
            out += "\"" + escaped + "\"\n";
        };

        // ==================== JVM 参数 ====================
        // 原版和模组一视同仁地读 arguments.jvm：原版 JSON 里的
        // -Djna.tmpdir / -Dorg.lwjgl.system.SharedLibraryExtractPath /
        // -Dio.netty.native.workdir / -Dminecraft.launcher.brand 等
        // 决定了原生库解压到哪里，只扫加载器版本会把它们整段丢掉。
        // 加载器版本往往只声明增量，所以要合并父版本（原版），父在前、子在后。
        std::vector<std::string> rawJvmValues;
        auto collectJvmArgs = [&rawJvmValues, customResolution](const nlohmann::json& src) {
            if (!src.is_object() || !src.contains("arguments") || !src["arguments"].is_object()) return;
            const nlohmann::json& args = src["arguments"];
            if (!args.contains("jvm") || !args["jvm"].is_array()) return;

            for (const auto& arg : args["jvm"]) {
                if (arg.is_string()) {
                    rawJvmValues.push_back(arg.get<std::string>());
                } else if (arg.is_object()) {
                    if (!CheckArgRules(arg, customResolution)) continue;
                    if (!arg.contains("value")) continue;

                    if (arg["value"].is_string()) {
                        rawJvmValues.push_back(arg["value"].get<std::string>());
                    } else if (arg["value"].is_array()) {
                        for (const auto& v : arg["value"]) {
                            if (v.is_string()) rawJvmValues.push_back(v.get<std::string>());
                        }
                    }
                }
            }
        };
        collectJvmArgs(parentVj);
        collectJvmArgs(vj);

        // 1.13 以前没有 arguments.jvm，只补最基础的两条（原版启动器当年也是这么做的）
        if (rawJvmValues.empty()) {
            rawJvmValues.push_back("-Djava.library.path=${natives_directory}");
            rawJvmValues.push_back("-cp");
            rawJvmValues.push_back("${classpath}");
        }

        std::vector<std::string> jvmArgsList;
        for (auto value : rawJvmValues) {
            if (value.empty()) continue;
            substituteLoaderValue(value);
            jvmArgsList.push_back(value);
        }
        WriteLaunchLog("JVM 参数: " + std::to_string(jvmArgsList.size()) + " 项");

        // ==================== 占位符替换 ====================
        const std::string versionType = vj.value("type", std::string("release"));

        ArgValues argValues;
        argValues.username = username;
        argValues.uuid = uuid;
        argValues.accessToken = (isMsaAccount || isYggdrasilAccount) ? accessToken : "0";
        argValues.userType = userType;
        argValues.xuid = xuid;
        argValues.gameDir = gameDir;
        argValues.assetsDir = assetsDirPath;
        argValues.assetIndex = assetIndex;
        argValues.versionName = profileName;
        argValues.versionType = versionType;
        argValues.resolutionWidth = std::to_string(settings.windowWidth);
        argValues.resolutionHeight = std::to_string(settings.windowHeight);
        argValues.primaryJar = Core::StringUtils::WideToUtf8(versionJar);
        argValues.gameAssetsDir = Core::StringUtils::WideToUtf8(minecraftDir) + "\\assets\\virtual\\legacy";
        argValues.librariesDir = libsDirStr;
        SubstituteGameArgs(gameArgList, argValues);

        // 原版参数兜底：版本 JSON（或它的父版本）没提供时至少保证游戏能拿到必需参数。
        // 正常安装下这些全部由版本清单提供，下面只会补上 --logFile
        std::set<std::string> presentFlags;
        for (const std::string& arg : gameArgList) {
            if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') presentFlags.insert(arg);
        }
        auto ensurePair = [&gameArgList, &presentFlags](const std::string& flag, const std::string& value) {
            if (value.empty() || presentFlags.count(flag)) return;
            gameArgList.push_back(flag);
            gameArgList.push_back(value);
            presentFlags.insert(flag);
        };
        ensurePair("--gameDir", gameDir);
        ensurePair("--assetsDir", assetsDirPath);
        ensurePair("--assetIndex", assetIndex);
        ensurePair("--username", username);
        ensurePair("--uuid", uuid);
        ensurePair("--accessToken", argValues.accessToken);
        ensurePair("--userType", userType);
        ensurePair("--version", profileName);
        ensurePair("--versionType", versionType);

        // 窗口尺寸：旧版清单里没有这对参数，按设置补上
        if (settings.windowMode != "fullscreen") {
            ensurePair("--width", argValues.resolutionWidth);
            ensurePair("--height", argValues.resolutionHeight);
        }

        std::string logsLatestPath = Core::StringUtils::WideToUtf8(minecraftDir) + "\\logs\\latest.log";
        ensurePair("--logFile", logsLatestPath);

        // --launchTarget 是 Forge / ModLauncher 专有参数，只有引导类才补，
        // 否则 Fabric 之类会把未知参数透传给游戏主类导致启动报错。
        bool hasLaunchTarget = false;
        for (const std::string& arg : gameArgList) {
            if (arg == "--launchTarget") hasLaunchTarget = true;
        }
        bool isForgeLikeLauncher =
            mainClass == "net.minecraftforge.bootstrap.ForgeBootstrap" ||
            mainClass.find("bootstraplauncher") != std::string::npos ||
            mainClass.find("fml.loading.ModLauncher") != std::string::npos;
        if (!hasLaunchTarget && isForgeLikeLauncher) {
            gameArgList.push_back("--launchTarget");
            gameArgList.push_back("forgeclient");
        }

        if (settings.windowMode == "fullscreen") {
            gameArgList.push_back("--fullscreen");
        }

        // 启动游戏
        std::wstring logsDir = minecraftDir + L"\\logs";
        CreateDirectoryW(logsDir.c_str(), nullptr);

        std::string exeDir = Core::StringUtils::WideToUtf8(Core::FileManager::GetExeDir());

        // classpath：加载器版本做模块去重并转成相对路径（参数更短），
        // 原版沿用绝对路径，两边都保持改动前的取值方式
        std::string pathStr;
        if (isDerivedVersion) {
            std::vector<std::string> allJars = classpath;
            std::string versionJarStr = Core::StringUtils::WideToUtf8(versionJar);
            if (needsVanillaJarOnClasspath &&
                GetFileAttributesW(versionJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
                bool found = false;
                for (const auto& jar : allJars) {
                    if (jar == versionJarStr) { found = true; break; }
                }
                if (!found) allJars.push_back(versionJarStr);
            }
            std::vector<std::string> dedupedJars = BuildDeduplicatedClasspath(libsBase, allJars);
            WriteLaunchLog("依赖 jar 数量: " + std::to_string(allJars.size()) +
                           ", 去重后: " + std::to_string(dedupedJars.size()));
            for (size_t i = 0; i < dedupedJars.size(); i++) {
                if (i > 0) pathStr += ";";
                // 必须用绝对路径：游戏进程的 CWD 现在是游戏目录，
                // 相对路径会被解析到 versions\<实例>\ 下面去
                pathStr += dedupedJars[i];
            }
        } else {
            pathStr = classpathStr;
        }

        // 合并进来的 JVM 参数可能已经自带 classpath 与 java.library.path
        // （原版 JSON 的 "-cp ${classpath}" 与 "-Djava.library.path=${natives_directory}"），
        // 此时不要重复添加，避免依赖"后者覆盖前者"的巧合。
        bool argsHaveClasspath = false;
        bool argsHaveLibraryPath = false;
        for (const std::string& jvmArg : jvmArgsList) {
            if (jvmArg.rfind("-Djava.library.path=", 0) == 0) argsHaveLibraryPath = true;
            if (jvmArg == "-cp" || jvmArg == "-classpath" ||
                jvmArg.rfind("-cp=", 0) == 0 || jvmArg.rfind("-classpath=", 0) == 0) {
                argsHaveClasspath = true;
            }
        }

        const int memoryMb = settings.maxMemoryMb > 0
            ? settings.maxMemoryMb
            : ComputeAutoMemoryMb(isDerivedVersion, Core::StringUtils::Utf8ToWide(gameDir));
        WriteLaunchLog("游戏内存: " + std::to_string(memoryMb) + " MB" +
                       (settings.maxMemoryMb > 0 ? "" : " (自动)"));

        // ==================== 装配最终参数 ====================
        // 原版与模组走同一条管线，只在"怎么把参数交给 JVM"上分叉：
        // Java 9+ 有 @argfile（按行解析，免转义，参数再长也不受命令行长度限制），
        // Java 8 没有这个特性，只能拼命令行并自行加引号。
        std::vector<std::string> finalArgs;
        finalArgs.push_back("-Xmx" + std::to_string(memoryMb) + "M");
        finalArgs.push_back("-Djava.awt.headless=false");
        if (!argsHaveLibraryPath) {
            finalArgs.push_back("-Djava.library.path=" + nativesDirStr);
        }
        if (supportsNativeAccess) {
            finalArgs.push_back("--enable-native-access=ALL-UNNAMED");
        }
        // 外置登录：注入 authlib-injector，让游戏把验证请求交给第三方服务器。
        // ⚠ 等号右边必须是探测后确认的完整 API 根地址（不是用户填的站点地址），
        // 否则游戏的验证请求会打到错误的地方，表现为"进服失败"而不是启动失败。
        // 放在 jvmArgsList 之前：-javaagent 属于 JVM 参数，必须在主类之前
        if (isYggdrasilAccount) {
            if (yggdrasilApiRoot.empty()) {
                WriteLaunchLog("错误: 外置登录账号缺少 API 根地址");
                return {{"ok", false}, {"error", "外置登录账号缺少验证服务器地址，请重新登录"}};
            }
            std::string jarError;
            const std::string jarPath = Core::AuthlibInjector::EnsureJar(jarError);
            if (jarPath.empty()) {
                WriteLaunchLog("错误: " + jarError);
                return {{"ok", false}, {"error", jarError}};
            }
            finalArgs.push_back("-javaagent:" + jarPath + "=" + yggdrasilApiRoot);
            WriteLaunchLog("已注入 authlib-injector, API 根地址: " + yggdrasilApiRoot);
        }
        for (std::string jvmArg : jvmArgsList) {
            replaceAll(jvmArg, "${classpath}", pathStr);
            finalArgs.push_back(jvmArg);
        }
        if (!argsHaveClasspath) {
            finalArgs.push_back("-cp");
            finalArgs.push_back(pathStr);
        }
        finalArgs.push_back(mainClass);
        finalArgs.insert(finalArgs.end(), gameArgList.begin(), gameArgList.end());

        std::string cmd;
        // argfile 的路径要在启动后删掉它，所以提到外面声明
        std::wstring argfilePath;
        const bool useArgfile = !(javaMajor > 0 && javaMajor < 9);
        if (useArgfile) {
            std::string argfile;
            for (const std::string& arg : finalArgs) appendArgfileArg(argfile, arg);

            std::wstring argfileDir = minecraftDir + L"\\temp";
            CreateDirectoryW(argfileDir.c_str(), nullptr);
            argfilePath = argfileDir + L"\\launch.argfile";
            std::ofstream argfileOut(argfilePath);
            if (argfileOut.is_open()) {
                argfileOut << argfile;
                argfileOut.close();
                WriteLaunchLog("argfile 已写入: " + Core::StringUtils::WideToUtf8(argfilePath));
                // argfile 同样要绝对路径（CWD 现在是游戏目录）。
                // 不能加引号 —— Java 的 @argfile 不会剥引号，
                // 实测 @"C:\path with space\x.txt" 会报 could not open；
                // 不加引号时空格路径反而正常
                cmd = "\"" + javaPath + "\" @" + Core::StringUtils::WideToUtf8(argfilePath);
            } else {
                // argfile 写不进去（权限 / 磁盘）时退化成命令行，至少别让启动直接失败
                WriteLaunchLog("警告: argfile 写入失败, 回退到直接命令行模式");
            }
        } else {
            WriteLaunchLog("Java " + std::to_string(javaMajor) + " 不支持 @argfile, 使用直接命令行模式");
        }

        if (cmd.empty()) {
            cmd = "\"" + javaPath + "\"";
            for (const std::string& arg : finalArgs) {
                if (arg.empty()) {
                    cmd += " \"\"";
                } else if (arg.find_first_of(" \t") != std::string::npos) {
                    cmd += " \"" + arg + "\"";
                } else {
                    cmd += " " + arg;
                }
            }
        }

        WriteLaunchLog("启动命令: " + RedactCommandSecrets(cmd));

        // 游戏进程的工作目录必须是**游戏目录**，绝不能是 exe 所在目录。
        // 不少模组（Jupiter、FancyMenu 等）用 "./config/xxx.json" 这种相对路径
        // 读配置：CWD 不对就读不到，FancyMenu 的 customizableScreens 会一直是
        // null，接着 DrippyLoadingScreen 在 LoadingOverlay 的静态初始化里空指针，
        // 表现为"进游戏就崩"。PCL 等启动器都是把 CWD 设成游戏目录的。
        std::wstring workDir = Core::StringUtils::Utf8ToWide(gameDir);

        std::wstring outputLogPath = logsDir + L"\\output.log";
        std::wstring errorLogPath = logsDir + L"\\error.log";

        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };

        HANDLE hOutputFile = CreateFileW(outputLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE hErrorFile = CreateFileW(errorLogPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (hOutputFile != INVALID_HANDLE_VALUE) {
            std::string header = "=== Game Output Log ===\nLaunch Time: " + std::to_string(time(nullptr)) + "\n\n";
            DWORD written;
            WriteFile(hOutputFile, header.c_str(), (DWORD)header.size(), &written, nullptr);
        }
        if (hErrorFile != INVALID_HANDLE_VALUE) {
            std::string header = "=== Game Error Log ===\nLaunch Time: " + std::to_string(time(nullptr)) + "\n\n";
            DWORD written;
            WriteFile(hErrorFile, header.c_str(), (DWORD)header.size(), &written, nullptr);
        }

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_SHOW;
        si.hStdOutput = hOutputFile;
        si.hStdError = hErrorFile;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi = {};

        std::wstring wCmd = Core::StringUtils::Utf8ToWide(cmd);
        if (!CreateProcessW(nullptr, wCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi)) {
            DWORD err = GetLastError();
            WriteLaunchLog("错误: 启动失败, 错误码: " + std::to_string(err));
            if (hOutputFile != INVALID_HANDLE_VALUE) CloseHandle(hOutputFile);
            if (hErrorFile != INVALID_HANDLE_VALUE) CloseHandle(hErrorFile);
            return {{"ok", false}, {"error", "Failed to launch, error: " + std::to_string(err)}};
        }

        if (hOutputFile != INVALID_HANDLE_VALUE) CloseHandle(hOutputFile);
        if (hErrorFile != INVALID_HANDLE_VALUE) CloseHandle(hErrorFile);

        WriteLaunchLog("游戏进程已启动, PID: " + std::to_string(pi.dwProcessId));

        // 通知前端：等待游戏窗口出现
        EmitLaunchProgress("waiting_window", pi.dwProcessId);

        // 监听游戏进程和窗口
        HANDLE hProcess = pi.hProcess;
        DWORD processId = pi.dwProcessId;
        Bridge::WebviewBridge* bridge = &bridge_;
        tauricpp::App* app = g_app_ptr;

        std::thread([hProcess, processId, bridge, app, argfilePath]() {
            // 等待游戏窗口出现或进程结束
            DWORD exitCode = 0;
            bool windowFound = false;
            int checkCount = 0;

            while (true) {
                // 检查进程是否已退出
                if (GetExitCodeProcess(hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                    WriteLaunchLog("游戏进程已结束, 退出码: " + std::to_string(exitCode));
                    break;
                }

                // 尝试查找游戏窗口 (LWJGL 窗口)
                HWND gameWindow = nullptr;

                // 枚举所有窗口，查找包含 Minecraft 或 GLFW 的窗口
                struct EnumWindowsData {
                    DWORD processId;
                    HWND foundWindow;
                } enumData = { processId, nullptr };

                EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                    auto* data = reinterpret_cast<EnumWindowsData*>(lParam);
                    DWORD windowProcessId;
                    GetWindowThreadProcessId(hwnd, &windowProcessId);

                    if (windowProcessId == data->processId) {
                        // 检查窗口标题或类名
                        wchar_t className[256] = {0};
                        GetClassNameW(hwnd, className, 256);
                        wchar_t windowTitle[256] = {0};
                        GetWindowTextW(hwnd, windowTitle, 256);

                        std::wstring cls(className);
                        std::wstring title(windowTitle);

                        // GLFW 窗口类名通常是 "GLFW30"
                        // Minecraft 窗口标题通常包含 "Minecraft"
                        if (cls.find(L"GLFW") != std::wstring::npos ||
                            title.find(L"Minecraft") != std::wstring::npos ||
                            title.find(L"minecraft") != std::wstring::npos) {
                            data->foundWindow = hwnd;
                            return FALSE; // 找到窗口，停止枚举
                        }
                    }
                    return TRUE; // 继续枚举
                }, reinterpret_cast<LPARAM>(&enumData));

                gameWindow = enumData.foundWindow;

                if (gameWindow != nullptr && !windowFound) {
                    windowFound = true;
                    WriteLaunchLog("游戏窗口已出现");
                    // 窗口都出来了，说明 JVM 早就读完参数，argfile 可以删了
                    RemoveLaunchArgfile(argfilePath);
                    // 通知前端：游戏窗口已出现
                    nlohmann::json windowEvent = {{"phase", "launch"}, {"status", "window_ready"}, {"pid", (int)processId}};
                    if (app) {
                        try {
                            app->GetBridge().Emit("game.launch.progress", windowEvent);
                        } catch (...) {}
                    }
                }

                checkCount++;
                if (checkCount > 300) { // 最多等待 30 秒
                    WriteLaunchLog("等待游戏窗口超时，继续监控进程");
                    break;
                }

                Sleep(100); // 每 100ms 检查一次
            }

            // 兜底：没等到窗口（等待超时或进程直接退出）也要把 argfile 清掉
            RemoveLaunchArgfile(argfilePath);

            // 等待进程完全结束
            WaitForSingleObject(hProcess, INFINITE);
            GetExitCodeProcess(hProcess, &exitCode);
            CloseHandle(hProcess);
            WriteLaunchLog("========== 游戏进程结束, 退出码: " + std::to_string(exitCode) + " ==========");

            // 通知前端：游戏已结束
            nlohmann::json endEvent = {{"phase", "launch"}, {"status", "game_ended"}, {"exitCode", (int)exitCode}};
            if (app) {
                try {
                    app->GetBridge().Emit("game.launch.progress", endEvent);
                } catch (...) {}
            }
        }).detach();

        return {{"ok", true}, {"pid", (int)pi.dwProcessId}};
    }

    // ============ 模块去重核心逻辑 (纯字符串解析, 无 I/O) ============

    // 从 jar 路径提取 artifactKey (groupId:artifactId[:classifier]), 用于同库不同版本去重
    // 路径格式: .../net/sf/jopt-simple/jopt-simple/5.0.4/jopt-simple-5.0.4.jar
    //
    // classifier 必须计入 key：Forge 的 net.minecraftforge:forge 会同时出现
    // -universal.jar（Forge 本体）和 -client.jar（打过补丁的客户端，含
    // net/minecraft/client/Minecraft.class）。只按 groupId:artifactId 去重会把
    // 后者当重复丢掉，导致 Forge 启动时
    // "Could not find net/minecraft/client/Minecraft.class in classloader"。
    static std::string ExtractArtifactKey(const std::string& jarPath, const std::wstring& libsBase) {
        std::string relPath = jarPath;
        std::string baseStr = Core::StringUtils::WideToUtf8(libsBase);
        if (relPath.find(baseStr) != 0) {
            // 不在 libraries 下的 jar（如版本目录里的游戏核心 jar）不参与去重
            return jarPath;
        }
        relPath = relPath.substr(baseStr.size());
        if (!relPath.empty() && (relPath[0] == '\\' || relPath[0] == '/')) relPath = relPath.substr(1);
        std::replace(relPath.begin(), relPath.end(), '\\', '/');

        std::vector<std::string> parts;
        std::string tok;
        for (char c : relPath) {
            if (c == '/') { parts.push_back(tok); tok.clear(); }
            else tok += c;
        }
        parts.push_back(tok);

        if (parts.size() < 4) return "";
        std::string artifactId = parts[parts.size() - 3];
        std::string versionDir = parts[parts.size() - 2];
        std::string fileName = parts[parts.size() - 1];
        std::string groupId;
        for (size_t i = 0; i < parts.size() - 3; i++) {
            if (i > 0) groupId += ".";
            groupId += parts[i];
        }

        // 从文件名剥出 classifier: <artifactId>-<version>[-classifier].jar
        std::string classifier;
        std::string prefix = artifactId + "-" + versionDir;
        if (fileName.size() > prefix.size() + 4 && fileName.rfind(prefix, 0) == 0) {
            classifier = fileName.substr(prefix.size(), fileName.size() - prefix.size() - 4);
            if (!classifier.empty() && classifier[0] == '-') classifier = classifier.substr(1);
        }

        return groupId + ":" + artifactId + (classifier.empty() ? "" : (":" + classifier));
    }

    // 构建去重后的 classpath (纯字符串解析, 无 I/O, 瞬间完成)
    static std::vector<std::string> BuildDeduplicatedClasspath(
        const std::wstring& libsBase,
        const std::vector<std::string>& allJars) {

        std::vector<std::string> result;
        std::map<std::string, bool> seenArtifacts;
        int excludedConflict = 0;
        int excludedDuplicate = 0;

        for (const auto& jarPath : allJars) {
            std::string jarPathLower = jarPath;
            std::transform(jarPathLower.begin(), jarPathLower.end(), jarPathLower.begin(), ::tolower);

            // 1. 硬编码排除已知冲突库
            if (jarPathLower.find("jopt-simple") != std::string::npos && jarPathLower.find("6.0-alpha") != std::string::npos) {
                excludedConflict++;
                continue;
            }

            // 2. 按 groupId:artifactId 去重, 同库只保留第一个版本
            std::string key = ExtractArtifactKey(jarPath, libsBase);
            if (!key.empty() && seenArtifacts.count(key)) {
                excludedDuplicate++;
                continue;
            }

            result.push_back(jarPath);
            if (!key.empty()) seenArtifacts[key] = true;
        }

        WriteLaunchLog("去重: " + std::to_string(allJars.size()) + " → " +
                       std::to_string(result.size()) + " (冲突 " + std::to_string(excludedConflict) +
                       ", 重复 " + std::to_string(excludedDuplicate) + ")");
        return result;
    }

    // 递归收集目录下所有 jar 文件
    static void CollectAllJars(const std::wstring& dir, std::vector<std::string>& result) {
        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            std::wstring fullPath = dir + L"\\" + name;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                CollectAllJars(fullPath, result);
            } else {
                std::wstring nameLower = name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
                if (nameLower.size() >= 4 && nameLower.substr(nameLower.size() - 4) == L".jar") {
                    result.push_back(Core::StringUtils::WideToUtf8(fullPath));
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    // 将绝对路径转换为相对于基准目录的路径 (缩短命令行长度)
    static std::string MakeRelativePath(const std::string& absolutePath, const std::string& basePath) {
        if (absolutePath.size() < basePath.size()) return absolutePath;
        if (absolutePath.substr(0, basePath.size()) != basePath) return absolutePath;

        std::string rel = absolutePath.substr(basePath.size());
        if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/')) rel = rel.substr(1);
        for (auto& c : rel) { if (c == '\\') c = '/'; }
        return rel;
    }
};

} // namespace Services
