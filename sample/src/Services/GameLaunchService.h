#pragma once
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <mutex>
#include <sstream>
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
#include "Core/ZipExtractor.h"

namespace Services {

class GameLaunchService {
public:
    GameLaunchService(Bridge::WebviewBridge& bridge) : bridge_(bridge), g_app_ptr(nullptr) {
        RegisterCommands();
    }

    void SetApp(tauricpp::App* app) { g_app_ptr = app; }

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
    // 比 MavenUtils::CheckRulesMatch 更完整：额外处理 os.arch 与 features。
    // 这一步很关键——原版 JSON 里有 os.name=osx 的 -XstartOnFirstThread，
    // 在 Windows 上带上它会让 JVM 直接拒绝启动。
    static bool CheckArgRules(const nlohmann::json& entry) {
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
            }
            // 依赖启动器特性（is_demo_user / has_custom_resolution 等）的规则一律不匹配
            if (rule.contains("features") && rule["features"].is_object()) continue;

            if (action == "allow") {
                allowed = true;
            } else if (action == "disallow") {
                allowed = false;
            }
        }
        return allowed;
    }

    // 日志写入
    static void WriteLaunchLog(const std::string& message) {
        std::wstring logDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\logs";
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

        // 获取账号信息
        std::string username = "";
        if (config.contains("accounts") && config["accounts"].is_array()) {
            for (const auto& acc : config["accounts"]) {
                if (acc.is_string() && !acc.get<std::string>().empty()) {
                    username = acc.get<std::string>();
                    break;
                }
            }
        }
        if (username.empty()) {
            return {{"ok", false}, {"error", "No account logged in"}};
        }

        std::string uuid = Core::HashUtils::GenerateOfflineUUID(username);
        WriteLaunchLog("用户名: " + username + ", UUID: " + uuid);

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

        // 处理 inheritsFrom: 从原版 JSON 读取 assetIndex 等信息
        // 原版 JSON 存储在 versions/<profileName>/<inheritsFrom>.json
        nlohmann::json parentVj;
        if (vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string()) {
            std::string parentId = vj["inheritsFrom"].get<std::string>();
            WriteLaunchLog("版本继承自: " + parentId);
            std::wstring parentJsonPath = versionDir + L"\\" +
                Core::StringUtils::Utf8ToWide(parentId) + L".json";
            std::ifstream parentFile(parentJsonPath);
            if (parentFile.is_open()) {
                std::stringstream parentSS;
                parentSS << parentFile.rdbuf();
                parentFile.close();
                try {
                    parentVj = nlohmann::json::parse(parentSS.str());
                    WriteLaunchLog("原版 JSON 加载成功: " + Core::StringUtils::WideToUtf8(parentJsonPath));
                } catch (...) {
                    WriteLaunchLog("警告: 原版 JSON 解析失败: " + parentId);
                }
            } else {
                WriteLaunchLog("警告: 原版 JSON 未找到: " + Core::StringUtils::WideToUtf8(parentJsonPath));
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

        // ==================== 文件完整性检查与自动下载 ====================
        std::wstring libsBase = minecraftDir + L"\\libraries";
        std::vector<std::string> missingFiles;

        WriteLaunchLog("--- 开始文件完整性检查 ---");
        WriteLaunchLog("版本 JSON 路径: " + Core::StringUtils::WideToUtf8(versionJsonPath));

        if (mergedLibs.empty()) {
            WriteLaunchLog("警告: 没有依赖库!");
        } else {
            WriteLaunchLog("依赖库数量: " + std::to_string(mergedLibs.size()));
        }

        // 检查所有库文件 (使用合并后的依赖列表)
        {
            int totalLibs = 0;
            int skippedLibs = 0;
            int checkedLibs = 0;

            for (const auto& lib : mergedLibs) {
                std::string name = lib.value("name", "");
                totalLibs++;

                if (!MavenUtils::CheckRulesMatch(lib)) {
                    skippedLibs++;
                    continue;
                }

                if (MavenUtils::IsLibraryIgnored(name, config)) {
                    WriteLaunchLog("已忽略: " + name + " (在忽略列表中)");
                    skippedLibs++;
                    continue;
                }

                Models::MavenCoord coord = MavenUtils::ParseMavenName(name);
                if (coord.groupId.empty() || coord.artifactId.empty() || coord.version.empty()) {
                    WriteLaunchLog("跳过: " + name + " (无法解析 Maven 坐标)");
                    continue;
                }

                checkedLibs++;

                std::string mavenPath = MavenUtils::BuildMavenPath(coord);
                std::string downloadUrl;
                std::string sha1;

                bool isNativeJar = !coord.classifier.empty() &&
                                   (coord.classifier.find("natives") != std::string::npos);

                if (lib.contains("downloads") && lib["downloads"].is_object()) {
                    if (isNativeJar && lib["downloads"].contains("classifiers") &&
                        lib["downloads"]["classifiers"].is_object() &&
                        lib["downloads"]["classifiers"].contains(coord.classifier)) {
                        const auto& classifierInfo = lib["downloads"]["classifiers"][coord.classifier];
                        downloadUrl = classifierInfo.value("url", "");
                        sha1 = classifierInfo.value("sha1", "");
                        if (classifierInfo.contains("path")) {
                            mavenPath = classifierInfo["path"].get<std::string>();
                        }
                    } else if (!isNativeJar && lib["downloads"].contains("artifact") &&
                               lib["downloads"]["artifact"].is_object()) {
                        const auto& artifact = lib["downloads"]["artifact"];
                        downloadUrl = artifact.value("url", "");
                        sha1 = artifact.value("sha1", "");
                        if (artifact.contains("path")) {
                            mavenPath = artifact["path"].get<std::string>();
                        }
                    }
                }

                if (downloadUrl.empty()) {
                    if (lib.contains("downloadUrl") && !lib["downloadUrl"].get<std::string>().empty()) {
                        downloadUrl = lib["downloadUrl"].get<std::string>();
                        sha1 = lib.value("sha1", "");
                    } else if (lib.contains("url") && !lib["url"].get<std::string>().empty()) {
                        std::string baseUrl = lib["url"].get<std::string>();
                        if (!baseUrl.empty() && baseUrl.back() != '/') baseUrl += '/';
                        downloadUrl = baseUrl + mavenPath;
                    } else {
                        // 使用下载源管理器获取库文件URL（支持镜像故障转移）
                        // 检测是否为Forge依赖
                        bool isForgeDep = (mavenPath.find("net/minecraftforge/") != std::string::npos);
                        std::vector<std::string> libUrls;
                        if (isForgeDep) {
                            libUrls = Core::DownloadSourceManager::GetForgeDownloadUrls(mavenPath);
                        } else {
                            libUrls = Core::DownloadSourceManager::GetLibraryMirrorUrls(mavenPath);
                        }
                        downloadUrl = libUrls.empty() ? "" : libUrls[0];
                    }
                } else {
                    // 已有downloadUrl，检测是否为Forge依赖并替换为镜像源
                    bool isForgeDep = (downloadUrl.find("maven.minecraftforge.net") != std::string::npos) ||
                                      (mavenPath.find("net/minecraftforge/") != std::string::npos);
                    if (isForgeDep) {
                        std::vector<std::string> forgeUrls = Core::DownloadSourceManager::GetForgeDownloadUrls(mavenPath);
                        if (!forgeUrls.empty()) {
                            downloadUrl = forgeUrls[0];
                        }
                    }
                }

                std::string localPath = mavenPath;
                std::replace(localPath.begin(), localPath.end(), '/', '\\');
                std::wstring jarPath = libsBase + L"\\" + Core::StringUtils::Utf8ToWide(localPath);

                DWORD attr = GetFileAttributesW(jarPath.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES) {
                    WriteLaunchLog("缺失: " + name);
                    WriteLaunchLog("  路径: " + Core::StringUtils::WideToUtf8(jarPath));
                    WriteLaunchLog("  URL: " + downloadUrl);
                    if (!downloadUrl.empty()) {
                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(downloadUrl));
                        if (res.status == 200 && !res.body.empty()) {
                            if (!sha1.empty()) {
                                std::string actualSha1 = Core::HashUtils::ComputeSHA1(res.body);
                                if (actualSha1 != sha1) {
                                    WriteLaunchLog("  SHA-1 校验失败!");
                                    missingFiles.push_back(name);
                                    continue;
                                }
                            }
                            std::wstring parentDir = jarPath.substr(0, jarPath.find_last_of(L'\\'));
                            SHCreateDirectoryExW(NULL, parentDir.c_str(), NULL);
                            Core::FileManager::SaveFile(jarPath, res.body);
                            WriteLaunchLog("  下载完成");
                        } else {
                            WriteLaunchLog("  下载失败, 状态码: " + std::to_string(res.status));
                            missingFiles.push_back(name);
                        }
                    }
                } else if (!sha1.empty()) {
                    std::string existingHash = Core::HashUtils::ComputeFileSHA1(jarPath);
                    if (existingHash != sha1) {
                        WriteLaunchLog("损坏: " + name);
                        WriteLaunchLog("  期望: " + sha1);
                        WriteLaunchLog("  实际: " + existingHash);
                        DeleteFileW(jarPath.c_str());
                        if (!downloadUrl.empty()) {
                            Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(downloadUrl));
                            if (res.status == 200 && !res.body.empty()) {
                                Core::FileManager::SaveFile(jarPath, res.body);
                                WriteLaunchLog("  重新下载完成");
                            } else {
                                missingFiles.push_back(name);
                            }
                        }
                    }
                }

                // 处理有 natives 字段的库
                if (!isNativeJar && lib.contains("natives") && lib["natives"].is_object() &&
                    lib.contains("downloads") && lib["downloads"].is_object() &&
                    lib["downloads"].contains("classifiers") && lib["downloads"]["classifiers"].is_object()) {

                    std::string classifierKey;
#if defined(_WIN32)
                    if (lib["natives"].contains("windows")) classifierKey = lib["natives"]["windows"].get<std::string>();
#elif defined(__APPLE__)
                    if (lib["natives"].contains("osx")) classifierKey = lib["natives"]["osx"].get<std::string>();
#else
                    if (lib["natives"].contains("linux")) classifierKey = lib["natives"]["linux"].get<std::string>();
#endif

                    if (!classifierKey.empty() && lib["downloads"]["classifiers"].contains(classifierKey)) {
                        const auto& ci = lib["downloads"]["classifiers"][classifierKey];
                        std::string nativeUrl = ci.value("url", "");
                        std::string nativeSha1 = ci.value("sha1", "");
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

                            DWORD nativeAttr = GetFileAttributesW(nativeJarPath.c_str());
                            if (nativeAttr == INVALID_FILE_ATTRIBUTES) {
                                WriteLaunchLog("缺失 Natives JAR: " + nativePath);
                                if (!nativeUrl.empty()) {
                                    Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(nativeUrl));
                                    if (res.status == 200 && !res.body.empty()) {
                                        if (!nativeSha1.empty()) {
                                            std::string actualSha1 = Core::HashUtils::ComputeSHA1(res.body);
                                            if (actualSha1 != nativeSha1) {
                                                WriteLaunchLog("  Natives JAR SHA-1 校验失败!");
                                                missingFiles.push_back(name + " (natives)");
                                                continue;
                                            }
                                        }
                                        std::wstring parentDir = nativeJarPath.substr(0, nativeJarPath.find_last_of(L'\\'));
                                        SHCreateDirectoryExW(NULL, parentDir.c_str(), NULL);
                                        Core::FileManager::SaveFile(nativeJarPath, res.body);
                                        WriteLaunchLog("  Natives JAR 下载完成");
                                    } else {
                                        WriteLaunchLog("  Natives JAR 下载失败, 状态码: " + std::to_string(res.status));
                                        missingFiles.push_back(name + " (natives)");
                                    }
                                }
                            } else if (!nativeSha1.empty()) {
                                std::string existingHash = Core::HashUtils::ComputeFileSHA1(nativeJarPath);
                                if (existingHash != nativeSha1) {
                                    WriteLaunchLog("损坏 Natives JAR: " + nativePath);
                                    DeleteFileW(nativeJarPath.c_str());
                                    if (!nativeUrl.empty()) {
                                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(nativeUrl));
                                        if (res.status == 200 && !res.body.empty()) {
                                            Core::FileManager::SaveFile(nativeJarPath, res.body);
                                            WriteLaunchLog("  Natives JAR 重新下载完成");
                                        } else {
                                            missingFiles.push_back(name + " (natives)");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            WriteLaunchLog("库文件统计: 总计 " + std::to_string(totalLibs) + ", 跳过 " + std::to_string(skippedLibs) + ", 检查 " + std::to_string(checkedLibs));
        }

        // 检查版本 JAR (游戏核心)
        // 加载器版本本身没有 downloads.client，游戏核心是继承的原版 jar；
        // 路径必须按实际落盘命名解析，否则 jar 不会被加入 classpath。
        std::wstring versionJar = ResolveGameJarPath(minecraftDir, versionDir, profileName, actualVersionId, vj);
        if (vj.contains("downloads") && vj["downloads"].is_object() &&
            vj["downloads"].contains("client") && vj["downloads"]["client"].is_object()) {
            const auto& client = vj["downloads"]["client"];
            std::string clientUrl = client.value("url", "");
            std::string clientSha1 = client.value("sha1", "");

            DWORD jarAttr = GetFileAttributesW(versionJar.c_str());
            if (jarAttr == INVALID_FILE_ATTRIBUTES) {
                WriteLaunchLog("缺失版本 JAR: " + profileName);
                if (!clientUrl.empty()) {
                    Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(clientUrl));
                    if (res.status == 200 && !res.body.empty()) {
                        if (!clientSha1.empty()) {
                            std::string actualSha1 = Core::HashUtils::ComputeSHA1(res.body);
                            if (actualSha1 != clientSha1) {
                                WriteLaunchLog("  SHA-1 校验失败!");
                                missingFiles.push_back(Core::StringUtils::WideToUtf8(versionJar));
                            }
                        }
                        Core::FileManager::SaveFile(versionJar, res.body);
                        WriteLaunchLog("  下载完成");
                    }
                }
            } else if (!clientSha1.empty()) {
                std::string existingHash = Core::HashUtils::ComputeFileSHA1(versionJar);
                if (existingHash != clientSha1) {
                    WriteLaunchLog("损坏版本 JAR: " + profileName);
                    DeleteFileW(versionJar.c_str());
                    if (!clientUrl.empty()) {
                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(clientUrl));
                        if (res.status == 200 && !res.body.empty()) {
                            Core::FileManager::SaveFile(versionJar, res.body);
                        }
                    }
                }
            }
        }

        // 游戏核心 jar 仍然缺失时明确报错。
        // 加载器版本没有 downloads.client 可下载，这里不检查就会一路走到启动，
        // 最终由 Forge/FML 抛出难以定位的 "your vanilla jar may be corrupt"。
        if (GetFileAttributesW(versionJar.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WriteLaunchLog("缺失游戏核心 JAR: " + Core::StringUtils::WideToUtf8(versionJar));
            missingFiles.push_back(Core::StringUtils::WideToUtf8(versionJar));
        }

        // 检查资源文件 (assets)
        std::wstring assetsDir = minecraftDir + L"\\assets";
        std::wstring indexesDir = assetsDir + L"\\indexes";
        std::wstring objectsDir = assetsDir + L"\\objects";

        // assetIndex 来源: 当前版本 JSON 或原版 JSON (inheritsFrom)
        nlohmann::json assetIndexObj;
        if (vj.contains("assetIndex") && vj["assetIndex"].is_object()) {
            assetIndexObj = vj["assetIndex"];
        } else if (parentVj.contains("assetIndex") && parentVj["assetIndex"].is_object()) {
            assetIndexObj = parentVj["assetIndex"];
            WriteLaunchLog("使用原版 JSON 的 assetIndex");
        }

        if (!assetIndexObj.empty()) {
            std::string assetIndexId = assetIndexObj.value("id", "");
            std::string assetIndexUrl = assetIndexObj.value("url", "");
            std::string assetIndexSha1 = assetIndexObj.value("sha1", "");

            if (!assetIndexId.empty()) {
                std::wstring indexPath = indexesDir + L"\\" + Core::StringUtils::Utf8ToWide(assetIndexId) + L".json";

                DWORD indexAttr = GetFileAttributesW(indexPath.c_str());
                if (indexAttr == INVALID_FILE_ATTRIBUTES) {
                    WriteLaunchLog("缺失资源索引: " + assetIndexId);
                    if (!assetIndexUrl.empty()) {
                        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(assetIndexUrl));
                        if (res.status == 200 && !res.body.empty()) {
                            if (!assetIndexSha1.empty()) {
                                std::string actualSha1 = Core::HashUtils::ComputeSHA1(res.body);
                                if (actualSha1 != assetIndexSha1) {
                                    WriteLaunchLog("  资源索引 SHA-1 校验失败!");
                                    missingFiles.push_back("assets/indexes/" + assetIndexId + ".json");
                                }
                            }
                            CreateDirectoryW(indexesDir.c_str(), nullptr);
                            Core::FileManager::SaveFile(indexPath, res.body);
                            WriteLaunchLog("  资源索引下载完成");
                        }
                    }
                }

                std::ifstream indexFile(indexPath);
                if (indexFile.is_open()) {
                    std::stringstream indexSS;
                    indexSS << indexFile.rdbuf();
                    nlohmann::json assetIndex;
                    try {
                        assetIndex = nlohmann::json::parse(indexSS.str());
                    } catch (...) {
                        WriteLaunchLog("警告: 资源索引解析失败");
                    }

                    if (assetIndex.contains("objects") && assetIndex["objects"].is_object()) {
                        int totalAssets = 0;
                        int missingAssets = 0;
                        for (const auto& [name, obj] : assetIndex["objects"].items()) {
                            if (!obj.is_object() || !obj.contains("hash")) continue;
                            totalAssets++;
                            std::string hash = obj["hash"].get<std::string>();
                            if (hash.size() < 2) continue;

                            std::string prefix = hash.substr(0, 2);
                            std::wstring objPath = objectsDir + L"\\" + Core::StringUtils::Utf8ToWide(prefix) + L"\\" + Core::StringUtils::Utf8ToWide(hash);

                            DWORD objAttr = GetFileAttributesW(objPath.c_str());
                            if (objAttr == INVALID_FILE_ATTRIBUTES) {
                                missingAssets++;
                                // 使用下载源管理器获取资源文件URL（支持镜像故障转移）
                                std::vector<std::string> objUrls = Core::DownloadSourceManager::GetResourceMirrorUrls(prefix, hash);
                                std::string objUrl = objUrls.empty() ? "" : objUrls[0];
                                if (!objUrl.empty()) {
                                    Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(objUrl));
                                    if (res.status == 200 && !res.body.empty()) {
                                        std::wstring objDir = objectsDir + L"\\" + Core::StringUtils::Utf8ToWide(prefix);
                                        CreateDirectoryW(objDir.c_str(), nullptr);
                                        Core::FileManager::SaveFile(objPath, res.body);
                                    } else {
                                        missingFiles.push_back("assets/objects/" + prefix + "/" + hash);
                                    }
                                }
                            }
                        }
                        WriteLaunchLog("资源文件: " + std::to_string(totalAssets) + " 个, 缺失 " + std::to_string(missingAssets) + " 个");
                    }
                }
            }
        }

        if (!missingFiles.empty()) {
            WriteLaunchLog("--- 文件检查完成 ---");
            WriteLaunchLog("有 " + std::to_string(missingFiles.size()) + " 个文件无法修复:");
            for (const auto& f : missingFiles) {
                WriteLaunchLog("  - " + f);
            }
            return {{"ok", false}, {"error", "Missing files: " + std::to_string(missingFiles.size())}};
        }

        WriteLaunchLog("--- 文件完整性检查通过 ---");
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

        // 添加版本 JAR
        if (GetFileAttributesW(versionJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
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

        // 带 inheritsFrom 的版本 JSON 说明这是模组加载器版本（Forge / Fabric / Quilt / NeoForge）。
        // 它们的 launch 完全依赖自己声明的 arguments.jvm（--module-path / -DignoreList /
        // -DlibraryDirectory / -DFabricMcEmu 等）。原版版本 JSON 没有 inheritsFrom，
        // 因此下面的加载器分支不会影响任何现有原版启动流程。
        bool isDerivedVersion = vj.contains("inheritsFrom") && vj["inheritsFrom"].is_string();
        if (isDerivedVersion) {
            WriteLaunchLog("检测到模组加载器版本, 继承自: " + vj.value("inheritsFrom", std::string()));
        }

        // 构建游戏参数
        std::string gameArgs;
        if (vj.contains("arguments") && vj["arguments"].is_object() && vj["arguments"].contains("game")) {
            for (const auto& arg : vj["arguments"]["game"]) {
                if (arg.is_string()) {
                    gameArgs += " " + arg.get<std::string>();
                } else if (arg.is_object() && arg.contains("value")) {
                    if (arg["value"].is_string()) {
                        gameArgs += " " + arg["value"].get<std::string>();
                    } else if (arg["value"].is_array()) {
                        for (const auto& v : arg["value"]) {
                            gameArgs += " " + v.get<std::string>();
                        }
                    }
                }
            }
        } else if (vj.contains("minecraftArguments")) {
            gameArgs = " " + vj["minecraftArguments"].get<std::string>();
        }

        std::string assetsDirPath = Core::StringUtils::WideToUtf8(minecraftDir) + "\\assets";
        std::string gameDir = Core::StringUtils::WideToUtf8(minecraftDir);
        // assetIndex: 优先从当前版本 JSON 读取, 否则从原版 JSON 读取
        std::string assetIndex = vj.contains("assetIndex") ? vj["assetIndex"].value("id", "") : "";
        if (assetIndex.empty() && parentVj.contains("assetIndex")) {
            assetIndex = parentVj["assetIndex"].value("id", "");
            WriteLaunchLog("assetIndex 从原版 JSON 读取: " + assetIndex);
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

        std::vector<std::string> jvmArgsList;
        if (isDerivedVersion) {
            // 合并父版本（原版）与当前版本的 arguments.jvm，父在前、子在后。
            // 加载器版本往往只声明增量参数：Forge 1.21 自己只写了
            // -Djava.net.preferIPv6Addresses，而 natives 提取路径(-Djna.tmpdir /
            // -Dorg.lwjgl.system.SharedLibraryExtractPath / -Dio.netty.native.workdir)
            // 都在原版 JSON 里，不合并就会丢。
            std::vector<std::string> rawValues;
            auto collectJvmArgs = [&rawValues](const nlohmann::json& src) {
                if (!src.is_object() || !src.contains("arguments") || !src["arguments"].is_object()) return;
                const nlohmann::json& args = src["arguments"];
                if (!args.contains("jvm") || !args["jvm"].is_array()) return;
                for (const auto& arg : args["jvm"]) {
                    if (arg.is_string()) {
                        rawValues.push_back(arg.get<std::string>());
                    } else if (arg.is_object()) {
                        if (!CheckArgRules(arg)) continue;
                        if (!arg.contains("value")) continue;
                        if (arg["value"].is_string()) {
                            rawValues.push_back(arg["value"].get<std::string>());
                        } else if (arg["value"].is_array()) {
                            for (const auto& v : arg["value"]) {
                                if (v.is_string()) rawValues.push_back(v.get<std::string>());
                            }
                        }
                    }
                }
            };
            collectJvmArgs(parentVj);
            collectJvmArgs(vj);

            for (auto value : rawValues) {
                if (value.empty()) continue;
                substituteLoaderValue(value);
                jvmArgsList.push_back(value);
            }
            if (!jvmArgsList.empty()) {
                WriteLaunchLog("合并后的 JVM 参数: " + std::to_string(jvmArgsList.size()) + " 项");
            }
        }

        replaceAll(gameArgs, "${auth_player_name}", username);
        replaceAll(gameArgs, "${auth_uuid}", uuid);
        replaceAll(gameArgs, "${auth_access_token}", "0");
        replaceAll(gameArgs, "${auth_session}", "0");
        replaceAll(gameArgs, "${game_directory}", gameDir);
        replaceAll(gameArgs, "${assets_root}", assetsDirPath);
        replaceAll(gameArgs, "${assets_index_name}", assetIndex);
        replaceAll(gameArgs, "${version_name}", profileName);
        replaceAll(gameArgs, "${user_properties}", "{}");
        replaceAll(gameArgs, "${user_type}", "mojang");
        replaceAll(gameArgs, "${version_type}", vj.value("type", "release"));

        replaceAll(gameArgs, "${clientid}", "");
        replaceAll(gameArgs, "${auth_xuid}", "");
        replaceAll(gameArgs, "${resolution_width}", "");
        replaceAll(gameArgs, "${resolution_height}", "");
        replaceAll(gameArgs, "${launcher_name}", "");
        replaceAll(gameArgs, "${launcher_version}", "");

        size_t pos = 0;
        while ((pos = gameArgs.find("${", pos)) != std::string::npos) {
            size_t end = gameArgs.find("}", pos);
            if (end != std::string::npos) {
                gameArgs.erase(pos, end - pos + 1);
            } else {
                break;
            }
        }

        auto removeArg = [&gameArgs](const std::string& arg) {
            size_t pos;
            while ((pos = gameArgs.find(arg)) != std::string::npos) {
                size_t start = pos;
                while (start > 0 && gameArgs[start - 1] == ' ') start--;

                size_t end = pos + arg.length();
                while (end < gameArgs.length() && gameArgs[end] == ' ') end++;
                while (end < gameArgs.length() && gameArgs[end] != ' ') end++;

                gameArgs.erase(start, end - start);
            }
        };

        removeArg("--clientId");
        removeArg("--xuid");
        removeArg("--demo");
        removeArg("--width");
        removeArg("--height");

        while ((pos = gameArgs.find("--quickPlay")) != std::string::npos) {
            size_t start = pos;
            while (start > 0 && gameArgs[start - 1] == ' ') start--;

            size_t end = pos + 11;
            while (end < gameArgs.length() && gameArgs[end] != ' ') end++;

            gameArgs.erase(start, end - start);
        }

        while (gameArgs.find("  ") != std::string::npos) {
            replaceAll(gameArgs, "  ", " ");
        }

        while (!gameArgs.empty() && gameArgs.front() == ' ') gameArgs.erase(0, 1);
        while (!gameArgs.empty() && gameArgs.back() == ' ') gameArgs.pop_back();

        // 启动游戏
        std::wstring logsDir = minecraftDir + L"\\logs";
        CreateDirectoryW(logsDir.c_str(), nullptr);

        std::string cmd;
        if (!jvmArgsList.empty()) {
            // ===== 模组加载器版本启动命令 (版本 JSON 自带 JVM 参数 + @argfile) =====
            // 加载器版本通过 arguments.jvm 提供 --module-path / -DignoreList /
            // -DlibraryDirectory / -DFabricMcEmu 等必需参数，参数长且可能含空格，
            // 因此写入 @argfile（按行解析，每行即一个参数，无需转义）。

            std::string exeDir = Core::StringUtils::WideToUtf8(Core::FileManager::GetExeDir());

            // 1. 使用已构建的 classpath (来自 mergedLibs, 已包含原版+加载器依赖)
            std::vector<std::string> allJars = classpath;

            // 2. 添加版本 JAR (如果不在列表中) — 复用上方按实际命名解析出的游戏核心 jar
            std::string versionJarStr = Core::StringUtils::WideToUtf8(versionJar);
            if (GetFileAttributesW(versionJar.c_str()) != INVALID_FILE_ATTRIBUTES) {
                bool found = false;
                for (const auto& j : allJars) {
                    if (j == versionJarStr) { found = true; break; }
                }
                if (!found) allJars.push_back(versionJarStr);
            }

            WriteLaunchLog("依赖 jar 数量: " + std::to_string(allJars.size()));

            // 3. 模块去重 (纯字符串解析, 无 I/O)
            std::vector<std::string> dedupedJars = BuildDeduplicatedClasspath(libsBase, allJars);
            WriteLaunchLog("去重后 jar 数量: " + std::to_string(dedupedJars.size()));

            // 4. 构建相对路径的 classpath 字符串
            std::string pathStr;
            for (size_t i = 0; i < dedupedJars.size(); i++) {
                if (i > 0) pathStr += ";";
                pathStr += MakeRelativePath(dedupedJars[i], exeDir);
            }

            // 5. 构建 argfile
            // 合并进来的参数可能已经提供 classpath 与 java.library.path
            // （原版 JSON 的 "-cp ${classpath}" 与 "-Djava.library.path=${natives_directory}"），
            // 此时不要重复添加，避免依赖"后者覆盖前者"的巧合。
            bool argsHaveClasspath = false;
            bool argsHaveLibraryPath = false;
            for (const auto& jvmArg : jvmArgsList) {
                if (jvmArg.rfind("-Djava.library.path=", 0) == 0) argsHaveLibraryPath = true;
                if (jvmArg == "-cp" || jvmArg == "-classpath" ||
                    jvmArg.rfind("-cp=", 0) == 0 || jvmArg.rfind("-classpath=", 0) == 0) {
                    argsHaveClasspath = true;
                }
            }

            std::string argfile;
            appendArgfileArg(argfile, "-Xmx4G");
            appendArgfileArg(argfile, "-Djava.awt.headless=false");
            appendArgfileArg(argfile, "-Dorg.lwjgl.util.Debug=true");
            if (!argsHaveLibraryPath) {
                appendArgfileArg(argfile, "-Djava.library.path=" + MakeRelativePath(nativesDirStr, exeDir));
            }
            if (supportsNativeAccess) {
                appendArgfileArg(argfile, "--enable-native-access=ALL-UNNAMED");
            }
            for (auto jvmArg : jvmArgsList) {
                replaceAll(jvmArg, "${classpath}", pathStr);
                appendArgfileArg(argfile, jvmArg);
            }
            if (!argsHaveClasspath) {
                appendArgfileArg(argfile, "-cp");
                appendArgfileArg(argfile, pathStr);
            }
            appendArgfileArg(argfile, mainClass);

            // 6. 版本 JSON 的 game 参数 (Forge 的 --launchTarget / --fml.* 等)
            bool hasLaunchTarget = false;
            if (vj.contains("arguments") && vj["arguments"].is_object() && vj["arguments"].contains("game")) {
                for (const auto& arg : vj["arguments"]["game"]) {
                    std::vector<std::string> values;
                    if (arg.is_string()) {
                        values.push_back(arg.get<std::string>());
                    } else if (arg.is_object() && arg.contains("value")) {
                        if (arg["value"].is_string()) {
                            values.push_back(arg["value"].get<std::string>());
                        } else if (arg["value"].is_array()) {
                            for (const auto& v : arg["value"]) {
                                if (v.is_string()) values.push_back(v.get<std::string>());
                            }
                        }
                    }
                    for (auto value : values) {
                        if (value.empty()) continue;
                        substituteLoaderValue(value);
                        if (value == "--launchTarget") hasLaunchTarget = true;
                        appendArgfileArg(argfile, value);
                    }
                }
            }

            // --launchTarget 是 Forge / ModLauncher 专有参数，只有引导类才补，
            // 否则 Fabric 之类会把未知参数透传给游戏主类导致启动报错。
            bool isForgeLikeLauncher =
                mainClass == "net.minecraftforge.bootstrap.ForgeBootstrap" ||
                mainClass.find("bootstraplauncher") != std::string::npos ||
                mainClass.find("fml.loading.ModLauncher") != std::string::npos;
            if (!hasLaunchTarget && isForgeLikeLauncher) {
                appendArgfileArg(argfile, "--launchTarget");
                appendArgfileArg(argfile, "forgeclient");
            }

            appendArgfileArg(argfile, "--gameDir");
            appendArgfileArg(argfile, MakeRelativePath(gameDir, exeDir));
            appendArgfileArg(argfile, "--assetsDir");
            appendArgfileArg(argfile, MakeRelativePath(assetsDirPath, exeDir));
            appendArgfileArg(argfile, "--assetIndex");
            appendArgfileArg(argfile, assetIndex);
            appendArgfileArg(argfile, "--username");
            appendArgfileArg(argfile, username);
            appendArgfileArg(argfile, "--uuid");
            appendArgfileArg(argfile, uuid);
            appendArgfileArg(argfile, "--accessToken");
            appendArgfileArg(argfile, "0");
            appendArgfileArg(argfile, "--version");
            appendArgfileArg(argfile, profileName);
            appendArgfileArg(argfile, "--versionType");
            appendArgfileArg(argfile, vj.value("type", "release"));
            appendArgfileArg(argfile, "--logFile");
            appendArgfileArg(argfile, MakeRelativePath(Core::StringUtils::WideToUtf8(minecraftDir) + "\\logs\\latest.log", exeDir));

            // 7. 写入 argfile
            std::wstring argfileDir = minecraftDir + L"\\temp";
            CreateDirectoryW(argfileDir.c_str(), nullptr);
            std::wstring argfilePath = argfileDir + L"\\loader_launch.argfile";
            {
                std::ofstream argfileOut(argfilePath);
                if (argfileOut.is_open()) {
                    argfileOut << argfile;
                    argfileOut.close();
                    WriteLaunchLog("argfile 已写入: " + Core::StringUtils::WideToUtf8(argfilePath));
                } else {
                    WriteLaunchLog("警告: argfile 写入失败, 回退到直接命令行模式");
                    cmd = "\"" + javaPath + "\" -cp \"" + classpathStr + "\" " + mainClass +
                          (gameArgs.empty() ? "" : " " + gameArgs);
                }
            }

            if (cmd.empty()) {
                std::string argfileRel = MakeRelativePath(Core::StringUtils::WideToUtf8(argfilePath), exeDir);
                cmd = "\"" + javaPath + "\" @" + argfileRel;
            }

            WriteLaunchLog("使用加载器声明参数启动 (@argfile)");
        } else {
            // ===== 普通版本启动命令 (传统 classpath 方式) =====
            cmd = "\"" + javaPath + "\"";
            cmd += " -Djava.awt.headless=false";
            cmd += " -Dorg.lwjgl.util.Debug=true";
            cmd += " -Djava.library.path=\"" + Core::StringUtils::WideToUtf8(nativesDir) + "\"";
            if (supportsNativeAccess) {
                cmd += " --enable-native-access=ALL-UNNAMED";
            }
            cmd += " -cp \"" + classpathStr + "\"";
            cmd += " " + mainClass;
            if (!gameArgs.empty()) {
                cmd += " " + gameArgs;
            }
            cmd += " --logFile \"" + Core::StringUtils::WideToUtf8(minecraftDir) + "\\logs\\latest.log\"";
        }

        WriteLaunchLog("启动命令: " + cmd);

        std::wstring workDir = Core::FileManager::GetExeDir();

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

        std::thread([hProcess, processId, bridge, app]() {
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
