#pragma once
#include <string>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/StringUtils.h"
#include "Core/FileManager.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

class JavaService {
public:
    JavaService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    void RegisterCommands() {
        // java.scan - 扫描 Java 安装
        bridge_.RegisterCommand("java.scan", [](const nlohmann::json&) -> nlohmann::json {
            return HandleJavaScan();
        });

        // java.select - 选择 Java
        bridge_.RegisterCommand("java.select", [](const nlohmann::json& args) -> nlohmann::json {
            return HandleJavaSelect(args);
        });

        // java.get - 获取已选 Java
        bridge_.RegisterCommand("java.get", [](const nlohmann::json&) -> nlohmann::json {
            return HandleJavaGet();
        });
    }

    // 运行 java -version 获取版本号
    static std::string GetJavaVersion(const std::wstring& javaPath) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
        HANDLE hReadPipe, hWritePipe;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return "";

        STARTUPINFOW si = { sizeof(STARTUPINFOW) };
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi = {};
        std::wstring cmd = L"\"" + javaPath + L"\" -version";

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return "";
        }

        CloseHandle(hWritePipe);

        std::string output;
        char buf[1024] = {0};
        DWORD bytesRead = 0;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            buf[bytesRead] = '\0';
            output += buf;
        }
        CloseHandle(hReadPipe);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        size_t pos = output.find("version \"");
        if (pos == std::string::npos) return "";
        pos += 9;
        size_t end = output.find("\"", pos);
        if (end == std::string::npos) return "";
        return output.substr(pos, end - pos);
    }

    // 检查目录下是否有 java.exe
    static bool CheckJavaInDir(const std::wstring& dir, nlohmann::json& results) {
        std::wstring javaPath = dir + L"\\bin\\java.exe";
        DWORD attr = GetFileAttributesW(javaPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) return false;

        std::wstring folderName = dir;
        size_t pos = folderName.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            folderName = folderName.substr(pos + 1);
        }

        std::string version = GetJavaVersion(javaPath);
        results.push_back({
            {"path", Core::StringUtils::WideToUtf8(javaPath)},
            {"name", Core::StringUtils::WideToUtf8(folderName)},
            {"version", version}
        });
        return true;
    }

    // 扫描指定目录下的 java.exe
    static void ScanJavaInDir(const std::wstring& dir, nlohmann::json& results) {
        if (CheckJavaInDir(dir, results)) return;

        WIN32_FIND_DATAW findData;
        std::wstring searchPath = dir + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring name = findData.cFileName;
                if (name == L"." || name == L"..") continue;

                std::wstring subDir = dir + L"\\" + name;
                if (CheckJavaInDir(subDir, results)) continue;

                std::wstring searchPath2 = subDir + L"\\*";
                WIN32_FIND_DATAW findData2;
                HANDLE hFind2 = FindFirstFileW(searchPath2.c_str(), &findData2);
                if (hFind2 != INVALID_HANDLE_VALUE) {
                    do {
                        if (findData2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            std::wstring name2 = findData2.cFileName;
                            if (name2 == L"." || name2 == L"..") continue;
                            std::wstring subDir2 = subDir + L"\\" + name2;
                            CheckJavaInDir(subDir2, results);
                        }
                    } while (FindNextFileW(hFind2, &findData2));
                    FindClose(hFind2);
                }
            }
        } while (FindNextFileW(hFind, &findData));

        FindClose(hFind);
    }

    // 前端 bridge: 扫描 Java
    static nlohmann::json HandleJavaScan() {
        nlohmann::json results = nlohmann::json::array();

        WCHAR programFiles[MAX_PATH];
        GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);

        std::wstring javaDir = std::wstring(programFiles) + L"\\Java";
        ScanJavaInDir(javaDir, results);
        ScanJavaInDir(programFiles, results);

        WCHAR programFilesX86[MAX_PATH];
        GetEnvironmentVariableW(L"ProgramFiles(x86)", programFilesX86, MAX_PATH);
        if (std::wstring(programFiles) != std::wstring(programFilesX86)) {
            javaDir = std::wstring(programFilesX86) + L"\\Java";
            ScanJavaInDir(javaDir, results);
            ScanJavaInDir(programFilesX86, results);
        }

        return {{"ok", true}, {"javas", results}};
    }

    // 前端 bridge: 选择 Java
    static nlohmann::json HandleJavaSelect(const nlohmann::json& args) {
        std::string javaPath = args.value("path", "");

        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            config = {{"accounts", nlohmann::json::array()}};
        }

        if (!config.contains("game") || !config["game"].is_object()) {
            config["game"] = {};
        }
        config["game"]["javaPath"] = javaPath;

        bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
        return {{"ok", ok}};
    }

    // 前端 bridge: 获取已选 Java
    static nlohmann::json HandleJavaGet() {
        std::string content = Core::ConfigManager::ReadConfigFile();
        nlohmann::json config;
        try {
            config = nlohmann::json::parse(content);
        } catch (...) {
            return {{"ok", true}, {"path", ""}};
        }

        std::string javaPath = "";
        if (config.contains("game") && config["game"].is_object()) {
            javaPath = config["game"].value("javaPath", "");
        }
        return {{"ok", true}, {"path", javaPath}};
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
