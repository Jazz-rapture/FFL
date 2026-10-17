#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/CryptoUtils.h"
#include "Core/YggdrasilAuth.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

// 外置登录（第三方验证服务器 / authlib-injector）。
// auth.yggdrasil.login 起一个后台线程：探测 API 根地址 → authenticate 拿令牌与角色列表
// → 把角色和 DPAPI 加密后的令牌写进账号槽位。命令立刻返回，全程用事件回报，
// 不阻塞 WebView 那条串行的工作线程。
//
// ⚠ 安全约定：密码只作为参数进内存，不写日志、不回传前端、不落盘；
// 令牌落盘前一律 DPAPI 加密（Core::CryptoUtils），事件里不含任何令牌字段
class YggdrasilAuthService {
public:
    YggdrasilAuthService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

    // 启动游戏前调用：校验令牌，失效就刷新。返回
    // {ok, type:"yggdrasil", name, uuid, accessToken, apiRoot}；失败 {ok:false, error}
    static nlohmann::json EnsureFreshSession(int index) {
        nlohmann::json result = {{"ok", false}};

        nlohmann::json config = LoadConfig();
        Core::ConfigManager::AccountSlot slot = Core::ConfigManager::ReadAccountSlot(config, index);
        if (!slot.used || slot.type != "yggdrasil") {
            result["error"] = "没有可用的外置登录账号";
            return result;
        }

        Core::YggdrasilAuth::Session session;
        if (!LoadSessionFromSlot(slot, session)) {
            result["error"] = "登录信息已无法读取（可能换了 Windows 用户或配置被改动），请重新登录";
            return result;
        }
        if (session.apiRoot.empty() || session.accessToken.empty()) {
            result["error"] = "登录信息不完整，请重新登录";
            return result;
        }

        bool valid = false;
        std::string error;
        if (!Core::YggdrasilAuth::Validate(session.apiRoot, session.accessToken,
                                           session.clientToken, valid, error)) {
            // 校验接口问不通（多半是网络）时不拦启动：游戏自己会给出准确报错，
            // 而在这里一刀切会让"验证服务器偶尔抽风"变成"完全进不了游戏"
            AuthLog("校验未完成（" + error + "），跳过校验直接使用现有令牌");
            valid = true;
        }

        if (!valid) {
            AuthLog("槽位 " + std::to_string(index) + " 的令牌已失效，尝试刷新");
            if (!Core::YggdrasilAuth::Refresh(session, nullptr, error)) {
                result["error"] = error;
                return result;
            }
            std::string saveError;
            if (!SaveSessionToSlot(index, session, false, saveError)) {
                result["error"] = saveError;
                return result;
            }
            AuthLog("令牌刷新成功: " + session.profileName);
        }

        result["ok"] = true;
        result["type"] = "yggdrasil";
        result["name"] = session.profileName.empty() ? slot.name : session.profileName;
        result["uuid"] = session.profileId.empty() ? slot.uuid : session.profileId;
        result["accessToken"] = session.accessToken;
        result["apiRoot"] = session.apiRoot;
        return result;
    }

private:
    Bridge::WebviewBridge& bridge_;
    std::atomic<bool> running_{false};
    // 流程代号：取消 / 重新登录时自增，被顶掉的线程在每个检查点发现代号变了就安静退出
    std::atomic<unsigned long long> generation_{0};

    // 已经认证成功、但服务端下发多个角色且没自动绑定时，先把会话挂在这儿等用户选
    std::mutex pendingMutex_;
    Core::YggdrasilAuth::Session pendingSession_;
    bool hasPending_ = false;

    void RegisterCommands() {
        // auth.yggdrasil.login - 外置登录（异步，结果走事件）
        bridge_.RegisterCommand("auth.yggdrasil.login",
                                [this](const nlohmann::json& args) -> nlohmann::json {
            const std::string url = args.value("url", "");
            const std::string username = args.value("username", "");
            const std::string password = args.value("password", "");

            if (url.empty()) return {{"ok", false}, {"error", "请输入验证服务器地址"}};
            if (username.empty()) return {{"ok", false}, {"error", "请输入用户名"}};
            if (password.empty()) return {{"ok", false}, {"error", "请输入密码"}};

            if (running_.exchange(true)) {
                return {{"ok", false}, {"error", "已有登录流程正在进行"}};
            }
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                hasPending_ = false;
            }

            const unsigned long long gen = ++generation_;
            std::thread([this, gen, url, username, password]() {
                RunLoginFlow(gen, url, username, password);
            }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // auth.yggdrasil.select - 服务端给了多个角色时，用户选定一个后再绑定
        bridge_.RegisterCommand("auth.yggdrasil.select",
                                [this](const nlohmann::json& args) -> nlohmann::json {
            const std::string profileId = args.value("profileId", "");
            if (profileId.empty()) return {{"ok", false}, {"error", "未指定角色"}};

            if (running_.exchange(true)) {
                return {{"ok", false}, {"error", "已有登录流程正在进行"}};
            }

            const unsigned long long gen = ++generation_;
            std::thread([this, gen, profileId]() { RunSelectFlow(gen, profileId); }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // auth.yggdrasil.cancel - 用户放弃等待
        bridge_.RegisterCommand("auth.yggdrasil.cancel",
                                [this](const nlohmann::json&) -> nlohmann::json {
            ++generation_;
            running_ = false;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                hasPending_ = false;
            }
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

    void EmitStatus(const std::string& stage) {
        EmitEvent("auth.yggdrasil.status", {{"stage", stage}});
    }

    // 事件里只放能见人的字段：name / uuid / apiRoot / 角色列表，绝不含令牌
    void Finish(bool ok, const std::string& error,
                const nlohmann::json& extra = nlohmann::json::object()) {
        nlohmann::json result = extra.is_object() ? extra : nlohmann::json::object();
        result["ok"] = ok;
        if (!error.empty()) result["error"] = error;
        running_ = false;
        EmitEvent("auth.yggdrasil.result", result);
    }

    void RunLoginFlow(unsigned long long gen, const std::string& url,
                      const std::string& username, const std::string& password) {
        AuthLog("=== 外置登录开始 ===");

        EmitStatus("正在探测验证服务器...");
        std::string apiRoot;
        std::string error;
        if (!Core::YggdrasilAuth::ProbeApiRoot(url, apiRoot, error)) {
            AuthLog("探测失败: " + error);
            Finish(false, error);
            return;
        }
        if (IsSuperseded(gen)) return;
        AuthLog("API 根地址: " + apiRoot);

        EmitStatus("正在登录...");
        Core::YggdrasilAuth::Session session;
        if (!Core::YggdrasilAuth::Authenticate(apiRoot, username, password, session, error)) {
            // ⚠ 这里只记服务端给的错误文本，不记用户名和密码
            AuthLog("认证失败: " + error);
            Finish(false, error);
            return;
        }
        if (IsSuperseded(gen)) return;

        // 服务端没自动绑定角色时的两种收尾：只有一个就直接绑，多个就交给用户选。
        // （协议允许省略 selectedProfile，多数服务端会在只有一个角色时自动选中）
        if (session.profileId.empty()) {
            if (session.availableProfiles.empty()) {
                AuthLog("账号下没有可用角色");
                Finish(false, "该账号下没有可用角色，请先在验证服务器的用户中心创建角色");
                return;
            }

            if (session.availableProfiles.size() > 1) {
                AuthLog("服务端返回 " + std::to_string(session.availableProfiles.size()) +
                        " 个角色，等待用户选择");
                {
                    std::lock_guard<std::mutex> lock(pendingMutex_);
                    pendingSession_ = session;
                    hasPending_ = true;
                }
                nlohmann::json profiles = nlohmann::json::array();
                for (const auto& p : session.availableProfiles) {
                    profiles.push_back({{"id", p.id}, {"name", p.name}});
                }
                Finish(true, "", {{"apiRoot", session.apiRoot},
                                  {"needProfileSelect", true},
                                  {"profiles", profiles}});
                return;
            }

            EmitStatus("正在绑定角色...");
            if (!Core::YggdrasilAuth::Refresh(session, &session.availableProfiles[0], error)) {
                AuthLog("绑定角色失败: " + error);
                Finish(false, error);
                return;
            }
            if (IsSuperseded(gen)) return;
        }

        SaveAndFinish(gen, session);
    }

    void RunSelectFlow(unsigned long long gen, const std::string& profileId) {
        Core::YggdrasilAuth::Session session;
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            if (!hasPending_) {
                Finish(false, "登录信息已失效，请重新登录");
                return;
            }
            session = pendingSession_;
        }

        const Core::YggdrasilAuth::Profile* chosen = nullptr;
        for (const auto& p : session.availableProfiles) {
            if (p.id == profileId) {
                chosen = &p;
                break;
            }
        }
        if (chosen == nullptr) {
            Finish(false, "选择的角色不在可用列表里，请重新登录");
            return;
        }

        EmitStatus("正在绑定角色...");
        std::string error;
        if (!Core::YggdrasilAuth::Refresh(session, chosen, error)) {
            AuthLog("绑定角色失败: " + error);
            Finish(false, error);
            return;
        }
        if (IsSuperseded(gen)) return;

        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            hasPending_ = false;
        }
        SaveAndFinish(gen, session);
    }

    void SaveAndFinish(unsigned long long gen, const Core::YggdrasilAuth::Session& session) {
        std::string error;
        const int slot = FindTargetSlot(session, error);
        if (slot < 0) {
            Finish(false, error);
            return;
        }
        if (!SaveSessionToSlot(slot, session, true, error)) {
            Finish(false, error);
            return;
        }
        if (IsSuperseded(gen)) return;

        AuthLog("登录完成: " + session.profileName + "，写入槽位 " + std::to_string(slot));
        Finish(true, "", {{"name", session.profileName},
                          {"uuid", session.profileId},
                          {"index", slot},
                          {"apiRoot", session.apiRoot}});
    }

    // 同一个验证服务器上的同一个角色就覆盖原槽位，否则用第一个空槽位
    static int FindTargetSlot(const Core::YggdrasilAuth::Session& session, std::string& error) {
        nlohmann::json config = LoadConfig();

        for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
            Core::ConfigManager::AccountSlot slot = Core::ConfigManager::ReadAccountSlot(config, i);
            if (!slot.used || slot.type != "yggdrasil") continue;
            if (slot.uuid != session.profileId) continue;

            Core::YggdrasilAuth::Session existing;
            if (LoadSessionFromSlot(slot, existing) && existing.apiRoot == session.apiRoot) return i;
        }
        for (int i = 0; i < Core::ConfigManager::kAccountSlots; i++) {
            if (!Core::ConfigManager::ReadAccountSlot(config, i).used) return i;
        }

        error = "账号槽位已满，请先在设置里删除一个角色";
        return -1;
    }

    // 令牌只以 DPAPI 密文落盘：config.json 被拷走也换不出一个能用的会话
    static bool SaveSessionToSlot(int index, const Core::YggdrasilAuth::Session& session,
                                  bool select, std::string& error) {
        nlohmann::json blob = {
            {"accessToken", session.accessToken},
            {"clientToken", session.clientToken},
            {"apiRoot", session.apiRoot},
            {"profileId", session.profileId},
            {"profileName", session.profileName}
        };

        std::string encrypted = Core::CryptoUtils::ProtectToBase64(blob.dump());
        if (encrypted.empty()) {
            error = "保存登录信息失败（系统加密不可用）";
            return false;
        }

        nlohmann::json config = LoadConfig();
        Core::ConfigManager::WriteAccountSlot(config, index, session.profileName,
                                              session.profileId, "yggdrasil", encrypted);
        if (select) {
            Core::ConfigManager::SetSelectedAccountIndex(config, index);
        }

        if (!Core::ConfigManager::WriteConfigFile(config.dump(4))) {
            error = "写入配置文件失败";
            return false;
        }
        return true;
    }

    static bool LoadSessionFromSlot(const Core::ConfigManager::AccountSlot& slot,
                                    Core::YggdrasilAuth::Session& out) {
        if (slot.token.empty()) return false;

        const std::string plain = Core::CryptoUtils::UnprotectFromBase64(slot.token);
        if (plain.empty()) return false;

        nlohmann::json blob;
        try {
            blob = nlohmann::json::parse(plain);
        } catch (...) {
            return false;
        }

        out.apiRoot = blob.value("apiRoot", "");
        out.accessToken = blob.value("accessToken", "");
        out.clientToken = blob.value("clientToken", "");
        out.profileId = blob.value("profileId", slot.uuid);
        out.profileName = blob.value("profileName", slot.name);
        return true;
    }

    // 只记步骤，绝不记密码 / accessToken / clientToken
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
