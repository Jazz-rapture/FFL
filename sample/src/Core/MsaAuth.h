#pragma once
#include <string>
#include <functional>
#include <ctime>
#include <nlohmann/json.hpp>
#include "Core/HttpClient.h"

namespace Core {

// 微软账号 → Minecraft 的完整验证链（设备代码流），对应 work.txt 里的每个请求：
//   设备代码 → 轮询授权 → Xbox Live → XSTS → Minecraft 令牌 → 拥有情况 → 角色档案
// 这里只做网络与解析，不碰配置文件也不碰前端；进度通过 onStage 回调交给上层上报。
class MsaAuth {
public:
    // Azure 应用（公共客户端）。设备代码流不需要 client_secret。
    static std::string ClientId() { return "566aa178-8c85-43de-9198-18901dac37ec"; }
    static std::string Scope() { return "XboxLive.signin offline_access"; }

    // 进度阶段标识，前端映射成中文提示
    static const char* StageXboxLive() { return "xbox_live"; }
    static const char* StageXsts() { return "xsts"; }
    static const char* StageMinecraft() { return "minecraft"; }
    static const char* StageEntitlements() { return "entitlements"; }
    static const char* StageProfile() { return "profile"; }

    struct DeviceCode {
        bool ok = false;
        std::string error;
        std::string deviceCode;       // 轮询用，不展示给用户
        std::string userCode;         // 用户在浏览器里输入的代码
        std::string verificationUri;
        int expiresIn = 900;          // 代码对有效期（秒）
        int interval = 5;             // 最小轮询间隔（秒）
    };

    // 一个可用的 Minecraft 会话
    struct Session {
        bool ok = false;
        std::string error;
        std::string accessToken;      // Minecraft 访问令牌（启动游戏用）
        std::string refreshToken;     // 微软刷新令牌（长期凭证）
        long long expiresAt = 0;      // accessToken 过期时刻（unix 秒）
        std::string uuid;             // 角色 UUID（32 位十六进制，无横线）
        std::string name;             // 角色名
        std::string xuid;             // Xbox 用户 ID
    };

    enum class PollState { Authorized, Pending, SlowDown, Failed };
    struct PollResult {
        PollState state = PollState::Failed;
        std::string error;
        std::string accessToken;
        std::string refreshToken;
        int expiresIn = 0;
    };

    // 第一步：申请设备代码对
    static DeviceCode RequestDeviceCode() {
        DeviceCode code;

        std::string form = "client_id=" + UrlEncode(ClientId()) +
                           "&scope=" + UrlEncode(Scope());
        Models::HttpResult res = HttpClient::HttpPostForm(
            L"https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode", form);

        nlohmann::json j;
        if (res.status != 200 || !ParseJson(res.body, j)) {
            code.error = DescribeFailure(res, "申请设备代码失败");
            return code;
        }

        code.deviceCode = j.value("device_code", "");
        code.userCode = j.value("user_code", "");
        code.verificationUri = j.value("verification_uri", "");
        code.expiresIn = j.value("expires_in", 900);
        code.interval = j.value("interval", 5);

        if (code.deviceCode.empty() || code.userCode.empty()) {
            code.error = "设备代码响应缺少必要字段";
            return code;
        }
        if (code.verificationUri.empty()) code.verificationUri = "https://www.microsoft.com/link";
        if (code.interval < 1) code.interval = 5;
        if (code.expiresIn < 60) code.expiresIn = 900;

        code.ok = true;
        return code;
    }

    // 第二步：轮询一次用户授权状态。调用方负责按 interval 间隔重复调用，
    // authorization_pending / slow_down 属于"继续等"，其余错误都要停下来。
    static PollResult PollToken(const std::string& deviceCode) {
        PollResult result;

        std::string form = "grant_type=" + UrlEncode("urn:ietf:params:oauth:grant-type:device_code") +
                           "&client_id=" + UrlEncode(ClientId()) +
                           "&device_code=" + UrlEncode(deviceCode);
        Models::HttpResult res = HttpClient::HttpPostForm(
            L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token", form);

        nlohmann::json j;
        if (!ParseJson(res.body, j)) {
            result.error = DescribeFailure(res, "轮询授权状态失败");
            return result;
        }

        if (res.status == 200 && j.contains("access_token")) {
            result.state = PollState::Authorized;
            result.accessToken = j.value("access_token", "");
            result.refreshToken = j.value("refresh_token", "");
            result.expiresIn = j.value("expires_in", 3600);
            return result;
        }

        std::string err = j.value("error", "");
        if (err == "authorization_pending") {
            result.state = PollState::Pending;
            return result;
        }
        if (err == "slow_down") {
            result.state = PollState::SlowDown;
            return result;
        }
        result.error = DescribeOAuthError(err, j.value("error_description", ""));
        return result;
    }

    // 刷新令牌 → 新会话。accessToken 过期（或快过期）时用，避免每次登录都弹浏览器。
    static Session Refresh(const std::string& refreshToken,
                           const std::function<void(const std::string&)>& onStage) {
        Session session;
        if (refreshToken.empty()) {
            session.error = "缺少刷新令牌，请重新登录";
            return session;
        }

        std::string form = "client_id=" + UrlEncode(ClientId()) +
                           "&refresh_token=" + UrlEncode(refreshToken) +
                           "&grant_type=refresh_token" +
                           "&scope=" + UrlEncode(Scope());
        Models::HttpResult res = HttpClient::HttpPostForm(
            L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token", form);

        nlohmann::json j;
        if (!ParseJson(res.body, j)) {
            session.error = DescribeFailure(res, "刷新登录状态失败");
            return session;
        }
        if (res.status != 200 || !j.contains("access_token")) {
            session.error = DescribeOAuthError(j.value("error", ""), j.value("error_description", ""));
            if (session.error.empty()) session.error = "刷新登录状态失败";
            return session;
        }

        return CompleteLogin(j["access_token"].get<std::string>(),
                             j.value("refresh_token", refreshToken), onStage);
    }

    // 微软访问令牌 → 完整 Minecraft 会话
    static Session CompleteLogin(const std::string& msaAccessToken,
                                 const std::string& msaRefreshToken,
                                 const std::function<void(const std::string&)>& onStage) {
        Session session;
        session.refreshToken = msaRefreshToken;
        if (msaAccessToken.empty()) {
            session.error = "缺少微软访问令牌";
            return session;
        }

        onStage(StageXboxLive());
        std::string xblToken;
        std::string uhs;
        std::string xuid;
        if (!AuthenticateXboxLive(msaAccessToken, xblToken, uhs, xuid, session.error)) return session;

        onStage(StageXsts());
        std::string xstsToken;
        std::string xstsUhs;
        if (!AuthorizeXsts(xblToken, xstsToken, xstsUhs, session.error)) return session;
        if (!xstsUhs.empty()) uhs = xstsUhs;

        onStage(StageMinecraft());
        int mcExpiresIn = 0;
        if (!LoginWithXbox(uhs, xstsToken, session.accessToken, mcExpiresIn, session.error)) return session;
        session.expiresAt = (long long)std::time(nullptr) + (mcExpiresIn > 0 ? mcExpiresIn : 86400);

        onStage(StageEntitlements());
        if (!CheckEntitlements(session.accessToken, session.error)) return session;

        onStage(StageProfile());
        if (!FetchProfile(session.accessToken, session.uuid, session.name, session.error)) return session;

        session.xuid = xuid;
        session.ok = true;
        return session;
    }

private:
    // 用户需要输入的账号级错误（XSTS 的 XErr）对应的中文提示
    static std::string XstsErrorText(const std::string& xerr) {
        if (xerr == "2148916233") return "该微软账号还没有 Xbox 档案，请先到 xbox.com 创建档案";
        if (xerr == "2148916235") return "该账号所在地区不支持 Xbox Live";
        if (xerr == "2148916236" || xerr == "2148916237") return "该账号需要先完成成人验证";
        if (xerr == "2148916238") return "该账号未满 18 岁，需要先加入一个家庭组";
        if (xerr == "2148916227") return "该账号已被 Xbox Live 封禁";
        return "Xbox Live 授权失败 (XErr " + xerr + ")";
    }

    static std::string DescribeOAuthError(const std::string& code, const std::string& description) {
        if (code == "authorization_declined") return "你取消了本次授权";
        if (code == "expired_token") return "授权超时，请重新登录";
        if (code == "bad_verification_code") return "设备代码无效，请重新登录";
        if (code == "invalid_grant") return "登录状态已失效，请重新登录";
        if (code == "invalid_client") return "客户端 ID 无效或未开启公共客户端流";
        if (!description.empty()) return description + (code.empty() ? "" : " (" + code + ")");
        if (!code.empty()) return code;
        return "";
    }

    static std::string DescribeFailure(const Models::HttpResult& res, const std::string& what) {
        if (res.status == 0) return what + "：网络请求失败（请检查网络或代理）";
        return what + "：HTTP " + std::to_string(res.status);
    }

    static bool ParseJson(const std::string& body, nlohmann::json& out) {
        try {
            out = nlohmann::json::parse(body);
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::string UrlEncode(const std::string& text) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        out.reserve(text.size() * 3);
        for (unsigned char c : text) {
            const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                    (c >= '0' && c <= '9') ||
                                    c == '-' || c == '_' || c == '.' || c == '~';
            if (unreserved) {
                out += (char)c;
            } else {
                out += '%';
                out += hex[(c >> 4) & 0x0F];
                out += hex[c & 0x0F];
            }
        }
        return out;
    }

    // 第三步：Xbox Live 身份验证，拿 XBL 令牌与 user hash
    static bool AuthenticateXboxLive(const std::string& msaAccessToken, std::string& outToken,
                                     std::string& outUhs, std::string& outXuid, std::string& error) {
        auto send = [&](const std::string& rpsTicket) {
            nlohmann::json body = {
                {"Properties", {
                    {"AuthMethod", "RPS"},
                    {"SiteName", "user.auth.xboxlive.com"},
                    {"RpsTicket", rpsTicket}
                }},
                {"RelyingParty", "http://auth.xboxlive.com"},
                {"TokenType", "JWT"}
            };
            return HttpClient::HttpPostJson(
                L"https://user.auth.xboxlive.com/user/authenticate", body.dump());
        };

        Models::HttpResult res = send("d=" + msaAccessToken);
        // work.txt：如遇 "Bad Request" 可去掉 "d=" 前缀
        if (res.status == 400) res = send(msaAccessToken);

        nlohmann::json j;
        if (res.status != 200 || !ParseJson(res.body, j)) {
            error = DescribeFailure(res, "Xbox Live 身份验证失败");
            return false;
        }

        outToken = j.value("Token", "");
        if (j.contains("DisplayClaims") && j["DisplayClaims"].is_object() &&
            j["DisplayClaims"].contains("xui") && j["DisplayClaims"]["xui"].is_array() &&
            !j["DisplayClaims"]["xui"].empty()) {
            const auto& xui = j["DisplayClaims"]["xui"][0];
            outUhs = xui.value("uhs", "");
            outXuid = xui.value("xid", "");
        }

        if (outToken.empty() || outUhs.empty()) {
            error = "Xbox Live 身份验证响应缺少 Token / uhs";
            return false;
        }
        return true;
    }

    // 第四步：XSTS 授权，拿到能登录 Minecraft 的令牌
    static bool AuthorizeXsts(const std::string& xblToken, std::string& outToken,
                              std::string& outUhs, std::string& error) {
        nlohmann::json body = {
            {"Properties", {
                {"SandboxId", "RETAIL"},
                {"UserTokens", nlohmann::json::array({xblToken})}
            }},
            {"RelyingParty", "rp://api.minecraftservices.com/"},
            {"TokenType", "JWT"}
        };

        Models::HttpResult res = HttpClient::HttpPostJson(
            L"https://xsts.auth.xboxlive.com/xsts/authorize", body.dump());

        nlohmann::json j;
        if (!ParseJson(res.body, j)) {
            error = DescribeFailure(res, "XSTS 授权失败");
            return false;
        }

        if (res.status != 200) {
            std::string xerr = j.value("XErr", "");
            // 401 + XErr 是账号本身的问题（没有 Xbox 档案、未成年等），
            // 这些提示必须原样告诉用户，否则用户完全不知道要做什么
            error = xerr.empty() ? DescribeFailure(res, "XSTS 授权失败") : XstsErrorText(xerr);
            return false;
        }

        outToken = j.value("Token", "");
        if (j.contains("DisplayClaims") && j["DisplayClaims"].is_object() &&
            j["DisplayClaims"].contains("xui") && j["DisplayClaims"]["xui"].is_array() &&
            !j["DisplayClaims"]["xui"].empty()) {
            outUhs = j["DisplayClaims"]["xui"][0].value("uhs", "");
        }

        if (outToken.empty()) {
            error = "XSTS 授权响应缺少 Token";
            return false;
        }
        return true;
    }

    // 第五步：用 XSTS 令牌换 Minecraft 访问令牌
    static bool LoginWithXbox(const std::string& uhs, const std::string& xstsToken,
                              std::string& outToken, int& outExpiresIn, std::string& error) {
        nlohmann::json body = {
            {"identityToken", "XBL3.0 x=" + uhs + ";" + xstsToken}
        };

        Models::HttpResult res = HttpClient::HttpPostJson(
            L"https://api.minecraftservices.com/authentication/login_with_xbox", body.dump());

        nlohmann::json j;
        if (res.status != 200 || !ParseJson(res.body, j)) {
            error = DescribeFailure(res, "获取 Minecraft 访问令牌失败");
            return false;
        }

        outToken = j.value("access_token", "");
        outExpiresIn = j.value("expires_in", 0);
        if (outToken.empty()) {
            error = "Minecraft 访问令牌响应缺少 access_token";
            return false;
        }
        return true;
    }

    // 第六步：检查账号是否拥有 Minecraft
    static bool CheckEntitlements(const std::string& accessToken, std::string& error) {
        Models::HttpResult res = HttpClient::HttpGetBearer(
            L"https://api.minecraftservices.com/entitlements/mcstore", accessToken);

        nlohmann::json j;
        if (!ParseJson(res.body, j)) {
            error = DescribeFailure(res, "检查游戏拥有情况失败");
            return false;
        }
        if (res.status != 200) {
            error = DescribeFailure(res, "检查游戏拥有情况失败");
            return false;
        }
        if (!j.contains("items") || !j["items"].is_array() || j["items"].empty()) {
            error = "该账号还没有购买 Minecraft，无法启动游戏";
            return false;
        }
        return true;
    }

    // 第七步：拿到角色 UUID 与用户名
    static bool FetchProfile(const std::string& accessToken, std::string& outUuid,
                             std::string& outName, std::string& error) {
        Models::HttpResult res = HttpClient::HttpGetBearer(
            L"https://api.minecraftservices.com/minecraft/profile", accessToken);

        nlohmann::json j;
        if (!ParseJson(res.body, j)) {
            error = DescribeFailure(res, "获取角色档案失败");
            return false;
        }
        if (res.status != 200) {
            error = "该账号没有 Minecraft 档案（可能未购买游戏）";
            return false;
        }

        outUuid = j.value("id", "");
        outName = j.value("name", "");
        if (outUuid.empty() || outName.empty()) {
            error = "角色档案响应缺少 id / name";
            return false;
        }
        return true;
    }
};

} // namespace Core
