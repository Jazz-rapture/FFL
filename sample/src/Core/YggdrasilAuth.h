#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <Windows.h>
#include <objbase.h>
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "StringUtils.h"

namespace Core {

// 外置登录（第三方验证服务器 / authlib-injector）的 Yggdrasil 协议客户端。
// 请求与响应严格按 Mojang Yggdrasil 协议：
//   探测 API 根   GET  <用户输入地址>                        → 读 X-Authlib-Injector-API-Location
//   登录          POST {apiRoot}/authserver/authenticate
//   刷新          POST {apiRoot}/authserver/refresh
//   校验          POST {apiRoot}/authserver/validate
//
// ⚠ 这个类经手明文密码。它自己不写任何日志；调用方也不得把密码写进
// 日志或配置文件（HttpClient 只记 URL，不记请求体，所以密码不会漏进 http 日志）。
// 令牌一律 DPAPI 加密后才落盘，见 Services::YggdrasilAuthService
class YggdrasilAuth {
public:
    struct Profile {
        std::string id;
        std::string name;
    };

    // 一次登录会话
    struct Session {
        std::string apiRoot;         // 探测后确认的完整 API 根地址
        std::string accessToken;
        std::string clientToken;
        std::string profileId;
        std::string profileName;
        std::vector<Profile> availableProfiles;
    };

    // 探测真正的 API 根地址：读响应头 X-Authlib-Injector-API-Location，
    // 有就用它（可能是相对地址，按请求地址解析成绝对地址）；
    // 没有就把用户输入本身当 API 根 —— 协议允许服务端不声明这个头
    static bool ProbeApiRoot(const std::string& userInput, std::string& outApiRoot, std::string& error) {
        const std::string root = NormalizeUrl(userInput);
        if (root.empty()) {
            error = "请输入验证服务器地址";
            return false;
        }

        Models::HttpResult res = HttpClient::HttpGet(StringUtils::Utf8ToWide(root));
        if (res.status == 0) {
            error = "无法连接验证服务器，请检查地址是否正确、网络是否可用";
            return false;
        }

        const std::string location = res.GetHeader("X-Authlib-Injector-API-Location");
        if (location.empty()) {
            outApiRoot = root;
            return true;
        }

        outApiRoot = ResolveLocation(root, location);
        if (outApiRoot.empty()) {
            error = "验证服务器返回的 API 地址无法解析";
            return false;
        }
        return true;
    }

    // 登录：拿 accessToken / clientToken / 角色列表
    static bool Authenticate(const std::string& apiRoot, const std::string& username,
                             const std::string& password, Session& out, std::string& error) {
        const std::string clientToken = NewClientToken();

        nlohmann::json body = {
            {"agent", {{"name", "Minecraft"}, {"version", 1}}},
            {"username", username},
            {"password", password},
            {"clientToken", clientToken},
            {"requestUser", true}
        };

        Models::HttpResult res = HttpClient::HttpPostJson(
            StringUtils::Utf8ToWide(apiRoot + "/authserver/authenticate"), body.dump());

        if (res.status == 0) {
            error = "无法连接验证服务器，请检查地址是否正确、网络是否可用";
            return false;
        }
        if (res.status != 200) {
            error = DescribeError(res, "登录失败");
            return false;
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(res.body);
        } catch (...) {
            error = "验证服务器返回了无法解析的内容";
            return false;
        }

        out.apiRoot = apiRoot;
        out.accessToken = j.value("accessToken", "");
        // 服务端可以不下发 clientToken（协议允许沿用请求里那个）
        out.clientToken = j.value("clientToken", clientToken);
        out.profileId.clear();
        out.profileName.clear();
        out.availableProfiles.clear();

        if (j.contains("availableProfiles") && j["availableProfiles"].is_array()) {
            for (const auto& p : j["availableProfiles"]) {
                if (!p.is_object()) continue;
                out.availableProfiles.push_back({p.value("id", ""), p.value("name", "")});
            }
        }
        if (j.contains("selectedProfile") && j["selectedProfile"].is_object()) {
            out.profileId = j["selectedProfile"].value("id", "");
            out.profileName = j["selectedProfile"].value("name", "");
        }

        if (out.accessToken.empty()) {
            error = "验证服务器没有返回访问令牌";
            return false;
        }
        return true;
    }

    // 刷新令牌。selectProfile 非空时顺便把令牌绑到该角色上
    // （服务端只下发多个角色、没自动绑定时要靠这一步）
    static bool Refresh(Session& io, const Profile* selectProfile, std::string& error) {
        if (io.accessToken.empty() || io.clientToken.empty()) {
            error = "登录信息不完整，请重新登录";
            return false;
        }
        if (io.apiRoot.empty()) {
            error = "缺少验证服务器地址，请重新登录";
            return false;
        }

        nlohmann::json body = {
            {"accessToken", io.accessToken},
            {"clientToken", io.clientToken},
            {"requestUser", true}
        };
        if (selectProfile != nullptr) {
            body["selectedProfile"] = {{"id", selectProfile->id}, {"name", selectProfile->name}};
        }

        Models::HttpResult res = HttpClient::HttpPostJson(
            StringUtils::Utf8ToWide(io.apiRoot + "/authserver/refresh"), body.dump());

        if (res.status == 0) {
            error = "无法连接验证服务器，请检查网络";
            return false;
        }
        if (res.status != 200) {
            error = DescribeError(res, "令牌已失效，请重新登录");
            return false;
        }

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(res.body);
        } catch (...) {
            error = "验证服务器返回了无法解析的内容";
            return false;
        }

        const std::string newToken = j.value("accessToken", "");
        if (newToken.empty()) {
            error = "验证服务器没有返回新的访问令牌";
            return false;
        }

        io.accessToken = newToken;
        io.clientToken = j.value("clientToken", io.clientToken);
        if (j.contains("selectedProfile") && j["selectedProfile"].is_object()) {
            io.profileId = j["selectedProfile"].value("id", io.profileId);
            io.profileName = j["selectedProfile"].value("name", io.profileName);
        }
        return true;
    }

    // 校验令牌：204 表示有效，403 表示无效。
    // 返回 false 只代表"问不出来"（网络问题），此时 valid 无意义
    static bool Validate(const std::string& apiRoot, const std::string& accessToken,
                         const std::string& clientToken, bool& valid, std::string& error) {
        nlohmann::json body = {
            {"accessToken", accessToken},
            {"clientToken", clientToken}
        };

        Models::HttpResult res = HttpClient::HttpPostJson(
            StringUtils::Utf8ToWide(apiRoot + "/authserver/validate"), body.dump());

        if (res.status == 204 || res.status == 200) {
            valid = true;
            return true;
        }
        if (res.status == 403 || res.status == 401) {
            valid = false;
            return true;
        }
        if (res.status == 0) {
            error = "无法连接验证服务器，请检查网络";
            return false;
        }
        error = "验证服务器返回异常（HTTP " + std::to_string(res.status) + "）";
        return false;
    }

    // 把 Yggdrasil 的错误体翻成人话：
    // {"error":"ForbiddenOperationException","errorMessage":"Invalid credentials..."}
    static std::string DescribeError(const Models::HttpResult& res, const std::string& fallback) {
        std::string detail;
        try {
            nlohmann::json j = nlohmann::json::parse(res.body);
            detail = j.value("errorMessage", "");
        } catch (...) {}

        if (res.status == 401 || res.status == 403) {
            return detail.empty() ? std::string("用户名或密码错误")
                                  : ("用户名或密码错误：" + detail);
        }
        if (!detail.empty()) return fallback + "：" + detail;
        return fallback + "（HTTP " + std::to_string(res.status) + "）";
    }

    // 规范化地址：去掉所有空白、补上 https:// 前缀、去掉末尾斜杠。
    // 用户经常直接贴 "example.com/api/yggdrasil"，不带协议头
    static std::string NormalizeUrl(const std::string& url) {
        std::string s;
        for (char c : url) {
            if (!std::isspace((unsigned char)c)) s.push_back(c);
        }
        if (s.empty()) return "";

        const std::string lower = ToLower(s);
        if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
            s = "https://" + s;
        }
        while (s.size() > 8 && s.back() == '/') s.pop_back();
        return s;
    }

private:
    static std::string ToLower(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) out.push_back((char)std::tolower((unsigned char)c));
        return out;
    }

    // X-Authlib-Injector-API-Location 可能是绝对地址，也可能只是路径。
    // 后者按"同一个站点"解析（实际服务器要么给绝对地址，要么给 '/' 开头的路径）
    static std::string ResolveLocation(const std::string& base, const std::string& location) {
        std::string loc;
        for (char c : location) {
            if (!std::isspace((unsigned char)c)) loc.push_back(c);
        }
        if (loc.empty()) return "";

        const std::string lower = ToLower(loc);
        if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) {
            return NormalizeUrl(loc);
        }

        const size_t schemeEnd = base.find("://");
        if (schemeEnd == std::string::npos) return "";
        const size_t pathStart = base.find('/', schemeEnd + 3);
        const std::string origin =
            (pathStart == std::string::npos) ? base : base.substr(0, pathStart);

        if (loc[0] != '/') loc = "/" + loc;
        return NormalizeUrl(origin + loc);
    }

    // 客户端令牌：协议要求是 UUID，直接问系统要一个
    static std::string NewClientToken() {
        GUID guid{};
        if (SUCCEEDED(CoCreateGuid(&guid))) {
            char buf[48];
            snprintf(buf, sizeof(buf),
                     "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                     (unsigned long)guid.Data1, (unsigned)guid.Data2, (unsigned)guid.Data3,
                     guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                     guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
            return std::string(buf);
        }

        // 拿不到 GUID 的退路。clientToken 只是会话标识，不是密钥
        static std::atomic<unsigned long long> counter{0};
        const unsigned long long v =
            (unsigned long long)GetTickCount64() ^ (++counter * 0x9E3779B97F4A7C15ull);
        char buf[48];
        snprintf(buf, sizeof(buf), "%016llx-0000-4000-8000-%012llx", v, v >> 16);
        return std::string(buf);
    }
};

} // namespace Core
