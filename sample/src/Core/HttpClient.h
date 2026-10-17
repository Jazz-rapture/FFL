#pragma once
#include <string>
#include <vector>
#include <utility>
#include <Windows.h>
#include <winhttp.h>
#include <sstream>
#include "Models/DownloadTypes.h"

#pragma comment(lib, "winhttp.lib")

namespace Core {

class HttpClient {
public:
    // 读到一块数据就回调一次已读字节数。单连接下载靠它才能把进度报出去
    using ByteCallback = std::function<void(long long)>;

    // HTTP GET 请求
    static Models::HttpResult HttpGet(const std::wstring& url, const std::string& ifModifiedSince = "",
                                      ByteCallback on_bytes = nullptr) {
        std::vector<std::pair<std::wstring, std::wstring>> headers;
        if (!ifModifiedSince.empty()) {
            headers.emplace_back(L"If-Modified-Since", Utf8ToWide(ifModifiedSince));
        }
        return Send(L"GET", url, headers, "", on_bytes);
    }

    // 表单提交：OAuth 令牌端点要求 Content-Type: application/x-www-form-urlencoded
    static Models::HttpResult HttpPostForm(const std::wstring& url, const std::string& form) {
        return Send(L"POST", url,
                    {{L"Content-Type", L"application/x-www-form-urlencoded"}}, form);
    }

    // JSON 提交：Xbox Live / XSTS 要求 Content-Type 与 Accept 都是 application/json
    static Models::HttpResult HttpPostJson(const std::wstring& url, const std::string& json) {
        return Send(L"POST", url,
                    {{L"Content-Type", L"application/json"}, {L"Accept", L"application/json"}}, json);
    }

    // 分段下载：只取 [from, to] 这段字节（闭区间）。
    // status == 206 表示服务端确实按 Range 返回；如果返回 200，说明它忽略了
    // Range、body 是整个文件，调用方必须降级成单连接下载。
    // 有了它，一个大文件才能拆给多个连接并发拉，而不是让整个任务卡在一个连接上。
    // on_bytes 会在读的过程中回报字节数 —— 服务端忽略 Range 直接甩回整个文件时，
    // 这条路径就是一次完整的单连接下载，不接回调的话进度会全程不动。
    static Models::HttpResult HttpGetRange(const std::wstring& url, long long from, long long to,
                                           ByteCallback on_bytes = nullptr) {
        if (from < 0 || to < from) return Models::HttpResult{};
        const std::wstring range = L"bytes=" + std::to_wstring(from) + L"-" + std::to_wstring(to);
        return Send(L"GET", url, {{L"Range", range}}, "", on_bytes);
    }

    // 带 Bearer 令牌的 GET（Minecraft 服务的档案 / 拥有情况查询）
    static Models::HttpResult HttpGetBearer(const std::wstring& url, const std::string& token) {
        return Send(L"GET", url,
                    {{L"Authorization", L"Bearer " + Utf8ToWide(token)},
                     {L"Accept", L"application/json"}}, "");
    }

    // 带 Bearer 令牌的表单提交（SakuraFrp 创建 / 删除隧道等写操作）
    static Models::HttpResult HttpPostFormBearer(const std::wstring& url, const std::string& token,
                                                 const std::string& form) {
        return Send(L"POST", url,
                    {{L"Authorization", L"Bearer " + Utf8ToWide(token)},
                     {L"Accept", L"application/json"},
                     {L"Content-Type", L"application/x-www-form-urlencoded"}}, form);
    }

private:
    // 所有请求共用的收发流程
    static Models::HttpResult Send(const std::wstring& verb, const std::wstring& url,
                                   const std::vector<std::pair<std::wstring, std::wstring>>& headers,
                                   const std::string& body, ByteCallback on_bytes = nullptr) {
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
            WriteHttpLog("连接失败: " + WideToUtf8(host) + ", 错误码: " + std::to_string(err));
            WinHttpCloseHandle(hSession);
            return result;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, verb.c_str(), path.c_str(),
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

        // 组装附加请求头（每个都要以 \r\n 结尾）
        std::wstring headerBlock;
        for (const auto& header : headers) {
            headerBlock += header.first + L": " + header.second + L"\r\n";
        }

        BOOL sent = WinHttpSendRequest(hRequest,
                                        headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
                                        headerBlock.empty() ? 0 : (DWORD)headerBlock.size(),
                                        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                        (DWORD)body.size(), (DWORD)body.size(), 0);
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
            result.lastModified = WideToUtf8(std::wstring(lastModBuf.begin(), lastModBuf.end()));
        }

        // 抓取全部响应头（CRLF 分隔）。外置登录要靠 X-Authlib-Injector-API-Location
        // 定位真正的 API 根地址；这个头在 3xx 响应上也有，所以要放在 304 早退之前
        DWORD rawHeaderSize = 0;
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                             WINHTTP_NO_OUTPUT_BUFFER, &rawHeaderSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && rawHeaderSize > 0) {
            std::vector<wchar_t> rawHeaderBuf(rawHeaderSize / sizeof(wchar_t) + 1, 0);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, rawHeaderBuf.data(),
                                    &rawHeaderSize, WINHTTP_NO_HEADER_INDEX)) {
                ParseHeaders(WideToUtf8(std::wstring(rawHeaderBuf.begin(), rawHeaderBuf.end())),
                             result.headers);
            }
        }

        // 304 Not Modified
        if (result.status == 304) {
            WriteHttpLog("资源未修改 (304)");
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return result;
        }

        // 读取响应体。非 2xx 也要读：OAuth 端点的错误详情就在 body 里
        std::string bodyOut;
        DWORD totalBytesRead = 0;
        DWORD bytesAvailable = 0;
        int chunkCount = 0;
        bool stalled = false;

        while (true) {
            // 低速看门狗。WinHTTP 的接收超时是按"单次读"算的，一条每秒只吐
            // 几百字节的连接永远不会触发它 —— 但整个任务会一直吊在上面（实测
            // 卡过 40 秒以上，看起来就是"卡死"）。所以这里自己量一下拿到这批
            // 数据花了多久：
            //   等了 5 秒以上，且耗时（毫秒）已经超过这批数据的字节数
            // 等价于速度低于 1 KB/s，直接判定为卡死。
            // 阈值与 PCL 一致（ModNet.vb:1030）。
            const ULONGLONG wait_start = GetTickCount64();
            const BOOL ok = WinHttpQueryDataAvailable(hRequest, &bytesAvailable);
            const ULONGLONG waited = GetTickCount64() - wait_start;
            if (!ok || bytesAvailable == 0) break;
            if (waited > 5000 && waited > static_cast<ULONGLONG>(bytesAvailable)) {
                WriteHttpLog("连接速度过低（" + std::to_string(waited) + "ms 只拿到 " +
                             std::to_string(bytesAvailable) + " 字节），主动断开重试");
                stalled = true;
                break;
            }

            std::vector<char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            if (!WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
                DWORD err = GetLastError();
                WriteHttpLog("读取数据失败, 错误码: " + std::to_string(err));
                break;
            }
            bodyOut.append(buf.data(), bytesRead);
            totalBytesRead += bytesRead;
            chunkCount++;
            bytesAvailable = 0;

            // 单连接下载靠这个回调把"已经下到多少"报给调用方，
            // 否则整个文件下完之前进度是完全不动的（速度会显示成 0）
            if (on_bytes && bytesRead > 0) {
                try {
                    on_bytes(static_cast<long long>(bytesRead));
                } catch (...) {
                }
            }

            // 每10个chunk或每100KB输出一次进度
            // 注意：乘 100 必须用 64 位，大文件（>42MB）时 totalBytesRead*100
            // 会溢出 DWORD，日志里就会出现 99% 被算成 31% 这种鬼数字
            if (chunkCount % 10 == 0 || totalBytesRead % (100 * 1024) < bytesRead) {
                const unsigned long long percent =
                    contentLength > 0
                        ? (static_cast<unsigned long long>(totalBytesRead) * 100ULL / contentLength)
                        : 0ULL;
                WriteHttpLog("下载进度: " + std::to_string(totalBytesRead) + " / " +
                            std::to_string(contentLength) + " 字节 (" +
                            std::to_string(percent) + "%)");
            }
        }

        result.body = bodyOut;

        // 截断检测：读取循环在接收超时/连接中断时会直接结束，但状态码还是 200。
        // 不拦的话调用方拿到的是一段残缺数据 —— 有 sha1 时会被校验拦下（整包失败），
        // 没有 sha1 时就会把残文件当成下载成功写进磁盘。
        // 低速被掐也算失败，同样按 status=0 上报，让调用方去试下一个下载源
        if (stalled) {
            result.body.clear();
            result.status = 0;
        } else if (contentLength > 0 && totalBytesRead != contentLength) {
            WriteHttpLog("响应体不完整（截断）: 已读 " + std::to_string(totalBytesRead) +
                         " / 应为 " + std::to_string(contentLength) + " 字节");
            result.body.clear();
            result.status = 0;
        } else {
            WriteHttpLog("下载完成: 共 " + std::to_string(totalBytesRead) + " 字节, " +
                         std::to_string(chunkCount) + " 个数据块");
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    // 把 "HTTP/1.1 200 OK\r\nName: value\r\nName2: value2\r\n" 拆成键值对。
    // 首行是状态行，跳过；名字保留服务端原始大小写（取值时统一按大小写不敏感比对）
    static void ParseHeaders(const std::string& raw,
                             std::vector<std::pair<std::string, std::string>>& out) {
        size_t pos = 0;
        bool firstLine = true;
        while (pos < raw.size()) {
            const size_t eol = raw.find("\r\n", pos);
            const std::string line =
                raw.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos = (eol == std::string::npos) ? raw.size() : eol + 2;

            // 结尾的 '\0' 会让最后一行是个空串，这里一并跳过
            if (firstLine) {
                firstLine = false;
                continue;
            }
            if (line.empty()) continue;

            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;

            const std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            const size_t firstNonSpace = value.find_first_not_of(" \t");
            value = (firstNonSpace == std::string::npos) ? "" : value.substr(firstNonSpace);
            out.emplace_back(name, value);
        }
    }

    // 宽字符串转UTF-8
    static std::string WideToUtf8(const std::wstring& wide) {        if (wide.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "";
        std::string result(size, 0);
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), result.data(), size, nullptr, nullptr);
        return result;
    }

    // UTF-8 转宽字符串
    static std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
        if (size <= 0) return L"";
        std::wstring result(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), result.data(), size);
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
