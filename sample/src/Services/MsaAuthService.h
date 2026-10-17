#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <ctime>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/CryptoUtils.h"
#include "Core/MsaAuth.h"
#include "Core/HashUtils.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

// 正版登录（微软设备代码流）。
// auth.msa.start 起一个后台线程：申请设备代码 → 轮询授权 → Xbox Live → XSTS →
// Minecraft 令牌 → 检查拥有情况 → 取角色档案，最后连同加密后的令牌写进账号槽位。
// 命令立刻返回，全程用事件回报，不阻塞 WebView 的消息循环。
class MsaAuthService {
public:
    MsaAuthService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

    // 启动游戏前调用：确保槽位的会话可用（快过期就刷新），返回
    // {ok, name, uuid, accessToken, xuid, type}；失败时 {ok:false, error}
    static nlohmann::json EnsureFreshSession(int index) {
        nlohmann::json result = {{"ok", false}};

        nlohmann::json config = LoadConfig();
        Core::ConfigManager::AccountSlot slot = Core::ConfigManager::ReadAccountSlot(config, index);
        if (!slot.used) {
            result["error"] = "没有可用的账号";
            return result;
        }

        if (slot.type != "msa") {
            result["ok"] = true;
            result["type"] = "offline";
            result["name"] = slot.name;
            result["uuid"] = slot.uuid.empty()
                ? Core::HashUtils::GenerateOfflineUUID(slot.name)
                : slot.uuid;
            result["accessToken"] = "";
            result["xuid"] = "";
            return result;
        }

        nlohmann::json blob;
        bool parsed = false;
        if (!slot.token.empty()) {
            std::string plain = Core::CryptoUtils::UnprotectFromBase64(slot.token);
            if (!plain.empty()) {
                try {
                    blob = nlohmann::json::parse(plain);
                    parsed = true;
                } catch (...) {}
            }
        }
        if (!parsed) {
            result["error"] = "登录信息已无法读取（可能换了 Windows 用户或配置被改动），请重新登录";
            return result;
        }

        // 留 5 分钟余量：令牌在启动过程中过期会让游戏拿到一个无效会话
        const long long now = (long long)std::time(nullptr);
        if (blob.value("expiresAt", (long long)0) - now > 300) {
            result["ok"] = true;
            result["type"] = "msa";
            result["name"] = slot.name;
            result["uuid"] = slot.uuid;
            result["accessToken"] = blob.value("accessToken", "");
            result["xuid"] = blob.value("xuid", "");
            return result;
        }

        AuthLog("槽位 " + std::to_string(index) + " 的令牌已过期，尝试刷新");
        Core::MsaAuth::Session session =
            Core::MsaAuth::Refresh(blob.value("refreshToken", ""), [](const std::string&) {});
        if (!session.ok) {
            result["error"] = session.error;
            return result;
        }

        std::string saveError;
        if (!SaveSessionToSlot(index, session, true, saveError)) {
            result["error"] = saveError;
            return result;
        }

        AuthLog("令牌刷新成功: " + session.name);
        result["ok"] = true;
        result["type"] = "msa";
        result["name"] = session.name;
        result["uuid"] = session.uuid;
        result["accessToken"] = session.accessToken;
        result["xuid"] = session.xuid;
        return result;
    }

private:
    Bridge::WebviewBridge& bridge_;
    std::atomic<bool> running_{false};
    // 流程代号。取消时自增，被顶掉的那条线程在每个检查点发现代号变了就安静退出——
    // 只用一个 bool 是不够的：取消后马上重新登录会把标志复位，
    // 于是旧线程又"活"过来，和新线程一起轮询。
    std::atomic<unsigned long long> generation_{0};

    void RegisterCommands() {
        // auth.msa.start - 开始正版登录（异步，结果走事件）
        bridge_.RegisterCommand("auth.msa.start", [this](const nlohmann::json&) -> nlohmann::json {
            if (running_.exchange(true)) {
                return {{"ok", false}, {"error", "已有登录流程正在进行"}};
            }
            const unsigned long long gen = ++generation_;
            std::thread([this, gen]() { RunLoginFlow(gen); }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // auth.msa.cancel - 用户放弃等待；旧线程会自己退出，不会再发事件
        bridge_.RegisterCommand("auth.msa.cancel", [this](const nlohmann::json&) -> nlohmann::json {
            ++generation_;
            running_ = false;
            return {{"ok", true}};
        });
    }

    bool IsSuperseded(unsigned long long gen) const {
        return generation_.load() != gen;
    }

    static nlohmann::json LoadConfig() {
        try {
            return nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    void EmitEvent(const std::string& name, const nlohmann::json& data) {
        try {
            bridge_.Emit(name, data);
        } catch (...) {}
    }

    void Finish(bool ok, const std::string& error, const nlohmann::json& extra = nlohmann::json::object()) {
        nlohmann::json result = extra.is_object() ? extra : nlohmann::json::object();
        result["ok"] = ok;
        if (!error.empty()) result["error"] = error;
        running_ = false;
        EmitEvent("auth.msa.result", result);
    }

    void RunLoginFlow(unsigned long long gen) {
        AuthLog("=== 正版登录开始 ===");

        Core::MsaAuth::DeviceCode code = Core::MsaAuth::RequestDeviceCode();
        if (IsSuperseded(gen)) return;
        if (!code.ok) {
            AuthLog("申请设备代码失败: " + code.error);
            Finish(false, code.error);
            return;
        }
        AuthLog("已取得设备代码, 有效期 " + std::to_string(code.expiresIn) + " 秒");

        // 把代码交给前端展示；前端负责用默认浏览器打开验证页
        EmitEvent("auth.msa.code", {
            {"userCode", code.userCode},
            {"verificationUri", code.verificationUri},
            {"expiresIn", code.expiresIn},
            {"interval", code.interval}
        });

        std::string msaAccessToken;
        std::string msaRefreshToken;
        switch (WaitForAuthorization(code, gen, msaAccessToken, msaRefreshToken)) {
            case AuthOutcome::Authorized:
                break;
            case AuthOutcome::Superseded:
                return;
            case AuthOutcome::Failed:
                return;   // 失败详情已经 Finish 过了
        }

        AuthLog("用户已授权, 开始换 Minecraft 令牌");
        auto onStage = [this, gen](const std::string& stage) {
            if (!IsSuperseded(gen)) EmitEvent("auth.msa.status", {{"stage", stage}});
        };
        Core::MsaAuth::Session session =
            Core::MsaAuth::CompleteLogin(msaAccessToken, msaRefreshToken, onStage);

        if (IsSuperseded(gen)) return;
        if (!session.ok) {
            AuthLog("登录失败: " + session.error);
            Finish(false, session.error);
            return;
        }

        std::string saveError;
        const int slot = FindTargetSlot(session, saveError);
        if (slot < 0) {
            Finish(false, saveError);
            return;
        }
        if (!SaveSessionToSlot(slot, session, true, saveError)) {
            Finish(false, saveError);
            return;
        }

        AuthLog("登录完成: " + session.name + " (" + session.uuid + "), 写入槽位 " + std::to_string(slot));
        Finish(true, "", {{"name", session.name}, {"uuid", session.uuid}, {"index", slot}});
    }

    enum class AuthOutcome { Authorized, Failed, Superseded };

    // 按 interval 轮询授权状态，直到用户授权 / 拒绝 / 超时 / 被取消
    AuthOutcome WaitForAuthorization(const Core::MsaAuth::DeviceCode& code, unsigned long long gen,
                                     std::string& outAccessToken, std::string& outRefreshToken) {
        int interval = code.interval;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(code.expiresIn);

        while (std::chrono::steady_clock::now() < deadline) {
            // 微软要求两次轮询之间不短于 interval，首个间隔也不能省
            std::this_thread::sleep_for(std::chrono::seconds(interval));
            if (IsSuperseded(gen)) {
                AuthLog("登录流程已被取消或取代");
                return AuthOutcome::Superseded;
            }

            Core::MsaAuth::PollResult poll = Core::MsaAuth::PollToken(code.deviceCode);
            switch (poll.state) {
                case Core::MsaAuth::PollState::Pending:
                    continue;
                case Core::MsaAuth::PollState::SlowDown:
                    // 轮询太快了，按官方建议退让一步
                    interval += 5;
                    AuthLog("轮询过快, 间隔调整为 " + std::to_string(interval) + " 秒");
                    continue;
                case Core::MsaAuth::PollState::Authorized:
                    outAccessToken = poll.accessToken;
                    outRefreshToken = poll.refreshToken;
                    return AuthOutcome::Authorized;
                case Core::MsaAuth::PollState::Failed:
                default:
                    AuthLog("授权失败: " + poll.error);
                    Finish(false, poll.error);
                    return AuthOutcome::Failed;
            }
        }

        AuthLog("授权超时");
        Finish(false, "授权超时，请重新登录");
        return AuthOutcome::Failed;
    }

    // 已登录过同一个角色就覆盖原槽位，否则用第一个空槽位
    static int FindTargetSlot(const Core::MsaAuth::Session& session, std::string& error) {
        nlohmann::json config = LoadConfig();

        for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
            Core::ConfigManager::AccountSlot slot = Core::ConfigManager::ReadAccountSlot(config, i);
            if (slot.used && slot.type == "msa" && slot.uuid == session.uuid) return i;
        }
        for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
            if (!Core::ConfigManager::ReadAccountSlot(config, i).used) return i;
        }

        error = "账号槽位已满，请先在设置里删除一个角色";
        return -1;
    }

    // 把会话写进槽位。select 为真时同时把该槽位记为当前使用的账号。
    static bool SaveSessionToSlot(int index, const Core::MsaAuth::Session& session,
                                  bool select, std::string& error) {
        nlohmann::json tokenBlob = {
            {"accessToken", session.accessToken},
            {"refreshToken", session.refreshToken},
            {"expiresAt", session.expiresAt},
            {"xuid", session.xuid}
        };

        // 明文令牌只存在于内存，落盘的永远是 DPAPI 密文
        std::string encrypted = Core::CryptoUtils::ProtectToBase64(tokenBlob.dump());
        if (encrypted.empty()) {
            error = "保存登录信息失败（系统加密不可用）";
            return false;
        }

        nlohmann::json config = LoadConfig();
        Core::ConfigManager::WriteAccountSlot(config, index, session.name, session.uuid, "msa", encrypted);
        if (select) {
            Core::ConfigManager::SetSelectedAccountIndex(config, index);
        }

        if (!Core::ConfigManager::WriteConfigFile(config.dump(4))) {
            error = "写入配置文件失败";
            return false;
        }
        return true;
    }

    // 只记步骤，绝不记 device_code / access_token / refresh_token
    static void AuthLog(const std::string& message) {
        std::wstring logDir = Core::FileManager::GetExeDir() + L"\\.minecraft\\logs";
        CreateDirectoryW(logDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char logName[64];
        snprintf(logName, sizeof(logName), "auth_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);
        std::wstring logPath = logDir + L"\\" + Core::StringUtils::Utf8ToWide(std::string(logName));

        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
        std::string line = timestamp + message + "\n";

        HANDLE hFile = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            SetFilePointer(hFile, 0, nullptr, FILE_END);
            DWORD written = 0;
            WriteFile(hFile, line.c_str(), (DWORD)line.size(), &written, nullptr);
            CloseHandle(hFile);
        }
    }
};

} // namespace Services
