#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include "StringUtils.h"

namespace Core {

class FileManager {
public:
    // 获取 exe 同目录路径
    static std::wstring GetExeDir() {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        WCHAR* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = L'\0';
        return exePath;
    }

    // 递归创建目录（类似 mkdir -p）
    static void CreateDirRecursive(const std::wstring& path) {
        DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return;

        size_t pos = path.find_last_of(L'\\');
        if (pos != std::wstring::npos && pos > 2) {
            CreateDirRecursive(path.substr(0, pos));
        }
        CreateDirectoryW(path.c_str(), nullptr);
    }

    // 保存文件到磁盘
    static bool SaveFile(const std::wstring& path, const std::string& content) {
        size_t pos = path.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            CreateDirRecursive(path.substr(0, pos));
        }

        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) return false;

        DWORD written = 0;
        BOOL ok = WriteFile(hFile, content.c_str(), (DWORD)content.size(), &written, nullptr);
        CloseHandle(hFile);
        return ok && written == (DWORD)content.size();
    }

    // 递归删除目录（类似 rm -rf）
    static bool RemoveDirectoryRecursive(const std::wstring& dirPath) {
        WIN32_FIND_DATAW findData;
        std::wstring searchPath = dirPath + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return false;

        do {
            std::wstring name = findData.cFileName;
            if (name == L"." || name == L"..") continue;

            std::wstring fullPath = dirPath + L"\\" + name;
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryRecursive(fullPath);
            } else {
                DWORD attrs = GetFileAttributesW(fullPath.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
                    SetFileAttributesW(fullPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
                }
                DeleteFileW(fullPath.c_str());
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
        return RemoveDirectoryW(dirPath.c_str()) != 0;
    }

    // 检查文件是否存在
    static bool FileExists(const std::wstring& path) {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES;
    }
};

} // namespace Core
