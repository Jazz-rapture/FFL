#pragma once
// ZipExtractor.h - ZIP 解压工具
// 使用 PowerShell 作为解压方案（简单可靠）

#include <string>
#include <vector>
#include <functional>
#include <windows.h>

namespace Core {

class ZipExtractor {
public:
    // 文件过滤器
    using FileFilter = std::function<bool(const std::wstring& filename)>;
    
    // 解压结果
    struct ExtractResult {
        bool success = false;
        int filesExtracted = 0;
        std::string errorMessage;
    };
    
    // 解压 JAR 文件中的动态库文件
    static ExtractResult ExtractNativeLibraries(const std::wstring& jarPath, const std::wstring& outputDir, 
                                               FileFilter filter = nullptr) {
        ExtractResult result;
        
        // 构建 PowerShell 命令
        std::wstring psCmd = L"powershell -NoProfile -Command \""
                            L"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                            L"$jar = [IO.Compression.ZipFile]::OpenRead('" + jarPath + L"'); "
                            L"$dlls = $jar.Entries | Where-Object { $_.Name -match '\\.(dll|so|dylib)$' }; "
                            L"$dlls | ForEach-Object { "
                            L"  [IO.Compression.ZipFileExtensions]::ExtractToFile($_, '" + outputDir + L"/' + $_.Name, $true) "
                            L"}; "
                            L"$jar.Dispose(); "
                            L"Write-Output ('Extracted ' + $dlls.Count + ' files')\"";
        
        // 执行 PowerShell 命令
        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        
        PROCESS_INFORMATION pi = {};
        
        // 创建可修改的命令行缓冲区
        std::vector<wchar_t> cmdLine(psCmd.begin(), psCmd.end());
        cmdLine.push_back(L'\0');
        
        if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000);
            
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            
            if (exitCode == 0) {
                result.success = true;
                result.filesExtracted = 1;
            } else {
                result.errorMessage = "PowerShell extraction failed with code: " + std::to_string(exitCode);
            }
        } else {
            result.errorMessage = "Failed to start PowerShell";
        }
        
        return result;
    }
};

} // namespace Core
