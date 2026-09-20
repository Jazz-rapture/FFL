#pragma once
#include <string>
#include <thread>
#include "Core/ConfigManager.h"
#include "Core/HttpClient.h"
#include "Core/FileManager.h"
#include "Core/DownloadSourceManager.h"
#include "Core/StringUtils.h"
#include "Models/DownloadTypes.h"

namespace Services {

class ManifestService {
public:
    // 启动时后台同步版本清单
    static void StartSync() {
        std::thread(SyncVersionManifest).detach();
    }

private:
    // 获取 Last-Modified 时间
    static std::string GetLocalLastModified() {
        std::wstring wpath = Core::ConfigManager::GetManifestPath();
        int mbLen = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (mbLen <= 0) return "";
        std::string path(mbLen, 0);
        WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, path.data(), mbLen, nullptr, nullptr);
        path.pop_back();

        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return "";

        FILETIME ftWrite;
        if (!GetFileTime(hFile, nullptr, nullptr, &ftWrite)) {
            CloseHandle(hFile);
            return "";
        }
        CloseHandle(hFile);

        SYSTEMTIME st;
        FileTimeToSystemTime(&ftWrite, &st);

        char buf[128];
        static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
                 days[st.wDayOfWeek], st.wDay, months[st.wMonth - 1], st.wYear,
                 st.wHour, st.wMinute, st.wSecond);
        return std::string(buf);
    }

    // 同步版本清单
    static void SyncVersionManifest() {
        std::string local = Core::ConfigManager::ReadLocalManifest();

        // 根据下载源配置获取版本清单URL列表（支持多节点故障转移）
        std::vector<std::string> manifestUrls = Core::DownloadSourceManager::GetMojangMetaMirrorUrls("mc/game/version_manifest_v2.json");

        if (local.empty()) {
            for (const auto& url : manifestUrls) {
                Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
                if (res.status == 200 && !res.body.empty()) {
                    Core::ConfigManager::WriteLocalManifest(res.body);
                    return;
                }
            }
            return;
        }

        std::string lastMod = GetLocalLastModified();
        for (const auto& url : manifestUrls) {
            Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url), lastMod);
            if (res.status == 200 && !res.body.empty()) {
                Core::ConfigManager::WriteLocalManifest(res.body);
                return;
            }
        }
    }
};

} // namespace Services
