#pragma once
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/HttpClient.h"
#include "Core/FileManager.h"
#include "Core/HashUtils.h"
#include "Core/StringUtils.h"
#include "Core/DownloadSourceManager.h"
#include "Bridge/WebviewBridge.h"
#include "Models/DownloadTypes.h"
#include "Services/MavenUtils.h"

namespace Services {

class DownloadService {
public:
    DownloadService(Bridge::WebviewBridge& bridge) : bridge_(bridge), g_app_ptr(nullptr) {
        RegisterCommands();
    }

    void SetApp(tauricpp::App* app) { g_app_ptr = app; }

private:
    // 写入下载日志
    static void WriteDownloadLog(const std::string& message) {
        std::wstring logDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\logs";
        CreateDirectoryW(logDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char logName[64];
        snprintf(logName, sizeof(logName), "download_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);
        std::wstring logPath = logDir + L"\\" + Core::StringUtils::Utf8ToWide(std::string(logName));

        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

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
    void RegisterCommands() {
        // config.getThreads - 获取下载线程数
        bridge_.RegisterCommand("config.getThreads", [](const nlohmann::json&) -> nlohmann::json {
            std::string configContent = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(configContent); } catch (...) { config = {}; }
            int threads = config.value("downloadThreads", 64);
            return {{"ok", true}, {"threads", threads}};
        });

        // config.setThreads - 设置下载线程数
        bridge_.RegisterCommand("config.setThreads", [](const nlohmann::json& args) -> nlohmann::json {
            int threads = args.value("threads", 64);
            if (threads < 1) threads = 1;
            if (threads > 256) threads = 256;
            std::string configContent = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(configContent); } catch (...) { config = {}; }
            config["downloadThreads"] = threads;
            Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", true}, {"threads", threads}};
        });

        // config.getDownloadSettings - 获取下载设置
        bridge_.RegisterCommand("config.getDownloadSettings", [](const nlohmann::json&) -> nlohmann::json {
            std::string configContent = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(configContent); } catch (...) { config = {}; }
            nlohmann::json settings;
            if (config.contains("downloadSettings") && config["downloadSettings"].is_object()) {
                settings = config["downloadSettings"];
            } else {
                settings = {
                    {"source", "mirror_first"},
                    {"versionListSource", "mirror_first"},
                    {"maxThreads", 64},
                    {"speedLimit", -1}
                };
            }
            return {{"ok", true}, {"settings", settings}};
        });

        // config.setDownloadSettings - 设置下载设置
        bridge_.RegisterCommand("config.setDownloadSettings", [](const nlohmann::json& args) -> nlohmann::json {
            std::string configContent = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try { config = nlohmann::json::parse(configContent); } catch (...) { config = {}; }
            if (args.contains("settings") && args["settings"].is_object()) {
                config["downloadSettings"] = args["settings"];
                if (args["settings"].contains("maxThreads")) {
                    config["downloadThreads"] = args["settings"]["maxThreads"];
                }
            }
            Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"ok", true}};
        });

        // config.getDownloadSources - 获取可用下载源列表
        bridge_.RegisterCommand("config.getDownloadSources", [](const nlohmann::json&) -> nlohmann::json {
            return Core::DownloadSourceManager::GetAvailableSources();
        });

        // config.setDownloadSource - 设置下载源
        bridge_.RegisterCommand("config.setDownloadSource", [](const nlohmann::json& args) -> nlohmann::json {
            std::string source = args.value("source", "official");
            Core::DownloadSourceManager::SetCurrentSource(source);
            return {{"ok", true}, {"source", source}};
        });

        // config.getDownloadSource - 获取当前下载源
        bridge_.RegisterCommand("config.getDownloadSource", [](const nlohmann::json&) -> nlohmann::json {
            std::string source = Core::DownloadSourceManager::GetCurrentSource();
            return {{"ok", true}, {"source", source}};
        });

        // version.download - 下载版本详情并解析
        bridge_.RegisterCommand("version.download", [this](const nlohmann::json& args) -> nlohmann::json {
            std::thread([this, args]() {
                nlohmann::json result = HandleVersionDownload(args);
                nlohmann::json wrapped = {{"phase", "version"}, {"result", result}};
                PushDownloadResult(wrapped);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // assets.download - 下载资源文件
        bridge_.RegisterCommand("assets.download", [this](const nlohmann::json& args) -> nlohmann::json {
            std::thread([this, args]() {
                nlohmann::json result = HandleAssetsDownload(args);
                nlohmann::json wrapped = {{"phase", "assets"}, {"result", result}};
                PushDownloadResult(wrapped);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // libraries.download - 下载库文件
        bridge_.RegisterCommand("libraries.download", [this](const nlohmann::json& args) -> nlohmann::json {
            std::thread([this, args]() {
                nlohmann::json result = HandleLibrariesDownload(args);
                nlohmann::json wrapped = {{"phase", "libraries"}, {"result", result}};
                PushDownloadResult(wrapped);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // client.download - 下载客户端核心 JAR
        bridge_.RegisterCommand("client.download", [this](const nlohmann::json& args) -> nlohmann::json {
            std::thread([this, args]() {
                nlohmann::json result = HandleClientDownload(args);
                nlohmann::json wrapped = {{"phase", "client"}, {"result", result}};
                PushDownloadResult(wrapped);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // download.poll - 轮询获取下载结果
        bridge_.RegisterCommand("download.poll", [this](const nlohmann::json&) -> nlohmann::json {
            nlohmann::json result = PopDownloadResult();
            nlohmann::json progress;
            {
                std::lock_guard<std::mutex> lock(g_latestProgressMtx);
                progress = g_latestProgress;
            }
            nlohmann::json response;
            if (result.is_null()) {
                response = {{"ok", true}, {"hasResult", false}};
            } else {
                response = {{"ok", true}, {"hasResult", true}, {"data", result}};
            }
            if (!progress.is_null()) {
                response["progress"] = progress;
            }
            return response;
        });
    }

    // 发送进度事件到前端
    void EmitProgress(const std::string& phase) {
        nlohmann::json progress = g_progress.ToJson(phase);
        {
            std::lock_guard<std::mutex> lock(g_latestProgressMtx);
            g_latestProgress = progress;
        }
        if (g_app_ptr) {
            try {
                g_app_ptr->GetBridge().Emit("download.progress", progress);
            } catch (...) {}
        }
    }

    // 推送下载结果
    void PushDownloadResult(const nlohmann::json& result) {
        std::lock_guard<std::mutex> lock(g_downloadResultsMtx);
        g_downloadResults.push(result);
    }

    // 弹出下载结果
    nlohmann::json PopDownloadResult() {
        std::lock_guard<std::mutex> lock(g_downloadResultsMtx);
        if (g_downloadResults.empty()) {
            return nullptr;
        }
        nlohmann::json result = g_downloadResults.front();
        g_downloadResults.pop();
        return result;
    }

    // 下载版本详情
    nlohmann::json HandleVersionDownload(const nlohmann::json& args) {
        std::string url = args.value("url", "");
        std::string versionId = args.value("versionId", "");
        std::string actualVersionId = args.value("actualVersionId", "");

        WriteDownloadLog("========== 下载版本详情 ==========");
        WriteDownloadLog("版本ID: " + versionId);
        WriteDownloadLog("实际版本ID: " + actualVersionId);
        WriteDownloadLog("下载URL: " + url);

        if (url.empty() || versionId.empty() || actualVersionId.empty()) {
            WriteDownloadLog("错误: 缺少必要参数");
            return {{"ok", false}, {"error", "Missing url, versionId or actualVersionId"}};
        }

        WriteDownloadLog("开始下载版本JSON...");
        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
        if (res.status != 200 || res.body.empty()) {
            WriteDownloadLog("下载失败, 状态码: " + std::to_string(res.status));
            return {{"ok", false}, {"error", "Failed to download version JSON, status: " + std::to_string(res.status)}};
        }
        WriteDownloadLog("下载成功, 大小: " + std::to_string(res.body.size()) + " 字节");

        std::wstring savePath = Core::FileManager::GetExeDir() + L"\\.minecraft\\versions\\" +
                                Core::StringUtils::Utf8ToWide(versionId) + L"\\" + Core::StringUtils::Utf8ToWide(actualVersionId) + L".json";
        Core::FileManager::SaveFile(savePath, res.body);
        WriteDownloadLog("已保存: " + Core::StringUtils::WideToUtf8(savePath));

        nlohmann::json vj;
        try {
            vj = nlohmann::json::parse(res.body);
        } catch (...) {
            WriteDownloadLog("错误: 版本JSON解析失败");
            return {{"ok", false}, {"error", "Invalid version JSON"}};
        }

        WriteDownloadLog("版本信息:");
        WriteDownloadLog("  - 主类: " + vj.value("mainClass", "N/A"));
        if (vj.contains("libraries") && vj["libraries"].is_array()) {
            WriteDownloadLog("  - 依赖库数量: " + std::to_string(vj["libraries"].size()));
        }

        nlohmann::json result;
        result["ok"] = true;
        result["mainClass"] = vj.value("mainClass", "");

        if (vj.contains("assetIndex") && vj["assetIndex"].is_object()) {
            result["assetIndex"] = {
                {"id", vj["assetIndex"].value("id", "")},
                {"url", vj["assetIndex"].value("url", "")},
                {"sha1", vj["assetIndex"].value("sha1", "")},
                {"size", vj["assetIndex"].value("size", 0)}
            };
        }

        if (vj.contains("downloads") && vj["downloads"].contains("client") &&
            vj["downloads"]["client"].is_object()) {
            const auto& client = vj["downloads"]["client"];
            result["clientDownload"] = {
                {"url", client.value("url", "")},
                {"sha1", client.value("sha1", "")},
                {"size", client.value("size", 0)}
            };
        }

        nlohmann::json libs = nlohmann::json::array();
        if (vj.contains("libraries") && vj["libraries"].is_array()) {
            for (const auto& lib : vj["libraries"]) {
                nlohmann::json libItem;
                std::string libName = lib.value("name", "");
                libItem["name"] = libName;
                libItem["url"] = lib.value("url", "");

                if (lib.contains("downloads") && lib["downloads"].is_object()) {
                    if (lib["downloads"].contains("artifact") && lib["downloads"]["artifact"].is_object()) {
                        const auto& art = lib["downloads"]["artifact"];
                        libItem["downloadUrl"] = art.value("url", "");
                        libItem["path"] = art.value("path", "");
                        libItem["sha1"] = art.value("sha1", "");
                        libItem["size"] = art.value("size", 0);
                    }
                    if (lib["downloads"].contains("classifiers") && lib["downloads"]["classifiers"].is_object()) {
                        libItem["classifiers"] = lib["downloads"]["classifiers"];
                    }
                }

                // 诊断日志: Forge相关库的提取信息
                if (libName.find("forge") != std::string::npos || libName.find("minecraft") != std::string::npos) {
                    WriteDownloadLog("[版本解析] lib=" + libName);
                    WriteDownloadLog("[版本解析]   url=" + lib.value("url", "(无)"));
                    WriteDownloadLog("[版本解析]   downloadUrl=" + libItem.value("downloadUrl", "(无)"));
                    WriteDownloadLog("[版本解析]   path=" + libItem.value("path", "(无)"));
                    WriteDownloadLog("[版本解析]   has_downloads=" + std::string(lib.contains("downloads") ? "true" : "false"));
                }

                if (lib.contains("rules") && lib["rules"].is_array()) {
                    libItem["rules"] = lib["rules"];
                }
                if (lib.contains("natives") && lib["natives"].is_object()) {
                    libItem["natives"] = lib["natives"];
                }
                if (lib.contains("extract") && lib["extract"].is_object()) {
                    libItem["extract"] = lib["extract"];
                }

                libs.push_back(libItem);
            }
        }
        result["libraries"] = libs;

        WriteDownloadLog("========== 版本详情下载完成 ==========");
        return result;
    }

    // 下载客户端核心 JAR
    nlohmann::json HandleClientDownload(const nlohmann::json& args) {
        std::string url = args.value("url", "");
        std::string sha1 = args.value("sha1", "");
        std::string profileName = args.value("profileName", "");
        std::string actualVersionId = args.value("actualVersionId", "");

        WriteDownloadLog("========== 下载客户端核心JAR ==========");
        WriteDownloadLog("版本: " + profileName);
        WriteDownloadLog("实际版本ID: " + actualVersionId);
        WriteDownloadLog("下载URL: " + url);
        WriteDownloadLog("预期SHA-1: " + (sha1.empty() ? "N/A" : sha1));

        if (url.empty() || profileName.empty() || actualVersionId.empty()) {
            WriteDownloadLog("错误: 缺少必要参数");
            return {{"ok", false}, {"error", "Missing url, profileName or actualVersionId"}};
        }

        std::wstring savePath = Core::FileManager::GetExeDir() + L"\\.minecraft\\versions\\" +
                                Core::StringUtils::Utf8ToWide(profileName) + L"\\" + Core::StringUtils::Utf8ToWide(actualVersionId) + L".jar";
        WriteDownloadLog("保存路径: " + Core::StringUtils::WideToUtf8(savePath));

        DWORD attr = GetFileAttributesW(savePath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !sha1.empty()) {
            std::string existingHash = Core::HashUtils::ComputeFileSHA1(savePath);
            if (existingHash == sha1) {
                WriteDownloadLog("文件已存在且SHA-1匹配, 跳过下载");
                return {{"ok", true}, {"skipped", true}, {"failedFiles", nlohmann::json::array()}};
            }
            WriteDownloadLog("文件已存在但SHA-1不匹配, 需要重新下载");
        }

        g_progress.Reset();
        g_progress.totalFiles = 1;
        EmitProgress("client");

        const int maxRetries = 3;
        const int retryDelayMs = 2000;
        nlohmann::json failedArr = nlohmann::json::array();

        WriteDownloadLog("开始下载...");

        for (int attempt = 0; attempt <= maxRetries; attempt++) {
            if (attempt > 0) {
                WriteDownloadLog("重试下载 (第" + std::to_string(attempt) + "次)...");
                std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
            }

            Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
            if (res.status == 200 && !res.body.empty()) {
                WriteDownloadLog("下载完成, 大小: " + std::to_string(res.body.size()) + " 字节");

                if (!sha1.empty()) {
                    std::string computedHash = Core::HashUtils::ComputeSHA1(res.body);
                    WriteDownloadLog("SHA-1校验: 期望=" + sha1 + " 实际=" + computedHash);
                    if (computedHash != sha1) {
                        WriteDownloadLog("SHA-1校验失败");
                        if (attempt == maxRetries) {
                            g_progress.failedFiles = 1;
                            EmitProgress("client");
                            failedArr.push_back({{"name", actualVersionId}, {"url", url}, {"error", "SHA-1 mismatch"}});
                        }
                        continue;
                    }
                    WriteDownloadLog("SHA-1校验通过");
                }

                if (Core::FileManager::SaveFile(savePath, res.body)) {
                    WriteDownloadLog("文件保存成功");
                    g_progress.completedFiles = 1;
                    g_progress.downloadedBytes = (long long)res.body.size();
                    EmitProgress("client");
                    WriteDownloadLog("========== 客户端核心JAR下载完成 ==========");
                    return {{"ok", true}, {"skipped", false}, {"failedFiles", failedArr}};
                }
            } else {
                WriteDownloadLog("下载失败, 状态码: " + std::to_string(res.status));
            }

            if (attempt == maxRetries) {
                WriteDownloadLog("达到最大重试次数, 下载失败");
                g_progress.failedFiles = 1;
                EmitProgress("client");
                std::string errorMsg = "下载失败, 状态码: " + std::to_string(res.status);
                failedArr.push_back({{"name", actualVersionId}, {"url", url}, {"error", errorMsg}});
            }
        }

        WriteDownloadLog("========== 客户端核心JAR下载失败 ==========");
        return {{"ok", false}, {"error", "Download failed after retries"}, {"failedFiles", failedArr}};
    }

    // 下载资源文件
    nlohmann::json HandleAssetsDownload(const nlohmann::json& args) {
        std::string assetIndexUrl = args.value("assetIndexUrl", "");
        std::string assetIndexId = args.value("assetIndexId", "");
        std::string profileName = args.value("profileName", "");

        WriteDownloadLog("========== 开始下载资源文件 ==========");
        WriteDownloadLog("资源索引URL: " + assetIndexUrl);
        WriteDownloadLog("资源索引ID: " + assetIndexId);

        if (assetIndexUrl.empty() || assetIndexId.empty()) {
            WriteDownloadLog("错误: 缺少 assetIndexUrl 或 assetIndexId");
            return {{"ok", false}, {"error", "Missing assetIndexUrl or assetIndexId"}};
        }

        WriteDownloadLog("下载资源索引...");
        Models::HttpResult indexRes = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(assetIndexUrl));
        if (indexRes.status != 200 || indexRes.body.empty()) {
            WriteDownloadLog("下载资源索引失败, 状态码: " + std::to_string(indexRes.status));
            return {{"ok", false}, {"error", "Failed to download asset index"}};
        }
        WriteDownloadLog("资源索引下载成功, 大小: " + std::to_string(indexRes.body.size()) + " 字节");

        std::wstring indexPath = Core::FileManager::GetExeDir() + L"\\.minecraft\\assets\\indexes\\" +
                                 Core::StringUtils::Utf8ToWide(assetIndexId) + L".json";
        Core::FileManager::SaveFile(indexPath, indexRes.body);
        WriteDownloadLog("资源索引已保存: " + Core::StringUtils::WideToUtf8(indexPath));

        nlohmann::json indexJson;
        try {
            indexJson = nlohmann::json::parse(indexRes.body);
        } catch (...) {
            WriteDownloadLog("错误: 资源索引JSON解析失败");
            return {{"ok", false}, {"error", "Invalid asset index JSON"}};
        }

        if (!indexJson.contains("objects") || !indexJson["objects"].is_object()) {
            WriteDownloadLog("错误: 资源索引中没有 objects 字段");
            return {{"ok", false}, {"error", "No objects field in asset index"}};
        }

        std::vector<Models::DownloadTask> assets;
        int skippedExisting = 0;
        for (auto& [name, obj] : indexJson["objects"].items()) {
            if (!obj.is_object() || !obj.contains("hash")) continue;
            std::string hash = obj["hash"].get<std::string>();
            if (hash.size() < 2) continue;

            std::string prefix = hash.substr(0, 2);
            std::wstring objPath = Core::FileManager::GetExeDir() + L"\\.minecraft\\assets\\objects\\" +
                                   Core::StringUtils::Utf8ToWide(prefix) + L"\\" + Core::StringUtils::Utf8ToWide(hash);

            DWORD attr = GetFileAttributesW(objPath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                std::string existingHash = Core::HashUtils::ComputeFileSHA1(objPath);
                if (existingHash == hash) {
                    skippedExisting++;
                    continue;
                }
            }

            // 资产文件专用URL列表：官方源优先，镜像源作为备选
            // 镜像源可能不完整，官方源更可靠
            std::vector<std::string> allUrls = Core::DownloadSourceManager::GetAssetsDownloadUrls(prefix, hash);

            Models::DownloadTask task;
            task.name = name;
            task.url = Core::StringUtils::Utf8ToWide(allUrls.empty() ? "" : allUrls[0]);
            task.mirrorUrls = allUrls;  // 包含所有源（官方+镜像）
            task.savePath = objPath;
            task.expectedSha1 = hash;
            task.resourceType = Models::ResourceType::ASSETS;
            if (obj.contains("size") && obj["size"].is_number()) {
                task.size = obj["size"].get<long long>();
            }
            assets.push_back(std::move(task));
        }

        WriteDownloadLog("资源文件统计:");
        WriteDownloadLog("  - 索引中总数: " + std::to_string(indexJson["objects"].size()));
        WriteDownloadLog("  - 已存在(跳过): " + std::to_string(skippedExisting));
        WriteDownloadLog("  - 需要下载: " + std::to_string(assets.size()));

        int total = (int)assets.size();
        if (total == 0) {
            WriteDownloadLog("所有资源文件已存在, 无需下载");
            return {{"ok", true}, {"total", 0}, {"downloaded", 0}, {"failed", 0}, {"failedFiles", nlohmann::json::array()}};
        }

        // 获取配置的线程数
        std::string cfgContent = Core::ConfigManager::ReadConfigFile();
        nlohmann::json cfg;
        try { cfg = nlohmann::json::parse(cfgContent); } catch (...) { cfg = {}; }
        const int maxConcurrent = cfg.value("downloadThreads", 8);

        WriteDownloadLog("下载配置:");
        WriteDownloadLog("  - 并发线程数: " + std::to_string(maxConcurrent));
        WriteDownloadLog("  - 最大重试次数: 3");

        g_progress.Reset();
        g_progress.totalFiles = total;
        long long totalSize = 0;
        for (const auto& task : assets) totalSize += task.size;
        g_progress.totalBytes = totalSize;
        WriteDownloadLog("  - 总下载大小: " + std::to_string(totalSize / 1024 / 1024) + " MB");
        EmitProgress("assets");

        // 并发下载
        int downloaded = 0;
        int failed = 0;
        std::mutex mtx;
        nlohmann::json failedArr = nlohmann::json::array();
        std::atomic<int> idx(0);
        std::atomic<int> progressCounter(0);

        WriteDownloadLog("开始并发下载...");

        auto worker = [&]() {
            while (true) {
                int i = idx.fetch_add(1);
                if (i >= total) break;
                const auto& task = assets[i];

                // 构建完整URL列表: 主URL + 镜像URL
                std::vector<std::string> allUrls;
                allUrls.push_back(Core::StringUtils::WideToUtf8(task.url));
                for (const auto& url : task.mirrorUrls) {
                    bool found = false;
                    for (const auto& existing : allUrls) {
                        if (existing == url) { found = true; break; }
                    }
                    if (!found) {
                        allUrls.push_back(url);
                    }
                }

                bool downloadSuccess = false;
                for (int attempt = 0; attempt <= task.maxRetries && !downloadSuccess; attempt++) {
                    if (attempt > 0) {
                        WriteDownloadLog("重试下载 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                        task.name + " (第" + std::to_string(attempt) + "次重试)");
                        std::this_thread::sleep_for(std::chrono::milliseconds(task.retryDelayMs));
                    }

                    // 尝试所有URL
                    for (size_t j = 0; j < allUrls.size() && !downloadSuccess; j++) {
                        const auto& url = allUrls[j];
                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
                        if (res.status == 200 && !res.body.empty()) {
                            if (!task.expectedSha1.empty()) {
                                std::string computedHash = Core::HashUtils::ComputeSHA1(res.body);
                                if (computedHash != task.expectedSha1) {
                                    WriteDownloadLog("SHA-1校验失败 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                                    task.name + " 期望:" + task.expectedSha1.substr(0, 8) +
                                                    " 实际:" + computedHash.substr(0, 8));
                                    continue;
                                }
                            }
                            if (Core::FileManager::SaveFile(task.savePath, res.body)) {
                                downloadSuccess = true;
                                std::lock_guard<std::mutex> lock(mtx);
                                downloaded++;
                                g_progress.completedFiles++;
                                g_progress.AddBytes(res.body.size());

                                int currentProgress = progressCounter.fetch_add(1);
                                if ((currentProgress + 1) % 100 == 0 || (currentProgress + 1) == total) {
                                    WriteDownloadLog("下载进度: " + std::to_string(currentProgress + 1) + "/" + std::to_string(total) +
                                                    " (" + std::to_string((currentProgress + 1) * 100 / total) + "%)" +
                                                    " 成功:" + std::to_string(downloaded) + " 失败:" + std::to_string(failed));
                                }

                                EmitProgress("assets");
                            }
                        }
                    }
                }

                if (!downloadSuccess) {
                    std::lock_guard<std::mutex> lock(mtx);
                    failed++;
                    g_progress.failedFiles++;
                    WriteDownloadLog("下载失败 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                    task.name + " 所有下载源均失败");
                    failedArr.push_back({{"name", task.name}, {"url", Core::StringUtils::WideToUtf8(task.url)}, {"error", "All mirrors failed"}});
                }
            }
        };

        {
            std::vector<std::thread> threads;
            for (int i = 0; i < maxConcurrent && i < total; i++) {
                threads.emplace_back(worker);
            }
            for (auto& t : threads) t.join();
        }

        WriteDownloadLog("初始下载完成, 成功:" + std::to_string(downloaded) + " 失败:" + std::to_string(failed));

        // ========== 失败文件重试机制 ==========
        // 资产文件下载失败时，统一对失败文件进行重试（最多3次）
        if (!failedArr.empty()) {
            const int maxRetryRounds = 3;
            for (int round = 0; round < maxRetryRounds && !failedArr.empty(); round++) {
                WriteDownloadLog("--- 资产文件重试 第" + std::to_string(round + 1) + "轮 (共" + std::to_string(failedArr.size()) + "个文件) ---");

                // 从failedArr中提取失败文件信息，构建重试任务
                std::vector<nlohmann::json> retryList = failedArr;
                failedArr = nlohmann::json::array();

                std::vector<Models::DownloadTask> retryTasks;
                for (const auto& failedItem : retryList) {
                    std::string failedName = failedItem.value("name", "");
                    std::string failedUrl = failedItem.value("url", "");

                    // 从原始assets中找到对应的完整任务信息
                    for (const auto& origTask : assets) {
                        if (origTask.name == failedName) {
                            Models::DownloadTask retryTask = origTask;
                            retryTasks.push_back(std::move(retryTask));
                            break;
                        }
                    }
                }

                if (retryTasks.empty()) break;

                std::atomic<int> retryIdx(0);
                std::atomic<int> retryDownloaded(0);
                std::atomic<int> retryFailed(0);
                std::mutex retryMtx;

                auto retryWorker = [&]() {
                    while (true) {
                        int i = retryIdx.fetch_add(1);
                        if (i >= (int)retryTasks.size()) break;
                        const auto& task = retryTasks[i];

                        // 重试时尝试所有URL
                        bool downloadSuccess = false;
                        for (size_t j = 0; j < task.mirrorUrls.size() && !downloadSuccess; j++) {
                            const auto& url = task.mirrorUrls[j];
                            Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
                            if (res.status == 200 && !res.body.empty()) {
                                if (!task.expectedSha1.empty()) {
                                    std::string computedHash = Core::HashUtils::ComputeSHA1(res.body);
                                    if (computedHash != task.expectedSha1) continue;
                                }
                                if (Core::FileManager::SaveFile(task.savePath, res.body)) {
                                    downloadSuccess = true;
                                    std::lock_guard<std::mutex> lock(retryMtx);
                                    retryDownloaded++;
                                    g_progress.completedFiles++;
                                    g_progress.failedFiles--;
                                    g_progress.AddBytes(res.body.size());
                                }
                            }
                        }

                        if (!downloadSuccess) {
                            std::lock_guard<std::mutex> lock(retryMtx);
                            retryFailed++;
                            failedArr.push_back({{"name", task.name}, {"url", Core::StringUtils::WideToUtf8(task.url)}, {"error", "Retry failed"}});
                        }
                    }
                };

                {
                    std::vector<std::thread> retryThreads;
                    for (int i = 0; i < maxConcurrent && i < (int)retryTasks.size(); i++) {
                        retryThreads.emplace_back(retryWorker);
                    }
                    for (auto& t : retryThreads) t.join();
                }

                downloaded += retryDownloaded;
                failed -= retryDownloaded;
                WriteDownloadLog("重试结果: 成功+" + std::to_string(retryDownloaded) + " 失败:" + std::to_string(retryFailed));
                EmitProgress("assets");
            }
        }

        WriteDownloadLog("========== 资源文件下载完成 ==========");
        WriteDownloadLog("统计:");
        WriteDownloadLog("  - 总计: " + std::to_string(total));
        WriteDownloadLog("  - 成功: " + std::to_string(downloaded));
        WriteDownloadLog("  - 失败: " + std::to_string(failed));

        return {
            {"ok", true},
            {"total", total},
            {"downloaded", downloaded},
            {"failed", failed},
            {"failedFiles", failedArr}
        };
    }

    // 下载库文件
    nlohmann::json HandleLibrariesDownload(const nlohmann::json& args) {
        WriteDownloadLog("========== 开始下载库文件 ==========");
        WriteDownloadLog("当前下载源配置: " + Core::DownloadSourceManager::GetCurrentSource());

        if (!args.contains("libraries") || !args["libraries"].is_array()) {
            WriteDownloadLog("错误: 缺少 libraries 数组");
            return {{"ok", false}, {"error", "Missing libraries array"}};
        }

        const auto& libraries = args["libraries"];
        std::wstring libsBase = Core::FileManager::GetExeDir() + L"\\.minecraft\\libraries";
        WriteDownloadLog("库目录: " + Core::StringUtils::WideToUtf8(libsBase));
        WriteDownloadLog("请求数量: " + std::to_string(libraries.size()));

        std::vector<Models::DownloadTask> toDownload;
        int skipped = 0;
        int skippedRules = 0;
        int skippedIgnored = 0;
        int skippedExisting = 0;
        int skippedNoUrl = 0;
        int forgeDetected = 0;

        std::string configContent = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try { config = nlohmann::json::parse(configContent); } catch (...) { config = {}; }

        for (const auto& lib : libraries) {
            if (!MavenUtils::CheckRulesMatch(lib)) {
                skippedRules++;
                skipped++;
                continue;
            }

            std::string name = lib.value("name", "");
            if (MavenUtils::IsLibraryIgnored(name, config)) {
                skippedIgnored++;
                skipped++;
                continue;
            }

            Models::MavenCoord coord = MavenUtils::ParseMavenName(name);
            if (coord.groupId.empty() || coord.artifactId.empty() || coord.version.empty()) {
                continue;
            }

            std::string mavenPath;
            std::wstring downloadUrl;
            std::string sha1;

            if (lib.contains("downloadUrl") && !lib["downloadUrl"].get<std::string>().empty()) {
                downloadUrl = Core::StringUtils::Utf8ToWide(lib["downloadUrl"].get<std::string>());
                mavenPath = lib.value("path", MavenUtils::BuildMavenPath(coord));
                sha1 = lib.value("sha1", "");
            } else if (lib.contains("url") && !lib["url"].get<std::string>().empty()) {
                mavenPath = MavenUtils::BuildMavenPath(coord);
                std::string baseUrl = lib["url"].get<std::string>();
                if (!baseUrl.empty() && baseUrl.back() != '/') baseUrl += '/';
                downloadUrl = Core::StringUtils::Utf8ToWide(baseUrl + mavenPath);
            } else {
                skippedNoUrl++;
                skipped++;
                continue;
            }

            // 获取镜像URL列表，自动检测Forge资源
            std::vector<std::string> mirrorUrls;
            Models::ResourceType resType = Models::ResourceType::LIBRARY;

            // 检测是否为Forge依赖（基于URL路径特征）
            std::string urlStr = Core::StringUtils::WideToUtf8(downloadUrl);
            bool isForgeDep = (urlStr.find("maven.minecraftforge.net") != std::string::npos) ||
                              (urlStr.find("files.minecraftforge.net") != std::string::npos) ||
                              (urlStr.find("net/minecraftforge/") != std::string::npos) ||
                              (mavenPath.find("net/minecraftforge/") != std::string::npos);

            // 诊断日志: 每个库的URL和路径信息
            WriteDownloadLog("[库文件诊断] name=" + name);
            WriteDownloadLog("[库文件诊断]   downloadUrl=" + urlStr);
            WriteDownloadLog("[库文件诊断]   mavenPath=" + mavenPath);
            WriteDownloadLog("[库文件诊断]   isForgeDep=" + std::string(isForgeDep ? "true" : "false"));

            if (isForgeDep) {
                // Forge依赖使用专用BMCLAPI镜像
                forgeDetected++;
                WriteDownloadLog("[FORGE检测] 命中Forge依赖: name=" + name);
                WriteDownloadLog("[FORGE检测]   urlStr=" + urlStr);
                WriteDownloadLog("[FORGE检测]   mavenPath=" + mavenPath);
                mirrorUrls = Core::DownloadSourceManager::GetForgeDownloadUrls(mavenPath);
                resType = Models::ResourceType::FORGE;
            } else {
                // 普通库文件使用标准镜像
                mirrorUrls = Core::DownloadSourceManager::GetLibraryMirrorUrls(mavenPath);
            }

            std::wstring primaryUrl = downloadUrl;
            if (!mirrorUrls.empty()) {
                primaryUrl = Core::StringUtils::Utf8ToWide(mirrorUrls[0]);
            }

            std::string localMavenPath = mavenPath;
            std::replace(localMavenPath.begin(), localMavenPath.end(), '/', '\\');
            std::wstring savePath = libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localMavenPath);

            DWORD attr = GetFileAttributesW(savePath.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                if (!sha1.empty()) {
                    std::string existingHash = Core::HashUtils::ComputeFileSHA1(savePath);
                    if (existingHash == sha1) {
                        skippedExisting++;
                        skipped++;
                    } else {
                        WriteDownloadLog("库文件已损坏, 需要重新下载: " + name);
                        DeleteFileW(savePath.c_str());
                        Models::DownloadTask task;
                        task.name = name;
                        task.url = primaryUrl;
                        task.mirrorUrls = mirrorUrls;
                        task.savePath = savePath;
                        task.expectedSha1 = sha1;
                        task.size = lib.value("size", 0LL);
                        task.resourceType = resType;
                        toDownload.push_back(std::move(task));
                    }
                } else {
                    skippedExisting++;
                    skipped++;
                }
            } else {
                Models::DownloadTask task;
                task.name = name;
                task.url = primaryUrl;
                task.mirrorUrls = mirrorUrls;
                task.savePath = savePath;
                task.expectedSha1 = sha1;
                task.size = lib.value("size", 0LL);
                task.resourceType = resType;
                toDownload.push_back(std::move(task));
            }
        }

        WriteDownloadLog("库文件统计:");
        WriteDownloadLog("  - 请求总数: " + std::to_string(libraries.size()));
        WriteDownloadLog("  - Forge依赖检测: " + std::to_string(forgeDetected) + " 个");
        WriteDownloadLog("  - 需要下载: " + std::to_string(toDownload.size()));
        WriteDownloadLog("  - 已跳过: " + std::to_string(skipped));
        WriteDownloadLog("    - 规则不匹配: " + std::to_string(skippedRules));
        WriteDownloadLog("    - 在忽略列表: " + std::to_string(skippedIgnored));
        WriteDownloadLog("    - 已存在且完整: " + std::to_string(skippedExisting));
        WriteDownloadLog("    - 无下载URL: " + std::to_string(skippedNoUrl));

        int total = (int)toDownload.size();
        if (total == 0) {
            WriteDownloadLog("所有库文件已存在, 无需下载");
            return {{"ok", true}, {"total", 0}, {"downloaded", 0}, {"failed", 0}, {"skipped", skipped}, {"failedFiles", nlohmann::json::array()}};
        }

        g_progress.Reset();
        g_progress.totalFiles = total;
        long long totalSize = 0;
        for (const auto& task : toDownload) totalSize += task.size;
        g_progress.totalBytes = totalSize;
        WriteDownloadLog("  - 总下载大小: " + std::to_string(totalSize / 1024) + " KB");
        EmitProgress("libraries");

        // 获取配置的线程数
        std::string cfgContent2 = Core::ConfigManager::ReadConfigFile();
        nlohmann::json cfg2;
        try { cfg2 = nlohmann::json::parse(cfgContent2); } catch (...) { cfg2 = {}; }
        const int maxConcurrent2 = cfg2.value("downloadThreads", 8);

        WriteDownloadLog("下载配置:");
        WriteDownloadLog("  - 并发线程数: " + std::to_string(maxConcurrent2));
        WriteDownloadLog("  - 最大重试次数: 3");

        int downloaded = 0;
        int failed = 0;
        int forgeDownloaded = 0;
        std::mutex mtx2;
        nlohmann::json failedArr = nlohmann::json::array();
        std::atomic<int> idx2(0);
        std::atomic<int> progressCounter2(0);

        WriteDownloadLog("开始并发下载...");

        auto worker2 = [&]() {
            while (true) {
                int i = idx2.fetch_add(1);
                if (i >= total) break;
                const auto& task = toDownload[i];

                // 构建完整URL列表: 主URL + 镜像URL
                std::vector<std::string> allUrls;
                allUrls.push_back(Core::StringUtils::WideToUtf8(task.url));
                for (const auto& url : task.mirrorUrls) {
                    bool found = false;
                    for (const auto& existing : allUrls) {
                        if (existing == url) { found = true; break; }
                    }
                    if (!found) {
                        allUrls.push_back(url);
                    }
                }

                WriteDownloadLog("[URL列表] [" + std::to_string(i + 1) + "/" + std::to_string(total) + "] " +
                                task.name + " type=" +
                                (task.resourceType == Models::ResourceType::FORGE ? "FORGE" :
                                 task.resourceType == Models::ResourceType::ASSETS ? "ASSETS" : "LIBRARY") +
                                " urls=" + std::to_string(allUrls.size()));

                bool downloadSuccess = false;
                for (int attempt = 0; attempt <= task.maxRetries && !downloadSuccess; attempt++) {
                    if (attempt > 0) {
                        WriteDownloadLog("重试下载 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                        task.name + " (第" + std::to_string(attempt) + "次重试)");
                        std::this_thread::sleep_for(std::chrono::milliseconds(task.retryDelayMs));
                    }

                    // 尝试所有URL
                    for (size_t j = 0; j < allUrls.size() && !downloadSuccess; j++) {
                        const auto& url = allUrls[j];
                        WriteDownloadLog("[下载请求] [" + std::to_string(i + 1) + "/" + std::to_string(total) + "] " +
                                        task.name + " -> " + url);
                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
                        WriteDownloadLog("[下载响应] [" + std::to_string(i + 1) + "/" + std::to_string(total) + "] " +
                                        task.name + " status=" + std::to_string(res.status) +
                                        " size=" + std::to_string(res.body.size()));
                        if (res.status == 200 && !res.body.empty()) {
                            if (!task.expectedSha1.empty()) {
                                std::string computedHash = Core::HashUtils::ComputeSHA1(res.body);
                                if (computedHash != task.expectedSha1) {
                                    WriteDownloadLog("SHA-1校验失败 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                                    task.name + " 期望:" + task.expectedSha1.substr(0, 8) +
                                                    " 实际:" + computedHash.substr(0, 8));
                                    continue;
                                }
                            }
                            if (Core::FileManager::SaveFile(task.savePath, res.body)) {
                                downloadSuccess = true;
                                std::lock_guard<std::mutex> lock(mtx2);
                                downloaded++;
                                if (task.resourceType == Models::ResourceType::FORGE) {
                                    forgeDownloaded++;
                                }
                                g_progress.completedFiles++;
                                g_progress.AddBytes(res.body.size());

                                int currentProgress = progressCounter2.fetch_add(1);
                                if ((currentProgress + 1) % 50 == 0 || (currentProgress + 1) == total) {
                                    WriteDownloadLog("下载进度: " + std::to_string(currentProgress + 1) + "/" + std::to_string(total) +
                                                    " (" + std::to_string((currentProgress + 1) * 100 / total) + "%)" +
                                                    " 成功:" + std::to_string(downloaded) + " 失败:" + std::to_string(failed));
                                }

                                EmitProgress("libraries");
                            }
                        }
                    }
                }

                if (!downloadSuccess) {
                    std::lock_guard<std::mutex> lock(mtx2);
                    failed++;
                    g_progress.failedFiles++;
                    WriteDownloadLog("下载失败 [" + std::to_string(i + 1) + "/" + std::to_string(total) + "]: " +
                                    task.name + " 所有下载源均失败");
                    failedArr.push_back({{"name", task.name}, {"url", Core::StringUtils::WideToUtf8(task.url)}, {"error", "All mirrors failed"}});
                }
            }
        };

        {
            std::vector<std::thread> threads2;
            for (int i = 0; i < maxConcurrent2 && i < total; i++) {
                threads2.emplace_back(worker2);
            }
            for (auto& t : threads2) t.join();
        }

        WriteDownloadLog("========== 库文件下载完成 ==========");
        WriteDownloadLog("统计:");
        WriteDownloadLog("  - 总计: " + std::to_string(total));
        WriteDownloadLog("  - 成功: " + std::to_string(downloaded));
        WriteDownloadLog("  - 失败: " + std::to_string(failed));
        WriteDownloadLog("  - Forge依赖下载: " + std::to_string(forgeDownloaded) + "/" + std::to_string(forgeDetected));

        return {
            {"ok", true},
            {"total", total},
            {"downloaded", downloaded},
            {"failed", failed},
            {"skipped", skipped},
            {"failedFiles", failedArr}
        };
    }

    Bridge::WebviewBridge& bridge_;
    tauricpp::App* g_app_ptr;
    Models::DownloadProgress g_progress;
    nlohmann::json g_latestProgress;
    std::mutex g_latestProgressMtx;
    std::queue<nlohmann::json> g_downloadResults;
    std::mutex g_downloadResultsMtx;
};

} // namespace Services
