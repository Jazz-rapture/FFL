#pragma once
#include <string>
#include <Windows.h>
#include <winhttp.h>
#include <sstream>
#include "Models/DownloadTypes.h"

#pragma comment(lib, "winhttp.lib")

namespace Core {

class HttpClient {
public:
    // HTTP GET 请求
    static Models::HttpResult HttpGet(const std::wstring& url, const std::string& ifModifiedSince = "") {
        Models::HttpResult result;

        // 解析 URL
        URL_COMPONENTS urlComp = {};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.dwSchemeLength = 1;
        urlComp.dwHostNameLength = 1;
        urlComp.dwUrlPathLength = 1;
        urlComp.dwExtraInfoLength = 1;

        std::vector<wchar_t> urlBuf(url.begin(), url.end());
        urlBuf.push_back(L'\0');
        if (!WinHttpCrackUrl(urlBuf.data(), 0, 0, &urlComp)) {
            WriteHttpLog("URL解析失败: " + WideToUtf8(url));
            return result;
        }

        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        if (urlComp.dwExtraInfoLength > 0) {
            path += std::wstring(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
        }

        std::string urlStr = WideToUtf8(url);
        WriteHttpLog("发起请求: " + urlStr);

        HINTERNET hSession = WinHttpOpen(L"TauriCPP-Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            DWORD err = GetLastError();
            WriteHttpLog("创建会话失败, 错误码: " + std::to_string(err));
            return result;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                             urlComp.nPort, 0);
        if (!hConnect) {
            DWORD err = GetLastError();
            WriteHttpLog("连接失败: " + std::string(host.begin(), host.end()) + ", 错误码: " + std::to_string(err));
            WinHttpCloseHandle(hSession);
            return result;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                                 nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hRequest) {
            DWORD err = GetLastError();
            WriteHttpLog("创建请求失败, 错误码: " + std::to_string(err));
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        // 设置超时: 连接10秒, 发送10秒, 接收30秒
        DWORD connectTimeout = 10000;  // 10秒连接超时
        DWORD sendTimeout = 10000;     // 10秒发送超时
        DWORD receiveTimeout = 30000;  // 30秒接收超时

        WinHttpSetOption(hSession, WINHTTP_OPTION_CONNECT_TIMEOUT, &connectTimeout, sizeof(connectTimeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SEND_TIMEOUT, &sendTimeout, sizeof(sendTimeout));
        WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receiveTimeout, sizeof(receiveTimeout));

        // 添加 If-Modified-Since 头
        if (!ifModifiedSince.empty()) {
            std::wstring header = L"If-Modified-Since: " +
                std::wstring(ifModifiedSince.begin(), ifModifiedSince.end());
            WinHttpAddRequestHeaders(hRequest, header.c_str(), (DWORD)header.size(), WINHTTP_ADDREQ_FLAG_ADD);
        }

        BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (!sent) {
            DWORD err = GetLastError();
            WriteHttpLog("发送请求失败, 错误码: " + std::to_string(err));
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        BOOL received = WinHttpReceiveResponse(hRequest, nullptr);
        if (!received) {
            DWORD err = GetLastError();
            WriteHttpLog("接收响应失败, 错误码: " + std::to_string(err));
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        // 获取状态码
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        result.status = (int)statusCode;

        // 获取内容长度
        DWORD contentLength = 0;
        DWORD clSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &clSize, WINHTTP_NO_HEADER_INDEX);

        WriteHttpLog("响应状态码: " + std::to_string(result.status) +
                     ", 内容长度: " + std::to_string(contentLength) + " 字节");

        // 获取 Last-Modified 头
        DWORD lastModSize = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LAST_MODIFIED, WINHTTP_HEADER_NAME_BY_INDEX,
                             WINHTTP_NO_OUTPUT_BUFFER, &lastModSize, 0);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && lastModSize > 0) {
            std::vector<wchar_t> lastModBuf(lastModSize / sizeof(wchar_t) + 1, 0);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LAST_MODIFIED, WINHTTP_HEADER_NAME_BY_INDEX,
                                 lastModBuf.data(), &lastModSize, WINHTTP_NO_HEADER_INDEX);
            result.lastModified = std::string(lastModBuf.begin(), lastModBuf.end());
        }

        // 304 Not Modified
        if (result.status == 304) {
            WriteHttpLog("资源未修改 (304)");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        // 读取响应体
        std::string body;
        DWORD totalBytesRead = 0;
        DWORD bytesAvailable = 0;
        int chunkCount = 0;

        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
                DWORD err = GetLastError();
                WriteHttpLog("读取数据失败, 错误码: " + std::to_string(err));
                break;
            }
            body.append(buf.data(), bytesRead);
            totalBytesRead += bytesRead;
            chunkCount++;
            bytesAvailable = 0;

            // 每10个chunk或每100KB输出一次进度
            if (chunkCount % 10 == 0 || totalBytesRead % (100 * 1024) < bytesRead) {
                WriteHttpLog("下载进度: " + std::to_string(totalBytesRead) + " / " +
                            std::to_string(contentLength) + " 字节 (" +
                            std::to_string(contentLength > 0 ? (totalBytesRead * 100 / contentLength) : 0) + "%)");
            }
        }

        result.body = body;
        WriteHttpLog("下载完成: 共 " + std::to_string(totalBytesRead) + " 字节, " +
                     std::to_string(chunkCount) + " 个数据块");

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

private:
    // 宽字符串转UTF-8
    static std::string WideToUtf8(const std::wstring& wide) {
        if (wide.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "";
        std::string result(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), result.data(), size, nullptr, nullptr);
        return result;
    }

    // 写入HTTP日志
    static void WriteHttpLog(const std::string& message) {
        std::wstring logDir = L".minecraft\\logs";
        CreateDirectoryW(logDir.c_str(), nullptr);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char logName[64];
        snprintf(logName, sizeof(logName), "http_%04d%02d%02d.log", st.wYear, st.wMonth, st.wDay);

        // 获取可执行文件目录
        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir(exePath);
        size_t lastSlash = exeDir.find_last_of(L'\\');
        if (lastSlash != std::wstring::npos) {
            exeDir = exeDir.substr(0, lastSlash);
        }

        std::wstring logPath = exeDir + L"\\" + logDir + L"\\" + std::wstring(logName, logName + strlen(logName));

        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp), "[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

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
};

} // namespace Core
