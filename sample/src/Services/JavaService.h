#pragma once
#include <string>
#include <cctype>
#include <fstream>
#include <set>
#include <vector>
#include <mutex>
#include <thread>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/StringUtils.h"
#include "Core/FileManager.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

class JavaService {
public:
    JavaService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    // java.scan 要枚举所有磁盘的 Program Files、把 %PATH% 逐个试一遍，
    // 还对每个候选跑一次 `java -version`（每次都起一个进程、最长等 5 秒），
    // 轻松跑好几秒。Bridge 的后台工作线程是所有命令共用的**单线程**
    // （src/bridge.cpp 的 WorkerLoop），直接在 handler 里扫会把其它命令全堵住 ——
    // 表现就是扫描期间切页面一律卡在"加载中"。所以改成"丢独立线程 + 前端轮询"。
    struct ScanState {
        std::mutex mtx;
        bool scanning = false;
        long long pendingRequestId = 0;  // 正在跑的请求号
        long long resultRequestId = 0;   // 已完成的结果对应的请求号
        nlohmann::json result;
        long long nextRequestId = 1;
    };

    static ScanState& State() {
        static ScanState state;
        return state;
    }

    void RegisterCommands() {
        // java.scan - 异步扫描 Java 安装；立即返回请求号，结果用 java.poll 取
        bridge_.RegisterCommand("java.scan", [](const nlohmann::json&) -> nlohmann::json {
            ScanState& state = State();
            long long requestId = 0;
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                if (state.scanning) {
                    // 已经有一次扫描在跑就复用它：反复进出设置页不该堆出一串全盘扫描
                    return {{"ok", true}, {"async", true}, {"requestId", state.pendingRequestId}};
                }
                requestId = state.nextRequestId++;
                state.scanning = true;
                state.pendingRequestId = requestId;
            }

            std::thread([requestId]() {
                // 必须保证走到最后把 scanning 清掉：否则一次异常就让
                // 后面所有 java.scan 都去复用那次永远不完成的扫描，界面永久卡在"正在扫描"
                nlohmann::json result;
                try {
                    result = HandleJavaScan();
                } catch (...) {
                    result = {{"ok", false}, {"error", "Java scan failed"}};
                }

                ScanState& state = State();
                std::lock_guard<std::mutex> lock(state.mtx);
                state.scanning = false;
                state.result = result;
                state.resultRequestId = requestId;
            }).detach();

            return {{"ok", true}, {"async", true}, {"requestId", requestId}};
        });

        // java.poll - 按请求号取扫描结果
        bridge_.RegisterCommand("java.poll", [](const nlohmann::json& args) -> nlohmann::json {
            long long requestId = args.value("requestId", (long long)0);
            ScanState& state = State();
            std::lock_guard<std::mutex> lock(state.mtx);
            if (requestId == 0 || state.resultRequestId != requestId || state.result.is_null()) {
                return {{"ok", true}, {"hasResult", false}};
            }
            return {{"ok", true}, {"hasResult", true}, {"data", state.result}};
        });

        // java.select - 选择 Java；auto 为 true 表示交给自动选择
        bridge_.RegisterCommand("java.select", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleJavaSelect(args);
        });
    }

    // 运行 java -version 获取版本号
    static std::string GetJavaVersion(const std::wstring& javaPath) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        HANDLE hReadPipe, hWritePipe;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + javaPath + L"\" -version";

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return "";
        }

        CloseHandle(hWritePipe);

        std::string output;
        char buf[1024] = {0};
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            output += buf;
        }
        CloseHandle(hReadPipe);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        size_t pos = output.find("version \"");
        if (pos == std::string::npos) return "";
        pos += 9;
        size_t end = output.find("\"", pos);
        if (end == std::string::npos) return "";
        return output.substr(pos, end - pos);
    }

    // 版本号归一化到主版本："21.0.2" → 21，"1.8.0_402" → 8
    static int JavaMajorFromVersion(const std::string& version) {
        size_t i = 0;
        int major = 0;
        while (i < version.size() && std::isdigit((unsigned char)version[i])) {
            major = major * 10 + (version[i++] - '0');
        }
        if (i == 0) return 0;
        if (major != 1) return major;

        // Java 8 及更早的报告形式是 1.x，主版本要取第二段
        if (i < version.size() && version[i] == '.') i++;
        int minor = 0;
        size_t start = i;
        while (i < version.size() && std::isdigit((unsigned char)version[i])) {
            minor = minor * 10 + (version[i++] - '0');
        }
        return i > start ? minor : 1;
    }

    // 已知的 JDK 发行方目录名（位于 Program Files / Program Files (x86) / %LOCALAPPDATA%\Programs 下）。
    // 只扫这些目录，不再全盘遍历 Program Files —— 那里散落着大量应用自带的 JRE，
    // 会把下拉框塞满无关项。IDE 自带的运行时（JetBrains / Android Studio）同理不收。
    static constexpr const wchar_t* kJdkVendorDirs[] = {
        L"Java",              // Oracle JDK / JRE
        L"JavaSoft",
        L"Eclipse Adoptium",  // Temurin
        L"Eclipse Foundation",
        L"AdoptOpenJDK",
        L"Temurin",
        L"Amazon Corretto",
        L"Zulu",              // Azul
        L"Microsoft",         // Microsoft Build of OpenJDK
        L"BellSoft",          // Liberica
        L"Semeru",
        L"IBM",
        L"RedHat",
        L"SapMachine",
        L"OpenJDK"
    };

    // PATH 常常远超 MAX_PATH，缓冲给足，否则长 PATH 会被整体丢弃
    static std::wstring EnvVar(const wchar_t* name) {
        std::wstring buffer(32768, L'\0');
        DWORD length = GetEnvironmentVariableW(name, buffer.data(), (DWORD)buffer.size());
        if (length == 0 || length >= buffer.size()) return std::wstring();
        buffer.resize(length);
        return buffer;
    }

    // 所有本地磁盘的根目录，形如 "C:"、"D:"。光驱 / 网络盘 / 可移动盘不参与扫描
    static std::vector<std::wstring> FixedDriveRoots() {
        std::vector<std::wstring> roots;
        DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; i++) {
            if (!(mask & (1u << i))) continue;

            std::wstring root;
            root += static_cast<wchar_t>(L'A' + i);
            root += L":";
            if (GetDriveTypeW((root + L"\\").c_str()) != DRIVE_FIXED) continue;

            roots.push_back(root);
        }
        return roots;
    }

    // 系统 PATH 里 ...\jdk-17\bin 这类直接指向 java.exe 的目录
    static void ScanPathEntries(nlohmann::json& results, std::set<std::wstring>& seen) {
        std::wstring path = EnvVar(L"PATH");
        if (path.empty()) return;

        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find(L';', start);
            if (end == std::wstring::npos) end = path.size();

            std::wstring entry = path.substr(start, end - start);
            // PATH 条目常被引号包裹，末尾还可能带空格或反斜杠
            while (!entry.empty() && (entry.front() == L'"' || entry.front() == L' ')) entry.erase(entry.begin());
            while (!entry.empty() && (entry.back() == L'"' || entry.back() == L' ' || entry.back() == L'\\')) entry.pop_back();
            // PATH 指向的就是 java.exe 所在目录，不再往下找 bin
            if (!entry.empty()) RecordJavaExe(entry + L"\\java.exe", results, seen);

            if (end >= path.size()) break;
            start = end + 1;
        }
    }

    static std::wstring LowerAscii(std::wstring value) {
        for (wchar_t& c : value) {
            if (c >= L'A' && c <= L'Z') c = c - L'A' + L'a';
        }
        return value;
    }

    static std::string LowerAscii(std::string value) {
        for (char& c : value) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return value;
    }

    // 判断这条路径来自 JRE 而不是 JDK（...\jre\bin 或 ...\jre1.8.0_202\bin）
    static bool IsJreInstall(const std::string& javaPath) {
        std::string lower = LowerAscii(javaPath);
        size_t binPos = lower.rfind("\\bin\\java.exe");
        if (binPos == std::string::npos || binPos == 0) return false;

        size_t nameStart = lower.rfind('\\', binPos - 1);
        std::string folder = lower.substr(nameStart + 1, binPos - nameStart - 1);
        return folder == "jre" || folder.rfind("jre", 0) == 0;
    }

    // java.exe 的最终路径：软链接 / 联结点指向同一份安装时用作去重键
    static std::wstring CanonicalJavaPath(const std::wstring& javaPath) {
        HANDLE handle = CreateFileW(javaPath.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return javaPath;

        WCHAR buffer[MAX_PATH * 2] = {0};
        DWORD length = GetFinalPathNameByHandleW(handle, buffer, MAX_PATH * 2,
                                                 FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        CloseHandle(handle);
        if (length == 0 || length >= MAX_PATH * 2) return javaPath;

        std::wstring result(buffer, length);
        const std::wstring devicePrefix = L"\\\\?\\";
        if (result.rfind(devicePrefix, 0) == 0) result = result.substr(devicePrefix.size());
        return result;
    }

    // Oracle 的 javapath / java8path 以及 Windows 的 system32 放的是跳转壳，
    // 它们指向的 Java 会被真正的安装目录扫到，收了只会让下拉框出现重复项
    static bool IsShimJavaDir(std::wstring dir) {
        dir = LowerAscii(dir);
        if (dir.find(L"\\system32") != std::wstring::npos) return true;
        if (dir.find(L"\\javatmp") != std::wstring::npos) return true;

        size_t pos = dir.find_last_of(L'\\');
        std::wstring name = pos == std::wstring::npos ? dir : dir.substr(pos + 1);
        return name == L"javapath" || name == L"java8path" ||
               name.rfind(L"javapath_target_", 0) == 0 ||
               name.rfind(L"java8path_target_", 0) == 0;
    }

    // 记录一个 java.exe；软链接 / 联结点解析后指向同一份安装的（例如同一份 Java
    // 同时被 PATH 与 Program Files 扫到）只保留第一次命中的那条
    static bool RecordJavaExe(const std::wstring& javaPath, nlohmann::json& results,
                             std::set<std::wstring>& seen) {
        if (GetFileAttributesW(javaPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

        size_t slash = javaPath.find_last_of(L'\\');
        if (slash != std::wstring::npos && IsShimJavaDir(javaPath.substr(0, slash))) return false;

        // 去重键和显示路径都用规范路径，避免同一份 Java 出现多条
        std::wstring realPath = CanonicalJavaPath(javaPath);
        if (!seen.insert(LowerAscii(realPath)).second) return true;

        // 版本目录名：...\jdk-17\bin\java.exe -> jdk-17
        std::wstring folder = realPath;
        size_t pos = folder.find_last_of(L'\\');
        if (pos != std::wstring::npos) folder = folder.substr(0, pos);

        size_t namePos = folder.find_last_of(L'\\');
        std::wstring folderName = namePos == std::wstring::npos ? folder : folder.substr(namePos + 1);
        if (LowerAscii(folderName) == L"bin" && namePos != std::wstring::npos) {
            std::wstring parent = folder.substr(0, namePos);
            size_t parentPos = parent.find_last_of(L'\\');
            folderName = parentPos == std::wstring::npos ? parent : parent.substr(parentPos + 1);
        }

        std::string version = GetJavaVersion(realPath);
        results.push_back({
            {"path", Core::StringUtils::WideToUtf8(realPath)},
            {"name", Core::StringUtils::WideToUtf8(folderName)},
            {"version", version},
            {"major", JavaMajorFromVersion(version)}
        });
        return true;
    }

    // 检查目录下是否有 java.exe
    static bool CheckJavaInDir(const std::wstring& dir, nlohmann::json& results,
                              std::set<std::wstring>& seen) {
        return RecordJavaExe(dir + L"\\bin\\java.exe", results, seen);
    }

    // 扫描指定目录下的 java.exe（发行方目录下最多再下探两层）
    static void ScanJavaInDir(const std::wstring& dir, nlohmann::json& results,
                             std::set<std::wstring>& seen) {
        if (CheckJavaInDir(dir, results, seen)) return;

        WIN32_FIND_DATAW findData;
        std::wstring searchPath = dir + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring name = findData.cFileName;
                if (name == L"." || name == L"..") continue;

                std::wstring subDir = dir + L"\\" + name;
                if (CheckJavaInDir(subDir, results, seen)) continue;

                std::wstring searchPath2 = subDir + L"\\*";
                WIN32_FIND_DATAW findData2;
                HANDLE hFind2 = FindFirstFileW(searchPath2.c_str(), &findData2);
                if (hFind2 != INVALID_HANDLE_VALUE) {
                    do {
                        if (findData2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            std::wstring name2 = findData2.cFileName;
                            if (name2 == L"." || name2 == L"..") continue;
                            std::wstring subDir2 = subDir + L"\\" + name2;
                            CheckJavaInDir(subDir2, results, seen);
                        }
                    } while (FindNextFileW(hFind2, &findData2));
                    FindClose(hFind2);
                }
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    // 扫描 <base> 下各个发行方目录
    static void ScanJavaVendors(const std::wstring& base, nlohmann::json& results,
                               std::set<std::wstring>& seen) {
        if (base.empty()) return;
        for (const wchar_t* vendor : kJdkVendorDirs) {
            ScanJavaInDir(base + L"\\" + vendor, results, seen);
        }
    }

    // 扫描已知的 Java 安装位置，主版本高的排在前面
    static void ScanAllJava(nlohmann::json& results) {
        std::set<std::wstring> seen;
        // 同一目录可能同时来自环境变量与磁盘枚举，记下来只遍历一次
        std::set<std::wstring> visitedBases;

        auto scanBase = [&](const std::wstring& base) {
            if (base.empty()) return;
            if (!visitedBases.insert(LowerAscii(base)).second) return;
            ScanJavaVendors(base, results, seen);
        };

        // 程序目录：环境变量给的是系统盘，其余盘靠枚举补齐
        scanBase(EnvVar(L"ProgramFiles"));
        scanBase(EnvVar(L"ProgramFiles(x86)"));
        for (const std::wstring& root : FixedDriveRoots()) {
            scanBase(root + L"\\Program Files");
            scanBase(root + L"\\Program Files (x86)");
        }

        // 每用户安装：Eclipse Adoptium 等安装器默认装在这里
        std::wstring localAppData = EnvVar(L"LOCALAPPDATA");
        if (!localAppData.empty()) {
            scanBase(localAppData + L"\\Programs");
        }

        // IntelliJ IDEA 等 IDE 下载的 JDK
        std::wstring userProfile = EnvVar(L"USERPROFILE");
        if (!userProfile.empty()) {
            ScanJavaInDir(userProfile + L"\\.jdks", results, seen);
        }

        // 自定义安装位置的兜底
        std::wstring javaHome = EnvVar(L"JAVA_HOME");
        if (!javaHome.empty()) {
            CheckJavaInDir(javaHome, results, seen);
        }

        // 系统 PATH
        ScanPathEntries(results, seen);

        std::sort(results.begin(), results.end(),
            [](const nlohmann::json& a, const nlohmann::json& b) {
                int majorA = a.value("major", 0);
                int majorB = b.value("major", 0);
                if (majorA != majorB) return majorA > majorB;
                return a.value("path", "") < b.value("path", "");
            });

        // 同一版本同时装着 JDK 和 JRE 时只留 JDK：两者跑游戏完全等价，JRE 是 JDK 的子集
        std::set<std::string> versionsWithJdk;
        for (const nlohmann::json& item : results) {
            std::string version = item.value("version", "");
            if (!version.empty() && !IsJreInstall(item.value("path", ""))) {
                versionsWithJdk.insert(version);
            }
        }

        nlohmann::json kept = nlohmann::json::array();
        for (const nlohmann::json& item : results) {
            std::string version = item.value("version", "");
            if (!version.empty() && IsJreInstall(item.value("path", "")) &&
                versionsWithJdk.count(version) > 0) {
                continue;
            }
            kept.push_back(item);
        }
        results = kept;
    }

    // ---- 最低 Java 要求 ----
    // Mojang 对最低 Java 版本的三次上调：
    //   1.17（快照 21w19a 起）→ 16
    //   1.18（快照 21w37a 起）→ 17
    //   1.20.5（快照 24w14a 起）→ 21
    // 更早的版本统一按 Java 8。
    struct JavaRequirement {
        int major;
        int release[3];   // 正式版边界：主.次.补丁
        int snapshot[3];  // 快照边界：年, 周, 字母序号（a = 1）
    };

    // 解析版本 id：正式版 "1.21.4"、快照 "24w14a"；
    // "-pre1" / "-rc1" / "-forge-51.0.1" 之类的后缀一律忽略
    static bool ParseVersionId(const std::string& id, bool& isSnapshot, int key[3]) {
        std::string s = id.substr(0, id.find('-'));
        if (s.empty()) return false;

        size_t w = s.find_first_of("wW");
        if (w != std::string::npos) {
            size_t i = 0;
            int year = 0;
            while (i < w && std::isdigit((unsigned char)s[i])) year = year * 10 + (s[i++] - '0');
            if (i != w) return false;

            i = w + 1;
            size_t weekStart = i;
            int week = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) week = week * 10 + (s[i++] - '0');
            if (i == weekStart) return false;

            int letter = 0;
            if (i < s.size() && std::isalpha((unsigned char)s[i])) {
                letter = std::tolower((unsigned char)s[i]) - 'a' + 1;
            }

            isSnapshot = true;
            key[0] = year;
            key[1] = week;
            key[2] = letter;
            return true;
        }

        isSnapshot = false;
        key[0] = key[1] = key[2] = 0;
        int part = 0;
        size_t i = 0;
        while (i < s.size() && part < 3) {
            size_t start = i;
            int value = 0;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) value = value * 10 + (s[i++] - '0');
            if (i == start) break;
            key[part++] = value;
            if (i < s.size() && s[i] == '.') i++;
            else break;
        }
        return part > 0;
    }

    static bool KeyAtLeast(const int a[3], const int b[3]) {
        for (int i = 0; i < 3; i++) {
            if (a[i] != b[i]) return a[i] > b[i];
        }
        return true;
    }

    // 按版本号推算最低 Java 主版本，边界表从高到低命中即返回
    static int RequiredJavaMajorFor(const std::string& versionId) {
        static const JavaRequirement kRequirements[] = {
            {21, {1, 20, 5}, {24, 14, 1}},
            {17, {1, 18, 0}, {21, 37, 1}},
            {16, {1, 17, 0}, {21, 19, 1}}
        };
        static const int kLegacyMajor = 8;

        bool isSnapshot = false;
        int key[3] = {0, 0, 0};
        if (!ParseVersionId(versionId, isSnapshot, key)) return kLegacyMajor;

        for (const JavaRequirement& req : kRequirements) {
            if (KeyAtLeast(key, isSnapshot ? req.snapshot : req.release)) return req.major;
        }
        return kLegacyMajor;
    }

    // 读取 .minecraft/versions/<name>/<name>.json
    static bool ReadVersionJson(const std::string& name, nlohmann::json& out) {
        if (name.empty()) return false;

        std::wstring dir = Core::FileManager::GetExeDir() + L"\\.minecraft\\versions\\"
            + Core::StringUtils::Utf8ToWide(name);
        std::ifstream ifs(dir + L"\\" + Core::StringUtils::Utf8ToWide(name) + L".json");
        if (!ifs.is_open()) return false;

        try {
            ifs >> out;
        } catch (...) {
            return false;
        }
        return true;
    }

    // 推算当前选中版本的最低 Java 要求：优先取版本清单里的 javaVersion.majorVersion，
    // 它最权威且对快照、未来版本自动生效；模组加载器的 profile 没有这个字段，
    // 需要沿 inheritsFrom 回到原版清单；都拿不到才按版本号查表。
    static int ResolveRequiredJavaMajor(const std::string& profileName) {
        std::string versionId = profileName;
        std::string current = profileName;

        for (int depth = 0; depth < 3 && !current.empty(); depth++) {
            nlohmann::json versionJson;
            if (!ReadVersionJson(current, versionJson)) break;

            if (versionJson.contains("javaVersion") && versionJson["javaVersion"].is_object()) {
                int major = versionJson["javaVersion"].value("majorVersion", 0);
                if (major > 0) return major;
            }
            versionId = versionJson.value("id", versionId);
            current = versionJson.value("inheritsFrom", "");
        }
        return RequiredJavaMajorFor(versionId);
    }

    // 挑最合适的 Java：满足最低要求的前提下取版本最低的一个（够用即可，
    // 过高的 Java 反而会让旧整合包出问题）；一个都不满足就退回现有最高版本。
    static nlohmann::json PickAutoJava(const nlohmann::json& javas, int requiredMajor) {
        nlohmann::json best;
        nlohmann::json fallback;

        for (const nlohmann::json& java : javas) {
            int major = java.value("major", 0);
            if (major >= requiredMajor && (best.is_null() || major < best.value("major", 0))) {
                best = java;
            }
            if (fallback.is_null() || major > fallback.value("major", 0)) {
                fallback = java;
            }
        }
        return best.is_null() ? fallback : best;
    }

    static nlohmann::json ReadConfig() {
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            config = nlohmann::json::object();
        }
        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = nlohmann::json::object();
        }
        return config;
    }

    // 前端 bridge: 扫描 Java，并附带当前选择与自动选择结果
    static nlohmann::json HandleJavaScan() {
        nlohmann::json javas = nlohmann::json::array();
        ScanAllJava(javas);

        nlohmann::json config = ReadConfig();
        std::string selectedVersion = config["game"].value("selectedVersion", "");
        std::string selectedPath = config["game"].value("javaPath", "");
        // 老配置没有 javaAuto 字段：没手动选过路径就当作自动
        bool autoSelect = config["game"].value("javaAuto", selectedPath.empty());

        int requiredMajor = ResolveRequiredJavaMajor(selectedVersion);
        nlohmann::json autoJava = PickAutoJava(javas, requiredMajor);

        return {
            {"ok", true},
            {"javas", javas},
            {"selectedVersion", selectedVersion},
            {"selectedPath", selectedPath},
            {"auto", autoSelect},
            {"requiredMajor", requiredMajor},
            {"autoPath", autoJava.is_null() ? "" : autoJava.value("path", "")},
            {"autoMajor", autoJava.is_null() ? 0 : autoJava.value("major", 0)}
        };
    }

    // 前端 bridge: 选择 Java；auto 为 true 时由前端把自动挑选的结果一并传回
    static nlohmann::json HandleJavaSelect(const nlohmann::json& args) {
        nlohmann::json config = ReadConfig();
        config["game"]["javaAuto"] = args.value("auto", false);
        config["game"]["javaPath"] = args.value("path", "");

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
