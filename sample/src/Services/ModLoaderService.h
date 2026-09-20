#pragma once
// ModLoaderService.h - 模组加载器（Forge / Fabric / Quilt / NeoForge）获取与自动安装
//
// 前端契约（frontend/js/app.js 下载页 -> 版本详情）：
//   modloader.getForgeVersions   { gameVersion } -> { ok, versions: [{ version, downloadUrl }] }
//   modloader.getFabricVersions  { gameVersion } -> 同上
//   modloader.getQuiltVersions   { gameVersion } -> 同上
//   modloader.getNeoForgeVersions{ gameVersion } -> 同上
//   modloader.install            { loaderType, loaderVersion, downloadUrl, gameVersion, profileName }
//   modloader.poll               -> { ok, hasResult, ...installResult }
//
// 安装策略：
//   Fabric / Quilt : 直接取 meta 的 profile json 写入版本 JSON，纯下载，无需 Java。
//   Forge / NeoForge: 下载官方安装器 jar，解析 install_profile.json + version.json，
//                     下载支持库后用 Java 执行 processors（与 PCL / HMCL 原理一致）。

#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <mutex>
#include <atomic>
#include <thread>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <Windows.h>
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

class ModLoaderService {
public:
    ModLoaderService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    Bridge::WebviewBridge& bridge_;

    // 安装结果队列（modloader.poll 取用）
    std::queue<nlohmann::json> results_;
    std::mutex results_mtx_;

    // ==================== 命令注册 ====================

    void RegisterCommands() {
        bridge_.RegisterCommand("modloader.getForgeVersions", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleGetForgeVersions(args);
        });

        bridge_.RegisterCommand("modloader.getFabricVersions", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleGetFabricVersions(args);
        });

        bridge_.RegisterCommand("modloader.getQuiltVersions", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleGetQuiltVersions(args);
        });

        bridge_.RegisterCommand("modloader.getNeoForgeVersions", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleGetNeoForgeVersions(args);
        });

        bridge_.RegisterCommand("modloader.install", [this](const nlohmann::json& args) -> nlohmann::json {
            std::thread([this, args]() {
                nlohmann::json result = HandleInstall(args);
                std::lock_guard<std::mutex> lock(results_mtx_);
                results_.push(result);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        bridge_.RegisterCommand("modloader.poll", [this](const nlohmann::json&) -> nlohmann::json {
            std::lock_guard<std::mutex> lock(results_mtx_);
            if (results_.empty()) {
                return {{"ok", true}, {"hasResult", false}};
            }
            nlohmann::json result = results_.front();
            results_.pop();
            if (!result.is_object()) {
                return {{"ok", true}, {"hasResult", false}};
            }
            result["hasResult"] = true;
            return result;
        });
    }

    // ==================== 通用工具 ====================

    // HTTP GET -> JSON（status 非 200 或解析失败返回 null）
    static nlohmann::json HttpGetJson(const std::string& url) {
        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
        if (res.status != 200 || res.body.empty()) return nullptr;
        try {
            return nlohmann::json::parse(res.body);
        } catch (...) {
            return nullptr;
        }
    }

    // 按顺序尝试多个地址，返回第一个成功的 JSON
    static nlohmann::json HttpGetJsonAny(const std::vector<std::string>& urls, int* lastStatus = nullptr) {
        int status = 0;
        for (const auto& url : urls) {
            Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
            status = res.status;
            if (res.status == 200 && !res.body.empty()) {
                try {
                    return nlohmann::json::parse(res.body);
                } catch (...) {
                }
            }
        }
        if (lastStatus) *lastStatus = status;
        return nullptr;
    }

    // 版本号比较（按 . - 分段，数字段按数值比较），返回 a 是否大于 b
    static bool VersionGreater(const std::string& a, const std::string& b) {
        auto tokenize = [](const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : s) {
                if (c == '.' || c == '-') {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            out.push_back(cur);
            return out;
        };
        auto isNum = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        };

        std::vector<std::string> ta = tokenize(a);
        std::vector<std::string> tb = tokenize(b);
        size_t n = (std::max)(ta.size(), tb.size());
        for (size_t i = 0; i < n; i++) {
            if (i >= ta.size()) return false;
            if (i >= tb.size()) return true;
            if (isNum(ta[i]) && isNum(tb[i])) {
                long long va = _strtoi64(ta[i].c_str(), nullptr, 10);
                long long vb = _strtoi64(tb[i].c_str(), nullptr, 10);
                if (va != vb) return va > vb;
            } else if (ta[i] != tb[i]) {
                return ta[i] > tb[i];
            }
        }
        return false;
    }

    // 将 { version: ... } 数组按版本号降序排列
    static void SortVersionsDesc(nlohmann::json& versions) {
        std::sort(versions.begin(), versions.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return VersionGreater(a.value("version", ""), b.value("version", ""));
        });
    }

    // 单引号安全化（PowerShell 字符串字面量）
    static std::wstring EscapePsW(const std::wstring& s) {
        std::wstring out;
        for (wchar_t c : s) {
            out += c;
            if (c == L'\'') out += L'\'';
        }
        return out;
    }

    static std::wstring Utf8ToWide(const std::string& s) { return Core::StringUtils::Utf8ToWide(s); }
    static std::string WideToUtf8(const std::wstring& s) { return Core::StringUtils::WideToUtf8(s); }

    // 读取文本文件
    static std::string ReadTextFile(const std::wstring& path) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return "";
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    // 从 ZIP/JAR 中提取条目到 outDir：
    //   entries     - 精确条目名列表
    //   prefix      - 前缀匹配（如 "maven/"，可为空）
    //   stripPrefix - 命中 prefix 时是否去掉前缀再落盘
    // 依赖 PowerShell 解压（与 Core::ZipExtractor 同一思路）
    static bool ExtractZipSel(const std::wstring& jarPath, const std::wstring& outDir,
                              const std::vector<std::string>& entries,
                              const std::string& prefix = "", bool stripPrefix = false) {
        if (entries.empty() && prefix.empty()) return true;
        Core::FileManager::CreateDirRecursive(outDir);

        std::wstring script = L"$ErrorActionPreference='Stop'\r\n";
        script += L"Add-Type -AssemblyName System.IO.Compression.FileSystem\r\n";
        script += L"$zip=[IO.Compression.ZipFile]::OpenRead('" + EscapePsW(jarPath) + L"')\r\n";
        script += L"$out='" + EscapePsW(outDir) + L"'\r\n";
        script += L"$names=@(";
        for (size_t i = 0; i < entries.size(); i++) {
            if (i > 0) script += L",";
            script += L"'" + EscapePsW(Utf8ToWide(entries[i])) + L"'";
        }
        script += L")\r\n";
        script += L"$prefix='" + EscapePsW(Utf8ToWide(prefix)) + L"'\r\n";
        script += L"$strip=$" + std::wstring(stripPrefix ? L"true" : L"false") + L"\r\n";
        script += L"foreach($e in $zip.Entries){\r\n";
        script += L"  if($e.FullName.EndsWith('/')){continue}\r\n";
        script += L"  $hit=$false\r\n";
        script += L"  foreach($n in $names){ if($n -eq $e.FullName){$hit=$true;break} }\r\n";
        script += L"  if(-not $hit -and $prefix.Length -gt 0 -and $e.FullName.StartsWith($prefix)){$hit=$true}\r\n";
        script += L"  if(-not $hit){continue}\r\n";
        script += L"  $rel=$e.FullName\r\n";
        script += L"  if($strip -and $prefix.Length -gt 0 -and $rel.StartsWith($prefix)){$rel=$rel.Substring($prefix.Length)}\r\n";
        script += L"  $t=Join-Path $out $rel.Replace('/','\\')\r\n";
        script += L"  $d=Split-Path -Parent $t\r\n";
        script += L"  if(-not (Test-Path $d)){New-Item -ItemType Directory -Force -Path $d | Out-Null}\r\n";
        script += L"  [IO.Compression.ZipFileExtensions]::ExtractToFile($e,$t,$true)\r\n";
        script += L"}\r\n";
        script += L"$zip.Dispose()\r\n";

        std::wstring scriptPath = outDir + L"\\.extract.ps1";
        if (!Core::FileManager::SaveFile(scriptPath, WideToUtf8(script))) return false;

        std::wstring cmd = L"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return false;
        }
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return exitCode == 0;
    }

    // 执行命令并捕获输出，返回退出码（-1 表示启动失败）
    static int RunProcessCapture(const std::wstring& cmdLine, int timeoutMs, const std::wstring& workDir,
                                 std::string* output) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        HANDLE hRead = nullptr;
        HANDLE hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;

        // 子进程需要一个合法的 stdin（GUI 宿主没有控制台，用 NUL 兜底）
        HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.hStdInput = hNul;

        std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
        cmdBuf.push_back(L'\0');

        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr,
                                      workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
        CloseHandle(hWrite);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        if (!started) {
            CloseHandle(hRead);
            return -1;
        }

        std::string collected;
        DWORD waitStart = GetTickCount();
        bool timedOut = false;
        for (;;) {
            DWORD available = 0;
            if (PeekNamedPipe(hRead, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                char buf[4096];
                DWORD read = 0;
                if (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                    buf[read] = '\0';
                    collected += buf;
                }
            } else if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
                // 进程已退出，把管道里剩余数据读完
                DWORD avail2 = 0;
                while (PeekNamedPipe(hRead, nullptr, 0, nullptr, &avail2, nullptr) && avail2 > 0) {
                    char buf[4096];
                    DWORD r = 0;
                    if (!ReadFile(hRead, buf, sizeof(buf) - 1, &r, nullptr) || r == 0) break;
                    buf[r] = '\0';
                    collected += buf;
                }
                break;
            }
            if ((int)(GetTickCount() - waitStart) > timeoutMs) {
                timedOut = true;
                TerminateProcess(pi.hProcess, 1);
                break;
            }
        }

        if (output) *output = collected;
        CloseHandle(hRead);

        DWORD exitCode = 1;
        WaitForSingleObject(pi.hProcess, 5000);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (timedOut) return -2;
        return (int)exitCode;
    }

    // ==================== Maven 坐标解析 ====================

    struct Coord {
        std::string groupId;
        std::string artifactId;
        std::string version;
        std::string classifier;
        std::string ext = "jar";
        std::string key;      // groupId:artifactId:version[:classifier]（不含 @ext）
    };

    // 支持 "group:artifact:version[:classifier][@ext]"
    static Coord ParseCoord(const std::string& raw) {
        Coord c;
        std::string name = raw;
        size_t at = name.find('@');
        if (at != std::string::npos) {
            c.ext = name.substr(at + 1);
            name = name.substr(0, at);
        }

        std::vector<std::string> parts;
        std::stringstream ss(name);
        std::string token;
        while (std::getline(ss, token, ':')) parts.push_back(token);

        if (parts.size() >= 1) c.groupId = parts[0];
        if (parts.size() >= 2) c.artifactId = parts[1];
        if (parts.size() >= 3) c.version = parts[2];
        if (parts.size() >= 4) c.classifier = parts[3];

        c.key = c.groupId + ":" + c.artifactId + ":" + c.version;
        if (!c.classifier.empty()) c.key += ":" + c.classifier;
        return c;
    }

    // maven 本地相对路径：group/artifact/version/artifact-version[-classifier].ext
    static std::string BuildCoordPath(const Coord& c) {
        std::string path;
        for (char ch : c.groupId) {
            path += (ch == '.') ? '/' : ch;
        }
        path += "/" + c.artifactId + "/" + c.version + "/";
        path += c.artifactId + "-" + c.version;
        if (!c.classifier.empty()) path += "-" + c.classifier;
        path += "." + c.ext;
        return path;
    }

    // 由库条目构造本地 maven 路径（优先使用 downloads.artifact.path）
    struct LibFile {
        std::string name;
        std::string mavenPath;
        std::string url;
        std::string sha1;
        long long size = 0;
        bool forgeLike = false;
    };

    static bool ExtractLibFile(const nlohmann::json& lib, LibFile& out) {
        std::string name = lib.value("name", "");
        if (name.empty()) return false;
        if (!MavenUtils::CheckRulesMatch(lib)) return false;

        out.name = name;

        if (lib.contains("downloads") && lib["downloads"].is_object() &&
            lib["downloads"].contains("artifact") && lib["downloads"]["artifact"].is_object()) {
            const auto& art = lib["downloads"]["artifact"];
            out.mavenPath = art.value("path", "");
            out.url = art.value("url", "");
            out.sha1 = art.value("sha1", "");
            out.size = art.value("size", 0);
            if (out.mavenPath.empty()) {
                Coord c = ParseCoord(name);
                if (c.groupId.empty() || c.artifactId.empty() || c.version.empty()) return false;
                out.mavenPath = BuildCoordPath(c);
            }
        } else if (lib.contains("url") && lib["url"].is_string() && !lib["url"].get<std::string>().empty()) {
            Coord c = ParseCoord(name);
            if (c.groupId.empty() || c.artifactId.empty() || c.version.empty()) return false;
            out.mavenPath = BuildCoordPath(c);
            std::string base = lib["url"].get<std::string>();
            if (!base.empty() && base.back() != '/') base += '/';
            out.url = base + out.mavenPath;
            out.sha1 = lib.value("sha1", "");
            out.size = lib.value("size", 0);
        } else {
            return false;
        }

        out.forgeLike = out.url.find("minecraftforge.net") != std::string::npos ||
                        out.url.find("neoforged.net") != std::string::npos ||
                        out.mavenPath.find("net/minecraftforge/") != std::string::npos ||
                        out.mavenPath.find("net/neoforged/") != std::string::npos;
        return true;
    }

    // 库文件候选地址：按当前「文件下载源」模式组合官方与镜像地址
    static std::vector<std::string> BuildMirrors(const LibFile& f) {
        std::vector<std::string> official;
        if (!f.url.empty()) official.push_back(f.url);
        if (f.mavenPath.find("net/neoforged/") != std::string::npos) {
            official.push_back("https://maven.neoforged.net/releases/" + f.mavenPath);
        } else if (f.forgeLike) {
            official.push_back("https://maven.minecraftforge.net/" + f.mavenPath);
        } else {
            official.push_back("https://libraries.minecraft.net/" + f.mavenPath);
        }

        std::vector<std::string> mirror = {
            std::string(Core::DownloadSourceManager::Bmclapi()) + "/maven/" + f.mavenPath
        };
        if (f.forgeLike) {
            mirror.push_back(std::string(Core::DownloadSourceManager::QluBmclapi()) + "/" + f.mavenPath);
        }

        return Core::DownloadSourceManager::ArrangeFile(official, mirror);
    }

    // 库在 libraries 下的落盘路径
    static std::wstring LibSavePath(const LibFile& f) {
        std::string localPath = f.mavenPath;
        std::replace(localPath.begin(), localPath.end(), '/', '\\');
        return LibsBaseDir() + L"\\" + Utf8ToWide(localPath);
    }

    // 下载到 libraries 目录，返回是否成功
    bool DownloadLibFile(const LibFile& f, std::wstring& error) {
        std::wstring savePath = LibSavePath(f);

        if (Core::FileManager::FileExists(savePath)) {
            if (f.sha1.empty()) return true;
            if (Core::HashUtils::ComputeFileSHA1(savePath) == f.sha1) return true;
            DeleteFileW(savePath.c_str());
        }

        Core::FileManager::CreateDirRecursive(savePath.substr(0, savePath.find_last_of(L'\\')));

        std::vector<std::string> tried;
        for (const auto& url : BuildMirrors(f)) {
            tried.push_back(url);
            Models::HttpResult res = Core::HttpClient::HttpGet(Utf8ToWide(url));
            if (res.status != 200 || res.body.empty()) continue;
            if (!f.sha1.empty() && Core::HashUtils::ComputeSHA1(res.body) != f.sha1) continue;
            if (!Core::FileManager::SaveFile(savePath, res.body)) continue;
            return true;
        }

        // 记录所有尝试过的地址，否则只剩一个库名很难定位
        error = Utf8ToWide(f.name) + L" (尝试过 " + Utf8ToWide(std::to_string(tried.size())) + L" 个地址全部失败)";
        WriteInstallLog("下载失败: " + f.name + "  目标: " + WideToUtf8(savePath));
        for (const auto& url : tried) {
            WriteInstallLog("  候选地址: " + url);
        }
        return false;
    }

    // 安装过程日志（追加到 .minecraft/logs/loader_install_YYYYMMDD.log）。
    // 这个服务不写日志时，安装失败只剩前端一个 alert，排查非常困难。
    static void WriteInstallLog(const std::string& message) {
        std::wstring logDir = MinecraftDir() + L"\\logs";
        CreateDirectoryW(logDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char logName[64];
        snprintf(logName, sizeof(logName), "loader_install_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);

        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        std::string line = timestamp + message + "\r\n";
        std::wstring logPath = logDir + L"\\" + Utf8ToWide(std::string(logName));

        HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return;
        SetFilePointer(hFile, 0, nullptr, FILE_END);
        DWORD written = 0;
        WriteFile(hFile, line.c_str(), (DWORD)line.size(), &written, nullptr);
        CloseHandle(hFile);
    }

    // 下载任意文件（安装器 jar 等），返回是否成功
    static bool DownloadToFile(const std::vector<std::string>& urls, const std::wstring& savePath) {
        for (const auto& url : urls) {
            Models::HttpResult res = Core::HttpClient::HttpGet(Utf8ToWide(url));
            if (res.status != 200 || res.body.empty()) continue;
            if (Core::FileManager::SaveFile(savePath, res.body)) return true;
        }
        return false;
    }

    // ==================== 版本列表 ====================

    static std::string ForgeInstallerUrl(const std::string& mc, const std::string& ver) {
        std::string dir = mc + "-" + ver;
        return "https://maven.minecraftforge.net/net/minecraftforge/forge/" + dir +
               "/forge-" + dir + "-installer.jar";
    }

    static std::string NeoForgeInstallerUrl(const std::string& mc, const std::string& ver) {
        if (mc == "1.20.1") {
            std::string dir = mc + "-" + ver;
            return "https://maven.neoforged.net/releases/net/neoforged/forge/" + dir +
                   "/forge-" + dir + "-installer.jar";
        }
        return "https://maven.neoforged.net/releases/net/neoforged/neoforge/" + ver +
               "/neoforge-" + ver + "-installer.jar";
    }

    static std::string FabricProfileUrl(const std::string& mc, const std::string& ver) {
        return "https://meta.fabricmc.net/v2/versions/loader/" + mc + "/" + ver + "/profile/json";
    }

    static std::string QuiltProfileUrl(const std::string& mc, const std::string& ver) {
        return "https://meta.quiltmc.org/v3/versions/loader/" + mc + "/" + ver + "/profile/json";
    }

    static nlohmann::json HandleGetForgeVersions(const nlohmann::json& args) {
        std::string mc = args.value("gameVersion", "");
        if (mc.empty()) return {{"ok", false}, {"error", "Missing gameVersion"}};

        Core::DownloadMode mode = Core::DownloadSourceManager::GetMode(Core::SourceScope::VersionList);
        nlohmann::json versions = nlohmann::json::array();
        std::set<std::string> seen;

        // 镜像源：BMCLAPI 专用接口（已按 MC 版本过滤）
        auto fromMirror = [&]() {
            nlohmann::json list = HttpGetJson(
                std::string(Core::DownloadSourceManager::Bmclapi()) + "/forge/minecraft/" + mc);
            if (!list.is_array()) return;
            for (const auto& item : list) {
                std::string ver = NormalizeLoaderVersion(mc, item.value("version", ""));
                if (ver.empty() || !seen.insert(ver).second) continue;
                versions.push_back({{"version", ver}, {"downloadUrl", ForgeInstallerUrl(mc, ver)}});
            }
        };

        // 官方源：maven-metadata.json
        auto fromOfficial = [&]() {
            nlohmann::json meta = HttpGetJson(
                "https://files.minecraftforge.net/maven/net/minecraftforge/forge/maven-metadata.json");
            if (!(meta.is_object() && meta.contains(mc) && meta[mc].is_array())) return;
            std::string prefix = mc + "-";
            for (const auto& v : meta[mc]) {
                if (!v.is_string()) continue;
                std::string full = v.get<std::string>();
                std::string ver = full.rfind(prefix, 0) == 0 ? full.substr(prefix.size()) : full;
                if (ver.empty() || !seen.insert(ver).second) continue;
                versions.push_back({{"version", ver}, {"downloadUrl", ForgeInstallerUrl(mc, ver)}});
            }
        };

        if (mode == Core::DownloadMode::OfficialOnly) {
            fromOfficial();
        } else if (mode == Core::DownloadMode::MirrorOnly) {
            fromMirror();
        } else {  // 镜像优先，官方兜底
            fromMirror();
            if (versions.empty()) fromOfficial();
        }

        if (versions.empty()) {
            return {{"ok", false}, {"error", "未找到 " + mc + " 对应的 Forge 版本"}};
        }

        SortVersionsDesc(versions);
        return {{"ok", true}, {"versions", versions}};
    }

    static nlohmann::json HandleGetFabricVersions(const nlohmann::json& args) {
        std::string mc = args.value("gameVersion", "");
        if (mc.empty()) return {{"ok", false}, {"error", "Missing gameVersion"}};

        std::vector<std::string> urls = Core::DownloadSourceManager::ArrangeVersionList(
            {"https://meta.fabricmc.net/v2/versions/loader/" + mc},
            {std::string(Core::DownloadSourceManager::Bmclapi()) + "/fabric-meta/v2/versions/loader/" + mc});
        nlohmann::json list = HttpGetJsonAny(urls);
        if (!list.is_array()) {
            return {{"ok", false}, {"error", "获取 Fabric 版本列表失败"}};
        }

        nlohmann::json versions = nlohmann::json::array();
        for (const auto& item : list) {
            if (!item.contains("loader") || !item["loader"].is_object()) continue;
            std::string ver = item["loader"].value("version", "");
            if (ver.empty()) continue;
            versions.push_back({
                {"version", ver},
                {"downloadUrl", FabricProfileUrl(mc, ver)},
                {"stable", item["loader"].value("stable", false)}
            });
        }
        if (versions.empty()) return {{"ok", false}, {"error", "未找到 " + mc + " 对应的 Fabric 版本"}};

        SortVersionsDesc(versions);
        return {{"ok", true}, {"versions", versions}};
    }

    static nlohmann::json HandleGetQuiltVersions(const nlohmann::json& args) {
        std::string mc = args.value("gameVersion", "");
        if (mc.empty()) return {{"ok", false}, {"error", "Missing gameVersion"}};

        // Quilt 目前没有可用的 BMCLAPI 镜像，仅官方源（ArrangeVersionList 会在无镜像时保留官方）
        std::vector<std::string> urls = Core::DownloadSourceManager::ArrangeVersionList(
            {"https://meta.quiltmc.org/v3/versions/loader/" + mc}, {});
        nlohmann::json list = HttpGetJsonAny(urls);
        if (!list.is_array()) {
            return {{"ok", false}, {"error", "获取 Quilt 版本列表失败"}};
        }

        nlohmann::json versions = nlohmann::json::array();
        for (const auto& item : list) {
            if (!item.contains("loader") || !item["loader"].is_object()) continue;
            std::string ver = item["loader"].value("version", "");
            if (ver.empty()) continue;
            versions.push_back({{"version", ver}, {"downloadUrl", QuiltProfileUrl(mc, ver)}});
        }
        if (versions.empty()) return {{"ok", false}, {"error", "未找到 " + mc + " 对应的 Quilt 版本"}};

        SortVersionsDesc(versions);
        return {{"ok", true}, {"versions", versions}};
    }

    // NeoForge 版本号 -> MC 版本（官方 maven-metadata 兜底用）
    // 21.1.1 -> 1.21.1 ; 21.0.x -> 1.21 ; 26.1.x -> 26.1
    static std::string NeoForgePrefixForMc(const std::string& mc) {
        std::vector<std::string> parts;
        std::stringstream ss(mc);
        std::string t;
        while (std::getline(ss, t, '.')) parts.push_back(t);
        if (parts.empty()) return "";
        if (parts[0] == "1" && parts.size() >= 2) {
            std::string patch = parts.size() >= 3 ? parts[2] : "0";
            return parts[1] + "." + patch;
        }
        if (parts.size() >= 2) return parts[0] + "." + parts[1];
        return "";
    }

    static std::vector<std::string> ExtractXmlVersions(const std::string& xml) {
        std::vector<std::string> out;
        const std::string openTag = "<version>";
        const std::string closeTag = "</version>";
        size_t pos = 0;
        while ((pos = xml.find(openTag, pos)) != std::string::npos) {
            size_t start = pos + openTag.size();
            size_t end = xml.find(closeTag, start);
            if (end == std::string::npos) break;
            out.push_back(xml.substr(start, end - start));
            pos = end + closeTag.size();
        }
        return out;
    }

    static nlohmann::json HandleGetNeoForgeVersions(const nlohmann::json& args) {
        std::string mc = args.value("gameVersion", "");
        if (mc.empty()) return {{"ok", false}, {"error", "Missing gameVersion"}};

        Core::DownloadMode mode = Core::DownloadSourceManager::GetMode(Core::SourceScope::VersionList);
        nlohmann::json versions = nlohmann::json::array();
        std::set<std::string> seen;

        // 镜像源：BMCLAPI 按 MC 版本列出
        auto fromMirror = [&]() {
            nlohmann::json list = HttpGetJson(
                std::string(Core::DownloadSourceManager::Bmclapi()) + "/neoforge/list/" + mc);
            if (!list.is_array()) return;
            for (const auto& item : list) {
                std::string ver = NormalizeLoaderVersion(mc, item.value("version", ""));
                if (ver.empty()) continue;
                if (!seen.insert(ver).second) continue;
                versions.push_back({{"version", ver}, {"downloadUrl", NeoForgeInstallerUrl(mc, ver)}});
            }
        };

        // 官方源：maven-metadata.xml
        auto fromOfficial = [&]() {
            if (mc == "1.20.1") {
                std::string xml = ReadUrlText(
                    "https://maven.neoforged.net/releases/net/neoforged/forge/maven-metadata.xml");
                std::string prefix = "1.20.1-";
                for (const auto& full : ExtractXmlVersions(xml)) {
                    if (full.rfind(prefix, 0) != 0) continue;
                    std::string ver = full.substr(prefix.size());
                    if (ver.empty() || !seen.insert(ver).second) continue;
                    versions.push_back({{"version", ver}, {"downloadUrl", NeoForgeInstallerUrl(mc, ver)}});
                }
            } else {
                std::string prefix = NeoForgePrefixForMc(mc);
                std::string xml = ReadUrlText(
                    "https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml");
                for (const auto& ver : ExtractXmlVersions(xml)) {
                    if (prefix.empty() || ver.rfind(prefix + ".", 0) != 0) continue;
                    if (!seen.insert(ver).second) continue;
                    versions.push_back({{"version", ver}, {"downloadUrl", NeoForgeInstallerUrl(mc, ver)}});
                }
            }
        };

        if (mode == Core::DownloadMode::OfficialOnly) {
            fromOfficial();
        } else if (mode == Core::DownloadMode::MirrorOnly) {
            fromMirror();
        } else {  // 镜像优先，官方兜底
            fromMirror();
            if (versions.empty()) fromOfficial();
        }

        if (versions.empty()) {
            return {{"ok", false}, {"error", "未找到 " + mc + " 对应的 NeoForge 版本"}};
        }

        SortVersionsDesc(versions);
        return {{"ok", true}, {"versions", versions}};
    }

    static std::string ReadUrlText(const std::string& url) {
        Models::HttpResult res = Core::HttpClient::HttpGet(Utf8ToWide(url));
        if (res.status != 200) return "";
        return res.body;
    }

    // BMCLAPI 少数条目会返回带前缀的版本号（如 "1.20.1-47.1.85"、"neoforge-21.1.1"），统一清洗
    static std::string NormalizeLoaderVersion(const std::string& mc, const std::string& ver) {
        std::string out = ver;
        const std::string neoforgePrefix = "neoforge-";
        if (out.rfind(neoforgePrefix, 0) == 0) out = out.substr(neoforgePrefix.size());
        const std::string mcPrefix = mc + "-";
        if (out.rfind(mcPrefix, 0) == 0) out = out.substr(mcPrefix.size());
        return out;
    }

    // ==================== 安装 ====================

    static std::wstring MinecraftDir() {
        return Core::FileManager::GetExeDir() + L"\\.minecraft";
    }

    static std::wstring LibsBaseDir() {
        return MinecraftDir() + L"\\libraries";
    }

    // 读取配置中的 Java 路径
    static std::string GetConfigJavaPath() {
        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            return "";
        }
        if (config.contains("game") && config["game"].is_object()) {
            return config["game"].value("javaPath", "");
        }
        return "";
    }

    // 写入版本 JSON 到 versions/<profileName>/<profileName>.json
    static bool WriteVersionJson(const std::string& profileName, const nlohmann::json& versionJson) {
        std::wstring dir = MinecraftDir() + L"\\versions\\" + Utf8ToWide(profileName);
        Core::FileManager::CreateDirRecursive(dir);
        return Core::FileManager::SaveFile(dir + L"\\" + Utf8ToWide(profileName) + L".json", versionJson.dump(2));
    }

    // 查找原版客户端 jar：优先 versions/<profileName>/<gameVersion>.jar，否则全盘扫描
    static std::wstring FindVanillaJar(const std::string& profileName, const std::string& gameVersion) {
        std::wstring versionsDir = MinecraftDir() + L"\\versions";
        std::wstring jarName = Utf8ToWide(gameVersion) + L".jar";

        std::wstring direct = versionsDir + L"\\" + Utf8ToWide(profileName) + L"\\" + jarName;
        if (Core::FileManager::FileExists(direct)) return direct;

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((versionsDir + L"\\*").c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return L"";

        std::wstring found;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring candidate = versionsDir + L"\\" + name + L"\\" + jarName;
            if (Core::FileManager::FileExists(candidate)) {
                found = candidate;
                break;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
        return found;
    }

    // 批量下载某个 JSON（含 libraries 数组）里的支持库。
    // skipPaths：这些本地路径由安装器的 processors 生成，不属于可下载物，
    // 必须跳过 —— 它们在 libraries 里的 downloads.artifact.url 往往是空的，
    // 盲目按 maven 规则拼镜像地址会 404（例如 Forge 1.21 的
    // net.minecraftforge:forge:1.21-51.0.33:client 由 binarypatcher 产出）。
    bool DownloadLibraries(const nlohmann::json& container, std::wstring& error,
                           const std::set<std::wstring>* skipPaths = nullptr) {
        if (!container.contains("libraries") || !container["libraries"].is_array()) return true;

        std::vector<LibFile> files;
        std::set<std::string> seen;
        for (const auto& lib : container["libraries"]) {
            LibFile f;
            if (!ExtractLibFile(lib, f)) continue;
            if (f.mavenPath.empty()) continue;
            if (!seen.insert(f.mavenPath).second) continue;
            if (skipPaths && skipPaths->count(LibSavePath(f))) {
                WriteInstallLog("跳过下载（由安装器 processors 生成）: " + f.name);
                continue;
            }
            files.push_back(f);
        }
        if (files.empty()) return true;

        std::atomic<int> next(0);
        std::atomic<bool> failed(false);
        std::mutex errMtx;
        std::wstring firstError;

        auto worker = [&]() {
            for (;;) {
                int i = next.fetch_add(1);
                if (i >= (int)files.size() || failed.load()) return;
                std::wstring err;
                if (!DownloadLibFile(files[i], err)) {
                    std::lock_guard<std::mutex> lock(errMtx);
                    if (firstError.empty()) firstError = err.empty() ? Utf8ToWide(files[i].name) : err;
                    failed.store(true);
                    return;
                }
            }
        };

        std::vector<std::thread> pool;
        int threads = (int)std::min<size_t>(8, files.size());
        for (int i = 0; i < threads; i++) pool.emplace_back(worker);
        for (auto& t : pool) t.join();

        if (failed.load()) {
            error = firstError;
            return false;
        }
        return true;
    }

    // 处理器执行上下文
    struct ProcContext {
        std::wstring mcRoot;         // {ROOT}
        std::wstring installerPath;  // {INSTALLER}
        std::wstring minecraftJar;   // {MINECRAFT_JAR}
        std::string mcVersion;
        std::map<std::string, std::wstring>* coordToPath = nullptr;
        std::map<std::string, std::wstring>* dataMap = nullptr;
    };

    // 坐标 -> 本地文件路径（优先用已下载库的映射，否则按 maven 规则推导）
    static std::wstring ResolveCoordLocal(const std::string& raw,
                                          const std::map<std::string, std::wstring>& coordToPath) {
        Coord c = ParseCoord(raw);
        auto it = coordToPath.find(c.key);
        if (it != coordToPath.end()) return it->second;
        std::string local = BuildCoordPath(c);
        std::replace(local.begin(), local.end(), '/', '\\');
        return LibsBaseDir() + L"\\" + Utf8ToWide(local);
    }

    static void ReplaceAllStr(std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // 解析 install_profile.data 中的单个值
    static std::wstring ResolveDataValue(const std::string& raw, const ProcContext& ctx,
                                         std::wstring& workDir) {
        if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
            return ResolveCoordLocal(raw.substr(1, raw.size() - 2), *ctx.coordToPath);
        }
        if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
            return Utf8ToWide(raw.substr(1, raw.size() - 2));
        }
        if (raw.size() >= 2 && raw.front() == '/' && raw.back() != '}') {
            // 安装器内部数据文件，解压后使用
            std::string entry = raw;
            while (!entry.empty() && entry.front() == '/') entry.erase(entry.begin());
            ExtractZipSel(ctx.installerPath, workDir, {entry});
            std::string local = entry;
            std::replace(local.begin(), local.end(), '/', '\\');
            return workDir + L"\\" + Utf8ToWide(local);
        }
        return Utf8ToWide(raw);
    }

    // 解析 processor 参数（与 Forge 安装器 PostProcessors 的替换顺序一致）
    static std::wstring ResolveProcessorArg(const std::string& arg, const ProcContext& ctx) {
        std::string s = arg;
        ReplaceAllStr(s, "{SIDE}", "client");
        ReplaceAllStr(s, "{MINECRAFT_JAR}", WideToUtf8(ctx.minecraftJar));
        ReplaceAllStr(s, "{ROOT}", WideToUtf8(ctx.mcRoot));
        ReplaceAllStr(s, "{INSTALLER}", WideToUtf8(ctx.installerPath));
        ReplaceAllStr(s, "{MINECRAFT_VERSION}", ctx.mcVersion);

        if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
            std::string key = s.substr(1, s.size() - 2);
            auto it = ctx.dataMap->find(key);
            if (it != ctx.dataMap->end()) return it->second;
            return Utf8ToWide(s);
        }
        if (s.size() >= 2 && s.front() == '[' && s.back() == ']') {
            return ResolveCoordLocal(s.substr(1, s.size() - 2), *ctx.coordToPath);
        }
        return Utf8ToWide(s);
    }

    // 从 jar 的 MANIFEST.MF 读取 Main-Class
    static std::string ReadJarMainClass(const std::wstring& jarPath, const std::wstring& tmpDir) {
        Core::FileManager::CreateDirRecursive(tmpDir);
        if (!ExtractZipSel(jarPath, tmpDir, {"META-INF/MANIFEST.MF"})) return "";
        std::string text = ReadTextFile(tmpDir + L"\\META-INF\\MANIFEST.MF");
        if (text.empty()) return "";

        std::istringstream ss(text);
        std::string line;
        std::string value;
        bool collecting = false;
        const std::string key = "Main-Class:";
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (collecting) {
                if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
                    value += line.substr(1);
                    continue;
                }
                break;
            }
            if (line.rfind(key, 0) == 0) {
                value = line.substr(key.size());
                collecting = true;
            }
        }
        size_t begin = value.find_first_not_of(" \t");
        size_t end = value.find_last_not_of(" \t\r");
        if (begin == std::string::npos) return "";
        return value.substr(begin, end - begin + 1);
    }

    // 取日志尾部若干行，便于报错定位
    static std::string TailLines(const std::string& text, int maxLines) {
        std::vector<std::string> lines;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
        std::string out;
        int start = (int)lines.size() > maxLines ? (int)lines.size() - maxLines : 0;
        for (int i = start; i < (int)lines.size(); i++) {
            if (!out.empty()) out += " | ";
            out += lines[i];
        }
        if (out.size() > 600) out = out.substr(out.size() - 600);
        return out;
    }

    // Fabric / Quilt：直接下载 meta profile JSON
    nlohmann::json InstallMetaProfile(bool isQuilt, const std::string& mc, const std::string& loaderVer,
                                      const std::string& profileName) {
        std::vector<std::string> official = {
            isQuilt ? QuiltProfileUrl(mc, loaderVer) : FabricProfileUrl(mc, loaderVer)
        };
        std::vector<std::string> mirror;
        if (!isQuilt) {  // Quilt 无 BMCLAPI 镜像
            mirror.push_back(std::string(Core::DownloadSourceManager::Bmclapi()) +
                             "/fabric-meta/v2/versions/loader/" + mc + "/" + loaderVer + "/profile/json");
        }
        nlohmann::json profile = HttpGetJsonAny(
            Core::DownloadSourceManager::ArrangeFile(official, mirror));
        if (profile.is_null() || !profile.is_object()) {
            return {{"ok", false}, {"error", "获取加载器 profile 失败，请检查网络连接"}};
        }

        profile["id"] = profileName;
        if (!profile.contains("inheritsFrom") || !profile["inheritsFrom"].is_string()) {
            profile["inheritsFrom"] = mc;
        }

        if (!WriteVersionJson(profileName, profile)) {
            return {{"ok", false}, {"error", "写入版本 JSON 失败"}};
        }

        std::wstring error;
        if (!DownloadLibraries(profile, error)) {
            return {{"ok", false}, {"error", "下载加载器支持库失败: " + WideToUtf8(error)}};
        }

        return {{"ok", true},
                {"libraries", nlohmann::json::array()},
                {"versionId", profileName},
                {"mainClass", profile.value("mainClass", "")}};
    }

    // Forge / NeoForge：解析安装器 + 执行 processors
    nlohmann::json InstallForgelike(bool isNeoForge, const std::string& mc, const std::string& loaderVer,
                                    const std::string& downloadUrl, const std::string& profileName) {
        std::string loaderName = isNeoForge ? "NeoForge" : "Forge";

        // --- Java 检查 ---
        std::string javaPath = GetConfigJavaPath();
        if (javaPath.empty()) {
            return {{"ok", false},
                    {"error", "未配置 Java，请先在「设置 → 启动器 → Java 路径」中选择 Java 后再安装 " + loaderName}};
        }
        if (!Core::FileManager::FileExists(Utf8ToWide(javaPath))) {
            return {{"ok", false}, {"error", "Java 路径无效: " + javaPath}};
        }

        // --- 原版客户端 jar ---
        std::wstring vanillaJar = FindVanillaJar(profileName, mc);
        if (vanillaJar.empty()) {
            return {{"ok", false},
                    {"error", "未找到原版客户端 jar (versions/" + profileName + "/" + mc +
                              ".jar)，请先下载原版 " + mc}};
        }

        // --- 工作目录 ---
        std::wstring workRoot = MinecraftDir() + L"\\temp\\loader_install";
        std::wstring workDir = workRoot + L"\\" + Utf8ToWide(profileName);
        Core::FileManager::CreateDirRecursive(workDir);
        std::wstring installerPath = workDir + L"\\" + Utf8ToWide(profileName) + L"-installer.jar";

        auto cleanup = [&]() {
            Core::FileManager::RemoveDirectoryRecursive(workDir);
        };

        // --- 下载安装器 ---
        std::string canonicalUrl = isNeoForge ? NeoForgeInstallerUrl(mc, loaderVer)
                                              : ForgeInstallerUrl(mc, loaderVer);
        std::vector<std::string> official;
        if (!downloadUrl.empty()) official.push_back(downloadUrl);
        if (canonicalUrl != downloadUrl) official.push_back(canonicalUrl);

        // 安装器在 maven 仓库中的相对路径（用于 BMCLAPI 镜像）
        std::string installerMavenPath = isNeoForge
            ? (mc == "1.20.1"
                   ? "net/neoforged/forge/" + mc + "-" + loaderVer +
                         "/forge-" + mc + "-" + loaderVer + "-installer.jar"
                   : "net/neoforged/neoforge/" + loaderVer +
                         "/neoforge-" + loaderVer + "-installer.jar")
            : "net/minecraftforge/forge/" + mc + "-" + loaderVer +
                  "/forge-" + mc + "-" + loaderVer + "-installer.jar";
        std::vector<std::string> installerMirror = {
            std::string(Core::DownloadSourceManager::Bmclapi()) + "/maven/" + installerMavenPath
        };
        std::vector<std::string> installerUrls = Core::DownloadSourceManager::ArrangeFile(official, installerMirror);

        if (!DownloadToFile(installerUrls, installerPath)) {
            cleanup();
            return {{"ok", false}, {"error", "下载 " + loaderName + " 安装器失败，请检查网络连接"}};
        }

        // --- 解析安装器内的 JSON ---
        if (!ExtractZipSel(installerPath, workDir, {"install_profile.json", "version.json"})) {
            cleanup();
            return {{"ok", false}, {"error", "解压 " + loaderName + " 安装器失败"}};
        }

        std::string profileText = ReadTextFile(workDir + L"\\install_profile.json");
        if (profileText.empty()) {
            cleanup();
            return {{"ok", false}, {"error", loaderName + " 安装器缺少 install_profile.json"}};
        }

        nlohmann::json installProfile;
        try {
            installProfile = nlohmann::json::parse(profileText);
        } catch (...) {
            cleanup();
            return {{"ok", false}, {"error", "解析 install_profile.json 失败"}};
        }

        std::string versionText = ReadTextFile(workDir + L"\\version.json");
        nlohmann::json versionJson;
        if (!versionText.empty()) {
            try {
                versionJson = nlohmann::json::parse(versionText);
            } catch (...) {
            }
        }

        nlohmann::json finalVersion;
        bool hasProcessors = installProfile.contains("processors") &&
                             installProfile["processors"].is_array() &&
                             !installProfile["processors"].empty();

        if (versionJson.is_object() && !versionJson.empty()) {
            finalVersion = versionJson;
        } else if (installProfile.contains("versionInfo") && installProfile["versionInfo"].is_object()) {
            finalVersion = installProfile["versionInfo"];
        } else if (installProfile.contains("json") && installProfile["json"].is_string()) {
            // 旧版安装器：version json 位于 jar 内
            std::string entry = installProfile["json"].get<std::string>();
            while (!entry.empty() && entry.front() == '/') entry.erase(entry.begin());
            if (!ExtractZipSel(installerPath, workDir, {entry})) {
                cleanup();
                return {{"ok", false}, {"error", "解压 " + loaderName + " 版本信息失败"}};
            }
            std::string local = entry;
            std::replace(local.begin(), local.end(), '/', '\\');
            std::string text = ReadTextFile(workDir + L"\\" + Utf8ToWide(local));
            try {
                finalVersion = nlohmann::json::parse(text);
            } catch (...) {
            }
        }

        if (!finalVersion.is_object() || finalVersion.empty()) {
            cleanup();
            return {{"ok", false}, {"error", "无法解析 " + loaderName + " 版本信息"}};
        }

        finalVersion["id"] = profileName;
        if (!finalVersion.contains("inheritsFrom") || !finalVersion["inheritsFrom"].is_string()) {
            finalVersion["inheritsFrom"] = mc;
        }

        if (!WriteVersionJson(profileName, finalVersion)) {
            cleanup();
            return {{"ok", false}, {"error", "写入版本 JSON 失败"}};
        }

        // --- 旧版安装器：把 jar 内 maven/ 目录释放到 libraries ---
        if (!hasProcessors && installProfile.contains("json")) {
            ExtractZipSel(installerPath, LibsBaseDir(), {}, "maven/", true);
        }
        if (installProfile.contains("install") && installProfile["install"].is_object()) {
            std::string filePath = installProfile["install"].value("filePath", "");
            std::string libCoord = installProfile["install"].value("path", "");
            if (!filePath.empty() && !libCoord.empty()) {
                ExtractZipSel(installerPath, workDir, {filePath});
                std::string body = ReadTextFile(workDir + L"\\" + Utf8ToWide(filePath));
                Coord c = ParseCoord(libCoord);
                std::string local = BuildCoordPath(c);
                std::replace(local.begin(), local.end(), '/', '\\');
                if (!body.empty()) {
                    Core::FileManager::SaveFile(LibsBaseDir() + L"\\" + Utf8ToWide(local), body);
                }
            }
        }

        // --- 坐标 -> 本地路径映射 ---
        std::map<std::string, std::wstring> coordToPath;
        auto registerLibs = [&](const nlohmann::json& libs) {
            if (!libs.is_array()) return;
            for (const auto& lib : libs) {
                std::string name = lib.value("name", "");
                if (name.empty()) continue;
                LibFile f;
                std::string mavenPath;
                if (ExtractLibFile(lib, f) && !f.mavenPath.empty()) {
                    mavenPath = f.mavenPath;
                } else {
                    mavenPath = BuildCoordPath(ParseCoord(name));
                }
                std::string local = mavenPath;
                std::replace(local.begin(), local.end(), '/', '\\');
                coordToPath[ParseCoord(name).key] = LibsBaseDir() + L"\\" + Utf8ToWide(local);
            }
        };
        registerLibs(installProfile.value("libraries", nlohmann::json::array()));
        registerLibs(finalVersion.value("libraries", nlohmann::json::array()));

        // --- 解析 install_profile.data ---
        // 必须放在下载之前：这些取值本身就是 processors 的输出路径
        // （PATCHED / MC_SLIM / MC_OFF / MOJMAPS ...），它们不是可下载物。
        ProcContext ctx;
        ctx.mcRoot = MinecraftDir();
        ctx.installerPath = installerPath;
        ctx.minecraftJar = vanillaJar;
        ctx.mcVersion = mc;
        ctx.coordToPath = &coordToPath;

        std::map<std::string, std::wstring> dataMap;
        ctx.dataMap = &dataMap;

        // 第一遍：解析非引用型取值
        std::vector<std::pair<std::string, std::string>> pendingRefs;
        if (installProfile.contains("data") && installProfile["data"].is_object()) {
            for (auto it = installProfile["data"].begin(); it != installProfile["data"].end(); ++it) {
                const nlohmann::json& v = it.value();
                std::string raw;
                if (v.is_object()) {
                    raw = v.value("client", v.value("server", ""));
                } else if (v.is_string()) {
                    raw = v.get<std::string>();
                }
                if (raw.empty()) continue;

                bool isRef = raw.size() >= 2 && raw.front() == '{' && raw.back() == '}';
                if (isRef) {
                    pendingRefs.emplace_back(it.key(), raw);
                } else {
                    dataMap[it.key()] = ResolveDataValue(raw, ctx, workDir);
                }
            }
            // 第二遍：解析形如 {KEY} 的引用
            for (const auto& ref : pendingRefs) {
                std::string key = ref.second.substr(1, ref.second.size() - 2);
                if (key == "SIDE") {
                    dataMap[ref.first] = L"client";
                } else if (key == "MINECRAFT_JAR") {
                    dataMap[ref.first] = vanillaJar;
                } else if (key == "ROOT") {
                    dataMap[ref.first] = ctx.mcRoot;
                } else if (key == "INSTALLER") {
                    dataMap[ref.first] = installerPath;
                } else {
                    auto found = dataMap.find(key);
                    if (found != dataMap.end()) dataMap[ref.first] = found->second;
                }
            }
        }

        // processor 输出路径（位于 libraries 下的）不参与下载，改由 processors 生成；
        // 顺便把它们所在目录先建出来。
        std::set<std::wstring> generatedPaths;
        std::wstring libsBase = LibsBaseDir();
        for (const auto& kv : dataMap) {
            const std::wstring& value = kv.second;
            if (value.size() > libsBase.size() && value.rfind(libsBase, 0) == 0) {
                generatedPaths.insert(value);
                size_t pos = value.find_last_of(L'\\');
                if (pos != std::wstring::npos && pos > 0) {
                    Core::FileManager::CreateDirRecursive(value.substr(0, pos));
                }
            }
        }

        // --- 下载支持库（版本库 + 安装器库） ---
        WriteInstallLog(loaderName + " " + mc + "-" + loaderVer + " 开始下载支持库 (profile=" + profileName + ")");
        std::wstring dlError;
        if (!DownloadLibraries(finalVersion, dlError, &generatedPaths)) {
            WriteInstallLog("失败: 版本支持库下载失败 -> " + WideToUtf8(dlError));
            cleanup();
            return {{"ok", false}, {"error", "下载 " + loaderName + " 支持库失败: " + WideToUtf8(dlError)}};
        }
        {
            nlohmann::json wrapper = {{"libraries", installProfile.value("libraries", nlohmann::json::array())}};
            if (!DownloadLibraries(wrapper, dlError, &generatedPaths)) {
                WriteInstallLog("失败: 安装器依赖下载失败 -> " + WideToUtf8(dlError));
                cleanup();
                return {{"ok", false}, {"error", "下载 " + loaderName + " 安装依赖失败: " + WideToUtf8(dlError)}};
            }
        }
        WriteInstallLog("支持库下载完成");

        // --- 执行 processors（仅客户端） ---
        if (hasProcessors) {
            int procIndex = 0;
            for (const auto& proc : installProfile["processors"]) {
                procIndex++;
                if (!proc.is_object()) continue;

                // 侧过滤：sides 缺失或为空表示客户端/服务端都执行，否则必须包含 client
                if (proc.contains("sides") && proc["sides"].is_array() && !proc["sides"].empty()) {
                    bool clientSide = false;
                    for (const auto& s : proc["sides"]) {
                        if (s.is_string() && s.get<std::string>() == "client") clientSide = true;
                    }
                    if (!clientSide) continue;
                }

                std::string jarCoord = proc.value("jar", "");
                if (jarCoord.empty()) continue;

                std::vector<std::wstring> cp;
                if (proc.contains("classpath") && proc["classpath"].is_array()) {
                    for (const auto& c : proc["classpath"]) {
                        if (c.is_string()) cp.push_back(ResolveCoordLocal(c.get<std::string>(), coordToPath));
                    }
                }
                std::wstring jarPath = ResolveCoordLocal(jarCoord, coordToPath);
                cp.push_back(jarPath);

                std::wstring manifestTmp = workDir + L"\\manifest_" + Utf8ToWide(std::to_string(procIndex));
                std::string mainClass = ReadJarMainClass(jarPath, manifestTmp);
                if (mainClass.empty()) {
                    cleanup();
                    return {{"ok", false},
                            {"error", loaderName + " 处理器 " + jarCoord + " 缺少 Main-Class，安装器可能已损坏"}};
                }

                std::wstring classpathJoined;
                for (size_t i = 0; i < cp.size(); i++) {
                    if (i > 0) classpathJoined += L";";
                    classpathJoined += cp[i];
                }

                std::wstring args;
                if (proc.contains("args") && proc["args"].is_array()) {
                    for (const auto& a : proc["args"]) {
                        if (!a.is_string()) continue;
                        args += L" \"" + ResolveProcessorArg(a.get<std::string>(), ctx) + L"\"";
                    }
                }

                std::wstring cmd = L"\"" + Utf8ToWide(javaPath) + L"\" -cp \"" + classpathJoined + L"\" " +
                                   Utf8ToWide(mainClass) + args;

                std::string output;
                int exitCode = RunProcessCapture(cmd, 900000, ctx.mcRoot, &output);
                if (exitCode != 0) {
                    std::string reason = exitCode == -2 ? "执行超时" : "退出码 " + std::to_string(exitCode);
                    cleanup();
                    return {{"ok", false},
                            {"error", loaderName + " 处理器执行失败 (" + reason + "): " + TailLines(output, 6)}};
                }
            }
        }

        cleanup();

        return {{"ok", true},
                {"libraries", nlohmann::json::array()},
                {"versionId", profileName},
                {"mainClass", finalVersion.value("mainClass", "")}};
    }

    // 安装入口
    nlohmann::json HandleInstall(const nlohmann::json& args) {
        std::string loaderType = args.value("loaderType", "");
        std::string loaderVersion = args.value("loaderVersion", "");
        std::string downloadUrl = args.value("downloadUrl", "");
        std::string gameVersion = args.value("gameVersion", "");
        std::string profileName = args.value("profileName", "");

        if (loaderType.empty() || loaderVersion.empty() || gameVersion.empty()) {
            return {{"ok", false}, {"error", "缺少 loaderType / loaderVersion / gameVersion 参数"}};
        }
        if (profileName.empty()) {
            profileName = gameVersion + "-" + loaderType + "-" + loaderVersion;
        }

        if (loaderType == "fabric") return InstallMetaProfile(false, gameVersion, loaderVersion, profileName);
        if (loaderType == "quilt") return InstallMetaProfile(true, gameVersion, loaderVersion, profileName);
        if (loaderType == "forge" || loaderType == "neoforge") {
            return InstallForgelike(loaderType == "neoforge", gameVersion, loaderVersion, downloadUrl, profileName);
        }
        return {{"ok", false}, {"error", "不支持的加载器类型: " + loaderType}};
    }
};

} // namespace Services
