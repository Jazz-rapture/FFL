#pragma once
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include "Bridge/WebviewBridge.h"
#include "Core/ConfigManager.h"
#include "Core/CryptoUtils.h"
#include "Core/FileManager.h"
#include "Core/HttpClient.h"
#include "Core/StringUtils.h"
#include "Models/DownloadTypes.h"

namespace Services {

// 联机（内网穿透）服务。
//
// 分工：
//   - 令牌：由用户从 SakuraFrp 个人中心自行获取，密文（DPAPI）存在 config.json，
//     明文只在进程内存里，任何命令都不会把它回给前端。
//   - 隧道 / 节点：走 SakuraFrp API v4 查询（见仓库根目录 openapi.yaml）。
//   - 穿透：由 exe 内嵌的官方 frpc 二进制（RT_RCDATA 资源）在本地拉起，
//     本类负责把它解到临时文件、托管进程并转发日志。
class MultiplayerService {
public:
    static constexpr const char* kApiBase = "https://api.natfrp.com/v4";
    static constexpr const char* kProfileUrl = "https://www.natfrp.com/user/profile";
    static constexpr int kDefaultLocalPort = 25565;
    static constexpr size_t kMaxLogLines = 400;
    // 内嵌 frpc 的资源 ID（见 CMakeLists.txt 生成的 frpc_resource.rc，类型 RT_RCDATA）
    static constexpr int kFrpcResourceId = 1;

    explicit MultiplayerService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

    ~MultiplayerService() { StopProcess(); }

    MultiplayerService(const MultiplayerService&) = delete;
    MultiplayerService& operator=(const MultiplayerService&) = delete;

private:
    // ==================== 配置读写 ====================

    struct Settings {
        std::string token;     // 明文，仅进程内可见
        int localPort = kDefaultLocalPort;
        int tunnelId = 0;
    };

    static nlohmann::json LoadConfig() {
        try {
            return nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    static Settings ReadSettings(const nlohmann::json& config) {
        Settings s;
        if (!config.contains("multiplayer") || !config["multiplayer"].is_object()) return s;
        const auto& mp = config["multiplayer"];
        s.token = Core::CryptoUtils::UnprotectFromBase64(mp.value("token", std::string("")));
        s.localPort = mp.value("localPort", kDefaultLocalPort);
        s.tunnelId = mp.value("tunnelId", 0);
        return s;
    }

    // 只更新传进来的字段，其余保持原样
    static bool PatchSettings(const nlohmann::json& patch) {
        nlohmann::json config = LoadConfig();
        if (!config.contains("multiplayer") || !config["multiplayer"].is_object()) {
            config["multiplayer"] = nlohmann::json::object();
        }
        auto& mp = config["multiplayer"];
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            mp[it.key()] = it.value();
        }
        return Core::ConfigManager::WriteConfigFile(config.dump(4));
    }

    // ==================== frpc 定位 ====================

    // frpc 以 RT_RCDATA 资源（ID 1）内嵌在 exe 里，这是它唯一的来源：
    // 运行时取出字节流写临时文件，之后复用同一份文件（同尺寸即认为是同一份）。
    std::wstring EnsureFrpcExtracted() {
        std::lock_guard<std::mutex> lock(frpcMutex_);
        if (!frpcPath_.empty() && Core::FileManager::FileExists(frpcPath_)) return frpcPath_;
        frpcPath_.clear();

        // 工程没有定义 UNICODE，资源 API 走 A 系列（同 embedded_dll.cpp）
        HMODULE hExe = GetModuleHandleW(nullptr);
        HRSRC hrsrc = FindResourceA(hExe, MAKEINTRESOURCEA(kFrpcResourceId), RT_RCDATA);
        if (!hrsrc) return L"";
        HGLOBAL hGlobal = LoadResource(hExe, hrsrc);
        if (!hGlobal) return L"";
        const DWORD size = SizeofResource(hExe, hrsrc);
        const void* data = LockResource(hGlobal);
        if (!data || size == 0) return L"";

        WCHAR tempDir[MAX_PATH] = {0};
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return L"";
        // 单独建一层目录，避免和别的程序在 %TEMP% 根下撞名
        const std::wstring dir = std::wstring(tempDir) + L"TauriCPP";
        CreateDirectoryW(dir.c_str(), nullptr);
        const std::wstring path = dir + L"\\frpc.exe";

        // 文件已存在且大小一致就直接用：内容本来就是内嵌的同一份，
        // 而且 frpc 正在运行时文件被占用、重写会失败
        if (!FileHasSize(path, size) && !WriteBytes(path, data, size)) return L"";

        frpcPath_ = path;
        return frpcPath_;
    }

    static bool FileHasSize(const std::wstring& path, DWORD size) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
        return info.nFileSizeHigh == 0 && info.nFileSizeLow == size;
    }

    static bool WriteBytes(const std::wstring& path, const void* data, DWORD size) {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const BOOL ok = WriteFile(hFile, data, size, &written, nullptr);
        CloseHandle(hFile);
        return ok && written == size;
    }

    // ==================== API 调用 ====================

    static std::string FormEscape(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (char c : raw) {
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out.push_back(c);
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
                out += buf;
            }
        }
        return out;
    }

    // 统一处理 SakuraFrp 的返回：2xx 视为成功，否则从 {code,msg} 里取错误文案
    static nlohmann::json ParseApiReply(const Models::HttpResult& res) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(res.body);
        } catch (...) {
            return {{"ok", false}, {"error", "响应解析失败 (HTTP " + std::to_string(res.status) + ")"}};
        }
        if (res.status >= 200 && res.status < 300) {
            return {{"ok", true}, {"data", body}};
        }
        std::string msg = body.value("msg", "请求失败");
        int code = body.value("code", res.status);
        return {{"ok", false}, {"error", msg}, {"code", code}, {"status", res.status}};
    }

    static nlohmann::json ApiGet(const std::string& token, const std::string& path) {
        return ParseApiReply(Core::HttpClient::HttpGetBearer(
            Core::StringUtils::Utf8ToWide(std::string(kApiBase) + path), token));
    }

    static nlohmann::json ApiPost(const std::string& token, const std::string& path,
                                  const std::string& form) {
        return ParseApiReply(Core::HttpClient::HttpPostFormBearer(
            Core::StringUtils::Utf8ToWide(std::string(kApiBase) + path), token, form));
    }

    // ==================== 日志 ====================

    void PushLog(const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(logMutex_);
            logs_.push_back(line);
            while (logs_.size() > kMaxLogLines) logs_.pop_front();
        }
        bridge_.Emit("multiplayer.log", {{"line", line}});
    }

    nlohmann::json SnapshotLogs() {
        std::lock_guard<std::mutex> lock(logMutex_);
        return nlohmann::json(logs_);
    }

    void ClearLogs() {
        std::lock_guard<std::mutex> lock(logMutex_);
        logs_.clear();
    }

    // ==================== 进程托管 ====================

    bool IsRunning() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        return running_;
    }

    nlohmann::json EmitState() {
        bool running;
        int tunnelId;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            running = running_;
            tunnelId = currentTunnelId_;
        }
        const std::wstring frpc = EnsureFrpcExtracted();
        nlohmann::json state = {
            {"running", running},
            {"tunnelId", tunnelId},
            {"frpcFound", !frpc.empty()},
            {"frpcPath", frpc.empty() ? "" : Core::StringUtils::WideToUtf8(frpc)},
            {"profileUrl", kProfileUrl}
        };
        bridge_.Emit("multiplayer.state", state);
        return state;
    }

    // 读 frpc 的 stdout/stderr，按行推给前端；进程退出后收尾
    void ReadLoop(HANDLE hRead, HANDLE hProcess) {
        std::string pending;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0) {
            pending.append(buf, n);
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                pending.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) PushLog(line);
            }
            // 极端情况：一行都没有换行又特别长，别让它无限堆积
            if (pending.size() > 8192) {
                PushLog(pending);
                pending.clear();
            }
        }
        if (!pending.empty()) PushLog(pending);

        WaitForSingleObject(hProcess, 3000);
        CloseHandle(hRead);

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            running_ = false;
        }
        PushLog("[联机] frpc 已退出");
        EmitState();
    }

    // 回收已结束的读线程，避免 joinable 线程在下次启动时抛异常
    void ReapReader() {
        if (reader_.joinable()) reader_.join();
    }

    // 进程句柄只在这里关闭，避免读线程与停止流程重复 Close
    void CloseProcessHandle() {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (process_ && process_ != INVALID_HANDLE_VALUE) CloseHandle(process_);
        process_ = nullptr;
        currentTunnelId_ = 0;
    }

    void StopProcess() {
        HANDLE process = nullptr;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            process = process_;
        }
        if (process && process != INVALID_HANDLE_VALUE) {
            TerminateProcess(process, 0);
        }
        ReapReader();
        CloseProcessHandle();
    }

    // ==================== 命令注册 ====================

    void RegisterCommands() {
        // 页面进入时拉一次：配置 + 进程状态 + 历史日志，全部不打网络
        bridge_.RegisterCommand("multiplayer.state", [this](const nlohmann::json&) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            const std::wstring frpc = EnsureFrpcExtracted();
            bool running;
            {
                std::lock_guard<std::mutex> lock(stateMutex_);
                running = running_;
            }
            return {
                {"ok", true},
                {"hasToken", !s.token.empty()},
                {"tokenTail", s.token.size() >= 4 ? s.token.substr(s.token.size() - 4) : ""},
                {"localPort", s.localPort},
                {"tunnelId", s.tunnelId},
                {"running", running},
                {"frpcFound", !frpc.empty()},
                {"frpcPath", frpc.empty() ? "" : Core::StringUtils::WideToUtf8(frpc)},
                {"profileUrl", kProfileUrl},
                {"logs", SnapshotLogs()}
            };
        });

        // 令牌由用户输入；这里先拿它验一次 /user/info，通过了才加密落盘
        bridge_.RegisterCommand("multiplayer.saveToken", [this](const nlohmann::json& args) -> nlohmann::json {
            std::string token = Trim(args.value("token", std::string("")));
            if (token.empty()) return {{"ok", false}, {"error", "令牌不能为空"}};

            nlohmann::json reply = ApiGet(token, "/user/info");
            if (!reply.value("ok", false)) return reply;

            if (!PatchSettings({{"token", Core::CryptoUtils::ProtectToBase64(token)}})) {
                return {{"ok", false}, {"error", "令牌保存失败"}};
            }
            return {{"ok", true}, {"user", reply["data"]}};
        });

        bridge_.RegisterCommand("multiplayer.clearToken", [](const nlohmann::json&) -> nlohmann::json {
            return {{"ok", PatchSettings({{"token", ""}})}};
        });

        bridge_.RegisterCommand("multiplayer.user", [this](const nlohmann::json&) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};
            nlohmann::json reply = ApiGet(s.token, "/user/info");
            if (!reply.value("ok", false)) return reply;
            return {{"ok", true}, {"user", reply["data"]}};
        });

        bridge_.RegisterCommand("multiplayer.tunnels", [this](const nlohmann::json&) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};
            nlohmann::json reply = ApiGet(s.token, "/tunnels");
            if (!reply.value("ok", false)) return reply;
            return {{"ok", true}, {"tunnels", reply["data"]}};
        });

        bridge_.RegisterCommand("multiplayer.nodes", [this](const nlohmann::json&) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};
            nlohmann::json reply = ApiGet(s.token, "/nodes");
            if (!reply.value("ok", false)) return reply;

            // /nodes 返回「节点 ID -> 节点信息」的对象，拍平成带 id 的数组方便前端排序
            nlohmann::json nodes = nlohmann::json::array();
            const auto& data = reply["data"];
            if (data.is_object()) {
                for (auto it = data.begin(); it != data.end(); ++it) {
                    if (!it.value().is_object()) continue;
                    nlohmann::json node = it.value();
                    node["id"] = it.key();
                    nodes.push_back(node);
                }
            }
            std::sort(nodes.begin(), nodes.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                return (int)strtol(a.value("id", std::string("0")).c_str(), nullptr, 10) <
                       (int)strtol(b.value("id", std::string("0")).c_str(), nullptr, 10);
            });
            return {{"ok", true}, {"nodes", nodes}};
        });

        bridge_.RegisterCommand("multiplayer.createTunnel", [this](const nlohmann::json& args) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};

            const std::string name = Trim(args.value("name", std::string("")));
            const int node = args.value("node", 0);
            const int localPort = args.value("localPort", s.localPort);
            const std::string nodeName = args.value("nodeName", std::string(""));

            if (name.empty()) return {{"ok", false}, {"error", "隧道名称不能为空"}};
            if (node <= 0) return {{"ok", false}, {"error", "请选择节点"}};
            if (localPort <= 0 || localPort > 65535) return {{"ok", false}, {"error", "本地端口不合法"}};

            std::string form =
                "name=" + FormEscape(name) +
                "&type=tcp" +
                "&node=" + std::to_string(node) +
                "&local_ip=" + FormEscape(args.value("localIp", std::string("127.0.0.1"))) +
                "&local_port=" + std::to_string(localPort);
            if (!nodeName.empty()) form += "&note=" + FormEscape(nodeName);

            return ApiPost(s.token, "/tunnels", form);
        });

        bridge_.RegisterCommand("multiplayer.deleteTunnel", [this](const nlohmann::json& args) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};
            std::string ids = args.value("ids", std::string(""));
            if (ids.empty()) return {{"ok", false}, {"error", "缺少隧道 ID"}};
            return ApiPost(s.token, "/tunnel/delete", "ids=" + FormEscape(ids));
        });

        // 记住用户当前选中的隧道 / 本地端口，下次进页面直接复用
        bridge_.RegisterCommand("multiplayer.select", [](const nlohmann::json& args) -> nlohmann::json {
            nlohmann::json patch;
            if (args.contains("tunnelId")) patch["tunnelId"] = args.value("tunnelId", 0);
            if (args.contains("localPort")) patch["localPort"] = args.value("localPort", kDefaultLocalPort);
            if (patch.empty()) return {{"ok", false}, {"error", "没有要保存的内容"}};
            return {{"ok", PatchSettings(patch)}};
        });

        // 本地通过 frpc 拉起隧道：必要时先把隧道的本地端口同步到服务端，再启动进程
        bridge_.RegisterCommand("multiplayer.start", [this](const nlohmann::json& args) -> nlohmann::json {
            Settings s = ReadSettings(LoadConfig());
            if (s.token.empty()) return {{"ok", false}, {"error", "尚未设置映射令牌"}};

            const int tunnelId = args.value("tunnelId", s.tunnelId);
            const int localPort = args.value("localPort", s.localPort);
            if (tunnelId <= 0) return {{"ok", false}, {"error", "请选择要映射的隧道"}};
            if (localPort <= 0 || localPort > 65535) return {{"ok", false}, {"error", "本地端口不合法"}};

            const std::wstring frpc = EnsureFrpcExtracted();
            if (frpc.empty()) {
                return {{"ok", false}, {"error", "未找到 frpc 程序，请确认 frpc.exe 与启动器在同一目录"}};
            }
            if (IsRunning()) return {{"ok", false}, {"error", "当前已有映射在运行，请先停止"}};
            ReapReader();          // 上一次自然退出的读线程要收掉
            CloseProcessHandle();  // 以及它留下的过期进程句柄

            // 服务端隧道记录的本地端口如果和当前设置不一致，frpc 会连错端口，先纠正
            nlohmann::json tunnels = ApiGet(s.token, "/tunnels");
            if (tunnels.value("ok", false) && tunnels["data"].is_array()) {
                for (const auto& t : tunnels["data"]) {
                    if (t.value("id", 0) != tunnelId) continue;
                    if (t.value("local_port", 0) != localPort) {
                        ApiPost(s.token, "/tunnel/edit",
                                "id=" + std::to_string(tunnelId) +
                                "&local_port=" + std::to_string(localPort) +
                                "&local_ip=127.0.0.1");
                    }
                    break;
                }
            }

            PatchSettings({{"tunnelId", tunnelId}, {"localPort", localPort}});

            if (!StartFrpc(frpc, s.token, tunnelId)) {
                return {{"ok", false}, {"error", "frpc 启动失败"}};
            }
            return {{"ok", true}, {"running", true}, {"tunnelId", tunnelId}};
        });

        bridge_.RegisterCommand("multiplayer.stop", [this](const nlohmann::json&) -> nlohmann::json {
            if (!IsRunning()) return {{"ok", true}, {"running", false}};
            PushLog("[联机] 正在停止映射...");
            StopProcess();
            EmitState();
            return {{"ok", true}, {"running", false}};
        });
    }

    bool StartFrpc(const std::wstring& frpcPath, const std::string& token, int tunnelId) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hRead = nullptr;
        HANDLE hWrite = nullptr;
        if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return false;
        SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

        HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &sa,
                                  OPEN_EXISTING, 0, nullptr);

        std::wstring cmd = L"\"" + frpcPath + L"\" -f " +
                           Core::StringUtils::Utf8ToWide(token + ":" + std::to_string(tunnelId)) +
                           L" -n --disable_log_color";

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.hStdInput = (hNul == INVALID_HANDLE_VALUE) ? nullptr : hNul;

        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        BOOL created = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr,
                                      Core::FileManager::GetExeDir().c_str(), &si, &pi);
        CloseHandle(hWrite);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

        if (!created) {
            CloseHandle(hRead);
            PushLog("[联机] 无法创建 frpc 进程, 错误码 " + std::to_string(GetLastError()));
            return false;
        }

        CloseHandle(pi.hThread);
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            process_ = pi.hProcess;
            currentTunnelId_ = tunnelId;
            running_ = true;
        }
        ClearLogs();
        PushLog("[联机] frpc 已启动, 隧道 ID " + std::to_string(tunnelId));
        EmitState();

        reader_ = std::thread([this, hRead, hProcess = pi.hProcess]() {
            ReadLoop(hRead, hProcess);
        });
        return true;
    }

    static std::string Trim(const std::string& raw) {
        size_t begin = raw.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) return "";
        size_t end = raw.find_last_not_of(" \t\r\n");
        return raw.substr(begin, end - begin + 1);
    }

    Bridge::WebviewBridge& bridge_;

    std::mutex stateMutex_;
    HANDLE process_ = nullptr;
    bool running_ = false;
    int currentTunnelId_ = 0;
    std::thread reader_;

    std::mutex logMutex_;
    std::deque<std::string> logs_;

    // 内嵌 frpc 解出来的临时文件路径，见 EnsureFrpcExtracted
    std::mutex frpcMutex_;
    std::wstring frpcPath_;
};

} // namespace Services
