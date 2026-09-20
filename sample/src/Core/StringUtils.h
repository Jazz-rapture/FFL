#pragma once
#include <string>
#include <Windows.h>

namespace Core {

class StringUtils {
public:
    // UTF-8 -> wstring
    static std::wstring Utf8ToWide(const std::string& str) {
        if (str.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), len);
        result.pop_back();
        return result;
    }

    // wstring -> UTF-8
    static std::string WideToUtf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, result.data(), len, nullptr, nullptr);
        result.pop_back();
        return result;
    }
};

} // namespace Core
