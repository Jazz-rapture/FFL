#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "HttpClient.h"
#include "FileManager.h"
#include "StringUtils.h"

namespace Core {

// authlib-injector.jar 的本地缓存。
// 外置登录启动游戏时要加 -javaagent:<jar 绝对路径>=<完整 API 根地址>，
// 游戏进程靠这个 agent 把验证请求转给第三方服务器，所以启动前必须保证 jar 在本地。
// 只在缺失时下载：启动路径上多一次网络请求不值得
class AuthlibInjector {
public:
    // 官方元数据端点，返回当前版本号与下载地址
    static const char* MetadataUrl() {
        return "https://authlib-injector.yushi.moe/artifact/latest.json";
    }

    // 缓存位置：<exe 目录>\.minecraft\cache\authlib-injector\authlib-injector.jar
    static std::wstring GetJarPath() {
        return FileManager::GetExeDir() +
               L"\\.minecraft\\cache\\authlib-injector\\authlib-injector.jar";
    }

    // 保证 jar 存在（没有就下载），返回它的绝对路径；失败返回空串并填 error
    static std::string EnsureJar(std::string& error) {
        const std::wstring jarPath = GetJarPath();
        if (FileManager::FileExists(jarPath)) {
            return StringUtils::WideToUtf8(jarPath);
        }

        Models::HttpResult meta =
            HttpClient::HttpGet(StringUtils::Utf8ToWide(std::string(MetadataUrl())));
        if (meta.status != 200) {
            error = "获取 authlib-injector 版本信息失败（HTTP " +
                    std::to_string(meta.status) + "）";
            return "";
        }

        std::string downloadUrl;
        try {
            nlohmann::json j = nlohmann::json::parse(meta.body);
            downloadUrl = j.value("download_url", "");
        } catch (...) {
            error = "authlib-injector 版本信息无法解析";
            return "";
        }
        if (downloadUrl.empty()) {
            error = "authlib-injector 版本信息里没有下载地址";
            return "";
        }

        Models::HttpResult jar =
            HttpClient::HttpGet(StringUtils::Utf8ToWide(downloadUrl));
        if (jar.status != 200 || jar.body.empty()) {
            error = "下载 authlib-injector 失败（HTTP " + std::to_string(jar.status) + "）";
            return "";
        }
        // 下载失败的响应体（例如一页 HTML 报错）长得不像 jar，别把它塞进缓存，
        // 否则游戏每次都拿着一个假 agent 启动，症状会很难查
        if (jar.body.size() < 1024 || jar.body[0] != 'P' || jar.body[1] != 'K') {
            error = "下载到的 authlib-injector 不是有效的 jar 文件";
            return "";
        }

        if (!FileManager::SaveFile(jarPath, jar.body)) {
            error = "写入 authlib-injector 失败，请检查磁盘权限";
            return "";
        }
        return StringUtils::WideToUtf8(jarPath);
    }
};

} // namespace Core
