#pragma once
// ModpackService.h - 整合包安装（Modrinth .mrpack / CurseForge .zip）
//
// 逻辑对齐 PCL 的 Mods/Minecraft/ModModpack.vb，落盘布局也按 PCL 的自包含模式来：
//
//   versions\<实例名>\<实例名>.json      ← 原版 + 加载器静态合并后的完整 JSON
//   versions\<实例名>\<mc版本>.jar       ← 原版客户端 jar
//   versions\<实例名>\<mc版本>.json      ← 安装期的中间产物，合并完即删除
//   versions\<实例名>\(mods|config|...)  ← 整合包 overrides + 下载的文件
//
// 实例目录里自带版本 JSON 和游戏核心 jar，不依赖 versions\<mc版本>\ 还能不能存在，
// 整个文件夹可以搬走。libraries / assets 仍然全局共享（PCL 也是这样）。
//
// 原版与加载器的安装不另起炉灶，直接复用 DownloadService 的下载管线
// （RunStep）和 ModLoaderService 的安装实现（InstallLoaderSync），
// 否则装出来的东西启动器自己认不出来。

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Bridge/WebviewBridge.h"
#include "Core/ConfigManager.h"
#include "Core/DownloadSourceManager.h"
#include "Core/FileManager.h"
#include "Core/HashUtils.h"
#include "Core/HttpClient.h"
#include "Core/SegmentedDownloader.h"
#include "Core/StringUtils.h"
#include "Core/ZipReader.h"
#include "Models/DownloadTypes.h"
#include "Services/DownloadService.h"
#include "Services/ModLoaderService.h"
#include "Services/VersionService.h"

namespace Services {

// 外部 JSON 的安全取值（定义在本文件末尾）。类内联的成员函数体也要用到，
// 所以必须先声明——默认实参只写在声明这里
inline std::string JsonStr(const nlohmann::json& obj, const char* key,
                           const std::string& fallback = std::string());
inline long long JsonInt(const nlohmann::json& obj, const char* key, long long fallback = 0);
inline bool JsonBool(const nlohmann::json& obj, const char* key, bool fallback);

class ModpackService {
public:
    ModpackService(Bridge::WebviewBridge& bridge, DownloadService& download, ModLoaderService& loader)
        : bridge_(bridge), download_(download), loader_(loader) {
        RegisterCommands();
    }

    void SetApp(tauricpp::App* app) { app_ = app; }

private:
    // ==================== 整合包描述 ====================

    enum class PackKind { Unknown, CurseForge, Modrinth };

    struct ModFile {
        std::string name;                    // 展示用（文件名）
        std::vector<std::string> urls;       // 候选下载地址，[0] 优先
        std::string relative_path;           // 相对实例目录，'/' 分隔
        std::string sha1;                    // 空则跳过校验
        long long size = 0;
        bool optional = false;
    };

    struct PackInfo {
        PackKind kind = PackKind::Unknown;
        std::string base_folder;             // "" 或 "子目录/"
        std::string name;                    // 整合包自报的名字
        std::string mc_version;
        std::string loader_type;             // forge / neoforge / fabric / quilt / 空
        std::string loader_version;
        std::vector<std::string> override_prefixes;  // 需要解压的 overrides 前缀
        std::vector<ModFile> files;
        std::vector<std::string> unsupported;  // 包内声明了但我们装不了的加载器
        // 接口没返回的文件（作者删档）—— 没法反推地址，只能让用户决定要不要继续
        std::vector<std::string> missing_files;
        // CurseForge 在识别阶段不联网，拿到的只是 manifest 里声明的文件数
        int declared_files = 0;
        int optional_count = 0;
        long long total_size = 0;
    };

    // ==================== 命令注册 ====================

    void RegisterCommands() {
        // modpack.inspect - 解析整合包，返回确认卡片需要的信息（不落盘）
        bridge_.RegisterCommand("modpack.inspect", [this](const nlohmann::json& args) -> nlohmann::json {
            return HandleInspect(args);
        });

        // modpack.install - 开始安装（异步，结果与进度走 modpack.poll）
        bridge_.RegisterCommand("modpack.install", [this](const nlohmann::json& args) -> nlohmann::json {
            {
                std::lock_guard<std::mutex> lock(state_mtx_);
                if (running_) {
                    return {{"ok", false}, {"error", "已有整合包正在安装中"}};
                }
                running_ = true;
                cancel_requested_ = false;
                while (!results_.empty()) results_.pop();
                latest_progress_ = nlohmann::json::object();
            }
            std::thread([this, args]() { InstallWorker(args); }).detach();
            return {{"ok", true}, {"async", true}};
        });

        // modpack.poll - 轮询进度与结果
        bridge_.RegisterCommand("modpack.poll", [this](const nlohmann::json&) -> nlohmann::json {
            std::lock_guard<std::mutex> lock(state_mtx_);
            nlohmann::json response = {{"ok", true}, {"running", running_}};
            if (!latest_progress_.is_null()) response["progress"] = latest_progress_;
            if (!results_.empty()) {
                response["hasResult"] = true;
                response["result"] = results_.front();
                results_.pop();
            } else {
                response["hasResult"] = false;
            }
            return response;
        });

        // modpack.cancel - 请求取消（下载线程和装原版阶段都会检查这个标志）
        bridge_.RegisterCommand("modpack.cancel", [this](const nlohmann::json&) -> nlohmann::json {
            cancel_requested_ = true;
            // 正在等用户确认的话，顺带按"取消"处理，别让它一直挂着
            {
                std::lock_guard<std::mutex> lock(ask_mtx_);
                if (ask_pending_) {
                    ask_continue_ = false;
                    ask_answered_ = true;
                }
            }
            ask_cv_.notify_all();
            return {{"ok", true}};
        });

        // modpack.answer - 回答「内容缺失，是否继续安装」
        bridge_.RegisterCommand("modpack.answer", [this](const nlohmann::json& args) -> nlohmann::json {
            {
                std::lock_guard<std::mutex> lock(ask_mtx_);
                if (!ask_pending_) return {{"ok", false}, {"error", "当前没有待确认的询问"}};
                ask_continue_ = JsonBool(args, "continue", false);
                ask_answered_ = true;
            }
            ask_cv_.notify_all();
            return {{"ok", true}};
        });
    }

    // ==================== 询问用户 ====================
    // PCL 在工作线程上直接 MyMsgBox 阻塞，这里等价实现：发事件给前端弹框，
    // 然后在本线程上用条件变量等回答。超时按取消处理，避免前端没了以后永远卡住。
    bool AskContinue(const std::string& title, const std::string& message, int timeout_ms = 600000) {
        {
            std::lock_guard<std::mutex> lock(ask_mtx_);
            ask_pending_ = true;
            ask_answered_ = false;
            ask_continue_ = false;
        }
        if (app_) {
            try {
                app_->GetBridge().Emit("modpack.ask", {{"title", title}, {"message", message}});
            } catch (...) {
            }
        }

        std::unique_lock<std::mutex> lock(ask_mtx_);
        const bool answered = ask_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                               [this]() { return ask_answered_ || Cancelled(); });
        const bool proceed = answered && ask_answered_ && ask_continue_ && !Cancelled();
        ask_pending_ = false;
        ask_answered_ = false;
        return proceed;
    }

    // ==================== 状态 ====================

    void EmitProgress(const std::string& phase, const std::string& message, int percent,
                      const nlohmann::json& extra = nlohmann::json::object()) {
        nlohmann::json progress = {{"phase", phase}, {"message", message}, {"percent", percent}};
        for (auto it = extra.begin(); it != extra.end(); ++it) progress[it.key()] = it.value();
        Log("[" + phase + " " + std::to_string(percent) + "%] " + message);
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            latest_progress_ = progress;
        }
        if (app_) {
            try {
                app_->GetBridge().Emit("modpack.progress", progress);
            } catch (...) {
            }
        }
    }

    void PushResult(const nlohmann::json& result) {
        if (result.value("ok", false)) {
            Log("安装完成：" + result.value("instanceName", std::string("?")));
        } else {
            Log("安装失败：" + result.value("error", std::string("未知错误")));
        }
        std::lock_guard<std::mutex> lock(state_mtx_);
        results_.push(result);
        // 结果一出来就放开占用标记：前端万一没来 poll，
        // 下一次安装也不该被永久挡住
        running_ = false;
    }

    bool Cancelled() const { return cancel_requested_.load(); }

    // 阶段 -> 总体百分比。整合包安装是一条长流水线，前端只显示一个进度条，
    // 所以每段占多少需要事先分好
    static int PercentAt(const std::string& phase, int inner, int inner_max) {
        struct Range { const char* phase; int from; int to; };
        static const Range ranges[] = {
            {"extract", 0, 20},
            {"download", 20, 65},
            {"vanilla", 65, 88},
            {"loader", 88, 97},
            {"finalize", 97, 100},
        };
        for (const auto& r : ranges) {
            if (phase != r.phase) continue;
            if (inner_max <= 0) return r.to;
            int value = r.from + (r.to - r.from) * inner / inner_max;
            if (value < r.from) value = r.from;
            if (value > r.to) value = r.to;
            return value;
        }
        return inner;
    }

    static std::wstring MinecraftDir() {
        return Core::FileManager::GetExeDir() + L"\\.minecraft";
    }

    // 整合包安装是黑盒流程（拖进去就没有再交互的入口），出问题时没有日志
    // 基本查不动。沿用其它服务的习惯写到 .minecraft\logs\modpack.log
    static void Log(const std::string& message) {
        static std::mutex log_mtx;
        std::lock_guard<std::mutex> lock(log_mtx);

        std::wstring dir = MinecraftDir() + L"\\logs";
        Core::FileManager::CreateDirRecursive(dir);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);

        const std::string line = std::string(stamp) + message + "\r\n";
        HANDLE file = CreateFileW((dir + L"\\modpack.log").c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        CloseHandle(file);
    }

    static std::wstring Utf8ToWide(const std::string& s) { return Core::StringUtils::Utf8ToWide(s); }
    static std::string WideToUtf8(const std::wstring& s) { return Core::StringUtils::WideToUtf8(s); }

    // ==================== 识别与解析 ====================

    nlohmann::json HandleInspect(const nlohmann::json& args);
    void InstallWorker(const nlohmann::json& args);
    void InstallWorkerImpl(const nlohmann::json& args);

    // resolve_downloads：是否把 CurseForge 的 fileID 换成真实下载地址。
    // 这一步要联网，只在真正安装时做；识别阶段必须离线完成，
    // 否则网络一慢用户就会看到"拖进去没反应"
    bool ParsePack(const std::wstring& archive_path, PackInfo& info, std::string& error,
                   bool resolve_downloads);
    bool ParseModrinth(Core::ZipReader& zip, PackInfo& info, std::string& error);
    bool ParseCurseForge(Core::ZipReader& zip, PackInfo& info, std::string& error,
                         bool resolve_downloads);
    bool FetchCurseForgeFiles(const nlohmann::json& manifest_files, PackInfo& info, std::string& error);

    bool ExtractOverrides(Core::ZipReader& zip, const PackInfo& info,
                          const std::wstring& instance_dir, std::string& error);
    bool DownloadModFiles(const PackInfo& info, const std::wstring& instance_dir,
                          std::string& error, bool include_optional);
    // 原版装进实例目录，让实例自包含（对齐 PCL）：实例不再依赖 versions\<mc版本>\，
    // 删掉那份原版之后实例照样能启动，整个实例文件夹也可以直接搬走
    bool InstallVanilla(const PackInfo& info, const std::string& instance_name, std::string& error);
    bool InstallLoader(const PackInfo& info, const std::string& instance_name, std::string& error);
    bool FinalizeInstance(const PackInfo& info, const std::string& instance_name, std::string& error);

    // 实例名清洗：去掉 Windows 不允许的字符，并挡掉 ! 和 ;（本项目历史上的路径坑）
    static std::string SanitizeInstanceName(const std::string& raw);

    // ==================== 成员 ====================

    Bridge::WebviewBridge& bridge_;
    DownloadService& download_;
    ModLoaderService& loader_;
    tauricpp::App* app_ = nullptr;

    std::mutex state_mtx_;
    std::queue<nlohmann::json> results_;
    nlohmann::json latest_progress_ = nlohmann::json::object();
    bool running_ = false;
    std::atomic<bool> cancel_requested_{false};

    // 安装线程等前端回答「是否继续」用的同步原语
    std::mutex ask_mtx_;
    std::condition_variable ask_cv_;
    bool ask_pending_ = false;
    bool ask_answered_ = false;
    bool ask_continue_ = false;
};

// ============================================================================
// 小工具
// ============================================================================
inline std::string LowerAscii(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 归档里的相对路径一律规范成 '\\' 分隔、无前导分隔符，方便和磁盘路径拼
inline std::string NormalizeRelative(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (out.empty() || out.back() == '\\') continue;
            out.push_back('\\');
        } else {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return out;
}

// 路径里有没有 ".." 段。整合包是外来数据，落到版本目录之外的路径一律拒绝
// （PCL 用 IsParentOf 做同一件事）。只认独立的一段，"my..mod.jar" 不算
inline bool HasParentSegment(const std::string& relative) {
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find_first_of("\\/", start);
        if (end == std::string::npos) end = relative.size();
        if (end - start == 2 && relative[start] == '.' && relative[start + 1] == '.') return true;
        if (end >= relative.size()) break;
        start = end + 1;
    }
    return false;
}

inline std::string FileNameOf(const std::string& relative) {
    const size_t pos = relative.find_last_of('\\');
    return pos == std::string::npos ? relative : relative.substr(pos + 1);
}

inline bool HasPrefixInArchive(const Core::ZipReader& zip, const std::string& prefix) {
    for (const auto& entry : zip.Entries()) {
        if (entry.name.size() >= prefix.size() && entry.name.compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

// forgecdn 的三个下载域名换成 mcimirror 的代理（路径一样，只换主机名）
inline std::string RewriteForgeCdn(const std::string& url) {
    static const char* hosts[] = {
        "https://edge.forgecdn.net",
        "https://mediafilez.forgecdn.net",
        "https://media.forgecdn.net",
    };
    for (const char* host : hosts) {
        const std::string prefix(host);
        if (url.compare(0, prefix.size(), prefix) == 0) {
            return "https://mod.mcimirror.top" + url.substr(prefix.size());
        }
    }
    return url;
}

// URL 路径段编码（RFC 3986 unreserved 之外一律百分号编码）。
// 文件名里常见空格、§、中文方括号，不编码的话 CDN 会 404
inline std::string UrlEncodePathSegment(const std::string& raw) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 3);
    for (unsigned char c : raw) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// CurseForge 文件的候选下载地址。
//
// 接口正常会给 downloadUrl；但很多作者关闭了第三方下载，这时接口回的是
// null。PCL 的做法（ResourceVersion.vb:135）是**按 CDN 的目录规则反推**：
//   https://edge.forgecdn.net/files/{id/1000}/{id%1000}/{编码后的文件名}
// 实测这条路对这类文件是通的，所以不能因为 downloadUrl 为空就放弃。
// 注意 id%1000 不补零：files/6217/044/x 是 404，files/6217/44/x 才是 200。
inline std::vector<std::string> CurseForgeFileUrls(long long file_id, const std::string& file_name,
                                                   const std::string& api_download_url) {
    std::vector<std::string> urls;
    auto add = [&urls](const std::string& u) {
        if (u.empty()) return;
        for (const auto& seen : urls) {
            if (seen == u) return;
        }
        urls.push_back(u);
    };

    if (!api_download_url.empty()) {
        // 接口给了真实地址：镜像优先，官方兜底（forgecdn 在国内经常拉不动）
        add(RewriteForgeCdn(api_download_url));
        add(api_download_url);
        return urls;
    }

    if (file_id <= 0 || file_name.empty()) return urls;

    const std::string dir = std::to_string(file_id / 1000) + "/" +
                            std::to_string(file_id % 1000) + "/";
    const std::string encoded = UrlEncodePathSegment(file_name);

    // mcimirror 的两种形式都放进去：实测它对编码和不编码的名字都认，
    // 但不同文件名表现并不一致，多给一个候选几乎没有成本
    add("https://mod.mcimirror.top/files/" + dir + encoded);
    add("https://mod.mcimirror.top/files/" + dir + file_name);
    add("https://edge.forgecdn.net/files/" + dir + encoded);
    add("https://mediafilez.forgecdn.net/files/" + dir + encoded);
    return urls;
}

// 读一个 JSON 文件，读不到或解析失败都返回空 object（调用方自行判断字段）
inline nlohmann::json ReadJsonFile(const std::wstring& path) {
    if (!Core::FileManager::FileExists(path)) return nlohmann::json::object();
    std::ifstream in(path);
    if (!in.is_open()) return nlohmann::json::object();
    std::stringstream ss;
    ss << in.rdbuf();
    try {
        nlohmann::json parsed = nlohmann::json::parse(ss.str());
        return parsed.is_object() ? parsed : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

// ---------------------------------------------------------------------------
// 外部 JSON 的安全取值
//
// 整合包里的 manifest.json / modrinth.index.json、以及 CurseForge 接口的响应，
// 内容都不受我们控制：字段可能是 null、也可能是别的类型。
// nlohmann 的 obj.value("k", default) 只在"键不存在"时给默认值 —— 键存在但类型
// 不符（null 就是最常见的）会抛 type_error.302。这曾经把启动器直接打崩过一次：
// CurseForge 上有作者关闭了第三方下载，接口就回 "downloadUrl": null，
// 390 个文件里有 17 个是这样，取字符串时抛异常，而安装线程没有 catch，
// 异常一路冒到 std::terminate，进程死于 ucrtbase 的 0xc0000409。
// 所以凡是外部数据，一律走下面这三个函数。
// ---------------------------------------------------------------------------
inline std::string JsonStr(const nlohmann::json& obj, const char* key,
                           const std::string& fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

inline long long JsonInt(const nlohmann::json& obj, const char* key, long long fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return fallback;
    return it->get<long long>();
}

inline bool JsonBool(const nlohmann::json& obj, const char* key, bool fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

// ============================================================================
// 把加载器 JSON 静态合并进原版 JSON，产出一份不带 inheritsFrom 的完整版本 JSON
// （对齐 PCL 的 MergeJson）。
//
// 合并顺序必须和 GameLaunchService 运行时的合并保持一致：父版本在前、子版本在后。
// libraries 按 name 去重（后出现的覆盖先出现的）；arguments.game / arguments.jvm
// 直接拼接 —— 漏了任何一边都会出事：原版提供 --username / --gameDir，
// 加载器提供 --launchTarget / --fml.*，缺一头游戏都起不来。
//
// 其余字段（assetIndex / assets / downloads / logging / javaVersion）保持原版的值，
// 加载器只覆盖它自己声明的东西（mainClass、libraries、arguments，旧版还有
// minecraftArguments）。
// ============================================================================
inline nlohmann::json MergeVersionJson(const nlohmann::json& parent, const nlohmann::json& child) {
    nlohmann::json merged = parent.is_object() ? parent : nlohmann::json::object();
    const bool has_child = child.is_object() && !child.empty();

    // libraries：父在前、子在后，同名以子为准
    {
        nlohmann::json libs = nlohmann::json::array();
        std::map<std::string, size_t> by_name;
        auto append = [&libs, &by_name](const nlohmann::json& src) {
            if (!src.is_object() || !src.contains("libraries") || !src["libraries"].is_array()) return;
            for (const auto& lib : src["libraries"]) {
                const std::string name = lib.is_object() ? JsonStr(lib, "name") : std::string();
                if (name.empty()) {
                    libs.push_back(lib);
                    continue;
                }
                const auto it = by_name.find(name);
                if (it != by_name.end()) {
                    libs[it->second] = lib;
                    continue;
                }
                by_name[name] = libs.size();
                libs.push_back(lib);
            }
        };
        append(parent);
        append(child);
        if (!libs.empty()) merged["libraries"] = libs;
    }

    // arguments.game / arguments.jvm：父在前、子在后，直接拼接
    for (const char* key : {"game", "jvm"}) {
        nlohmann::json collected = nlohmann::json::array();
        auto append = [&collected, key](const nlohmann::json& src) {
            if (!src.is_object() || !src.contains("arguments") || !src["arguments"].is_object()) return;
            const auto& args = src["arguments"];
            if (!args.contains(key) || !args[key].is_array()) return;
            for (const auto& item : args[key]) collected.push_back(item);
        };
        append(parent);
        append(child);
        if (!collected.empty()) merged["arguments"][key] = collected;
    }

    if (has_child) {
        if (child.contains("mainClass")) merged["mainClass"] = child["mainClass"];
        // 旧版加载器（Forge ≤1.12.2）只有 minecraftArguments 一个字符串，
        // 且它已经是「原版参数 + Forge 追加」的完整一份。
        // GameLaunchService 只会用其中一份，所以子版本有就整个替换
        if (child.contains("minecraftArguments") && child["minecraftArguments"].is_string()) {
            merged["minecraftArguments"] = child["minecraftArguments"];
        }
    }

    // 合并完就不再需要继承了
    merged.erase("inheritsFrom");
    return merged;
}

// ============================================================================
// 识别：modrinth.index.json 优先于 manifest.json，根目录优先于一级子目录
// ============================================================================
inline bool ModpackService::ParsePack(const std::wstring& archive_path, PackInfo& info, std::string& error,
                                     bool resolve_downloads) {
    Core::ZipReader zip;
    if (!zip.Open(archive_path, error)) return false;

    std::vector<std::string> bases{""};
    std::set<std::string> seen;
    for (const auto& entry : zip.Entries()) {
        const size_t slash = entry.name.find('/');
        if (slash == std::string::npos) continue;
        std::string first = entry.name.substr(0, slash + 1);
        if (seen.insert(first).second) bases.push_back(first);
    }

    for (const auto& base : bases) {
        if (zip.HasEntry(base + "modrinth.index.json")) {
            info.kind = PackKind::Modrinth;
            info.base_folder = base;
            return ParseModrinth(zip, info, error);
        }
        if (zip.HasEntry(base + "manifest.json")) {
            std::string text;
            std::string read_error;
            if (!zip.ReadAll(base + "manifest.json", text, read_error)) {
                error = read_error;
                return false;
            }
            nlohmann::json manifest;
            try {
                manifest = nlohmann::json::parse(text);
            } catch (...) {
                error = "manifest.json 解析失败，整合包可能已损坏";
                return false;
            }
            // 带 addons 字段的是 MCBBS 整合包（第一版不支持）
            if (manifest.contains("addons")) {
                error = "暂不支持 MCBBS 格式的整合包，请改用 Modrinth 或 CurseForge 整合包";
                return false;
            }
            info.kind = PackKind::CurseForge;
            info.base_folder = base;
            return ParseCurseForge(zip, info, error, resolve_downloads);
        }
        if (zip.HasEntry(base + "mcbbs.packmeta")) {
            error = "暂不支持 MCBBS 格式的整合包，请改用 Modrinth 或 CurseForge 整合包";
            return false;
        }
        if (zip.HasEntry(base + "modpack.json")) {
            error = "暂不支持 HMCL 格式的整合包，请改用 Modrinth 或 CurseForge 整合包";
            return false;
        }
        if (zip.HasEntry(base + "mmc-pack.json")) {
            error = "暂不支持 MultiMC 格式的整合包，请改用 Modrinth 或 CurseForge 整合包";
            return false;
        }
    }

    error = "无法识别该整合包格式（压缩包内既没有 modrinth.index.json 也没有 manifest.json）";
    return false;
}

// ============================================================================
// Modrinth
// ============================================================================
inline bool ModpackService::ParseModrinth(Core::ZipReader& zip, PackInfo& info, std::string& error) {
    std::string text;
    if (!zip.ReadAll(info.base_folder + "modrinth.index.json", text, error)) return false;

    nlohmann::json index;
    try {
        index = nlohmann::json::parse(text);
    } catch (...) {
        error = "modrinth.index.json 解析失败，整合包可能已损坏";
        return false;
    }

    info.name = JsonStr(index, "name");
    if (info.name.empty()) info.name = JsonStr(index, "versionId");

    if (!index.contains("dependencies") || !index["dependencies"].is_object() ||
        !index["dependencies"].contains("minecraft")) {
        error = "Modrinth 整合包未提供 Minecraft 版本信息";
        return false;
    }

    const nlohmann::json& deps = index["dependencies"];
    info.mc_version = deps["minecraft"].is_string() ? deps["minecraft"].get<std::string>() : "";
    if (info.mc_version.empty()) {
        error = "Modrinth 整合包未提供 Minecraft 版本信息";
        return false;
    }

    for (auto it = deps.begin(); it != deps.end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::string key = LowerAscii(it.key());
        const std::string value = it.value().get<std::string>();
        if (key == "minecraft" || value.empty()) continue;
        if (key == "forge") {
            info.loader_type = "forge";
            info.loader_version = value;
        } else if (key == "neoforge" || key == "neo-forge") {
            info.loader_type = "neoforge";
            info.loader_version = value;
        } else if (key == "fabric-loader") {
            info.loader_type = "fabric";
            info.loader_version = value;
        } else if (key == "quilt-loader") {
            info.loader_type = "quilt";
            info.loader_version = value;
        } else {
            info.unsupported.push_back(it.key());
        }
    }

    if (index.contains("files") && index["files"].is_array()) {
        for (const auto& item : index["files"]) {
            if (!item.is_object()) continue;
            const std::string path = JsonStr(item, "path");
            if (path.empty()) continue;

            // env.client：unsupported 表示客户端用不着，optional 是可选安装
            bool optional = false;
            bool unsupported = false;
            if (item.contains("env") && item["env"].is_object()) {
                const std::string env = LowerAscii(JsonStr(item["env"], "client", "required"));
                unsupported = (env == "unsupported");
                optional = (env == "optional");
            }
            if (unsupported) continue;

            ModFile file;
            file.relative_path = NormalizeRelative(path);
            if (file.relative_path.empty()) continue;
            // 落地路径必须留在实例目录里
            if (HasParentSegment(file.relative_path)) continue;
            file.name = FileNameOf(file.relative_path);
            file.optional = optional;
            file.size = JsonInt(item, "fileSize");

            // 只认 sha1：项目里只有 SHA-1 实现，sha512 校验不了就不校验
            if (item.contains("hashes") && item["hashes"].is_object()) {
                file.sha1 = JsonStr(item["hashes"], "sha1");
            }
            if (item.contains("downloads") && item["downloads"].is_array()) {
                for (const auto& url : item["downloads"]) {
                    if (url.is_string()) file.urls.push_back(url.get<std::string>());
                }
            }
            if (file.urls.empty()) continue;

            if (optional) info.optional_count++;
            info.total_size += file.size;
            info.files.push_back(std::move(file));
        }
    }
    info.declared_files = static_cast<int>(info.files.size());

    // Modrinth 的客户端覆写目录有两种，两个都可能存在
    for (const char* name : {"overrides/", "client-overrides/"}) {
        const std::string prefix = info.base_folder + name;
        if (HasPrefixInArchive(zip, prefix)) info.override_prefixes.push_back(prefix);
    }
    return true;
}

// ============================================================================
// CurseForge
// ============================================================================
inline bool ModpackService::ParseCurseForge(Core::ZipReader& zip, PackInfo& info, std::string& error,
                                          bool resolve_downloads) {
    std::string text;
    if (!zip.ReadAll(info.base_folder + "manifest.json", text, error)) return false;

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(text);
    } catch (...) {
        error = "manifest.json 解析失败，整合包可能已损坏";
        return false;
    }

    info.name = JsonStr(manifest, "name");

    if (!manifest.contains("minecraft") || !manifest["minecraft"].is_object()) {
        error = "CurseForge 整合包未提供 Minecraft 版本信息";
        return false;
    }
    const nlohmann::json& mc = manifest["minecraft"];
    info.mc_version = JsonStr(mc, "version");
    if (info.mc_version.empty()) {
        error = "CurseForge 整合包未提供 Minecraft 版本信息";
        return false;
    }

    if (mc.contains("modLoaders") && mc["modLoaders"].is_array()) {
        for (const auto& entry : mc["modLoaders"]) {
            if (!entry.is_object()) continue;
            const std::string raw_id = JsonStr(entry, "id");
            const std::string id = LowerAscii(raw_id);
            if (id.rfind("forge-", 0) == 0) {
                // 早期整合包会写 forge-xxx-recommended，这种版本号无法安装
                if (id.find("recommended") != std::string::npos) {
                    error = "该整合包用的 Forge 版本号是 recommended 写法，年代过久，无法安装";
                    return false;
                }
                info.loader_type = "forge";
                info.loader_version = id.substr(6);
            } else if (id.rfind("neoforge-", 0) == 0) {
                info.loader_type = "neoforge";
                info.loader_version = id.substr(9);
            } else if (id.rfind("fabric-", 0) == 0) {
                info.loader_type = "fabric";
                info.loader_version = id.substr(7);
            } else if (id.rfind("quilt-", 0) == 0) {
                info.loader_type = "quilt";
                info.loader_version = id.substr(6);
            } else if (!id.empty()) {
                info.unsupported.push_back(raw_id);
            }
        }
    }

    // overrides 目录。没声明就是没有覆写文件 —— 不能退化成"整个压缩包"，
    // 那会把 manifest.json 之类的东西全倒进实例目录里
    std::string overrides = JsonStr(manifest, "overrides");
    if (overrides == "." || overrides == "./") overrides.clear();
    if (!overrides.empty()) {
        const std::string prefix = info.base_folder + overrides + "/";
        if (HasPrefixInArchive(zip, prefix)) {
            info.override_prefixes.push_back(prefix);
        } else if (info.base_folder.empty() && HasPrefixInArchive(zip, "overrides/")) {
            // 少数整合包声明了自定义名字但实际还是 overrides/
            info.override_prefixes.push_back("overrides/");
        }
    }

    // 文件清单里只有 projectID / fileID。可选文件的判定、文件总数在本地就能
    // 算出来；真实下载地址必须联网查，只在真正安装时才做
    const nlohmann::json files = manifest.contains("files") && manifest["files"].is_array()
                                     ? manifest["files"] : nlohmann::json::array();
    int declared = 0;
    for (const auto& entry : files) {
        if (!entry.is_object()) continue;
        if (JsonInt(entry, "fileID") <= 0) continue;
        declared++;
        if (!JsonBool(entry, "required", true)) info.optional_count++;
    }
    info.declared_files = declared;

    if (!resolve_downloads) return true;
    return FetchCurseForgeFiles(files, info, error);
}

// ============================================================================
// CurseForge：把 fileID 批量换成真实下载地址
// 走 mod.mcimirror.top 的代理端点 —— 官方 api.curseforge.com 需要 x-api-key，
// 这个镜像由对方服务端带 key 转发，PCL 也是这么做的
// ============================================================================
inline bool ModpackService::FetchCurseForgeFiles(const nlohmann::json& manifest_files,
                                                 PackInfo& info, std::string& error) {
    // fileID -> 是否可选 / 属于哪个工程（工程 ID 用来把缺失文件显示成可读名字）
    std::vector<long long> file_ids;
    std::set<long long> optional_ids;
    std::map<long long, long long> file_to_project;
    for (const auto& entry : manifest_files) {
        if (!entry.is_object()) continue;
        const long long file_id = JsonInt(entry, "fileID");
        if (file_id <= 0) continue;
        file_ids.push_back(file_id);
        if (!JsonBool(entry, "required", true)) optional_ids.insert(file_id);
        const long long project_id = JsonInt(entry, "projectID");
        if (project_id > 0) file_to_project[file_id] = project_id;
    }
    if (file_ids.empty()) return true;

    // 接口一次能吃不少，但别赌上限，500 一批
    std::vector<nlohmann::json> records;
    const size_t kChunk = 500;
    for (size_t start = 0; start < file_ids.size(); start += kChunk) {
        const size_t end = (std::min)(start + kChunk, file_ids.size());
        nlohmann::json body;
        body["fileIds"] = nlohmann::json::array();
        for (size_t i = start; i < end; i++) body["fileIds"].push_back(file_ids[i]);

        Models::HttpResult res = Core::HttpClient::HttpPostJson(
            L"https://mod.mcimirror.top/curseforge/v1/mods/files", body.dump());
        if (res.status != 200 || res.body.empty()) {
            error = "获取 CurseForge 文件信息失败（HTTP " + std::to_string(res.status) +
                    "），请检查网络连接";
            return false;
        }

        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(res.body);
        } catch (...) {
            error = "CurseForge 返回内容无法解析";
            return false;
        }
        if (!parsed.contains("data") || !parsed["data"].is_array()) continue;
        for (const auto& item : parsed["data"]) records.push_back(item);
    }

    // 一个文件都没查到多半是接口异常，而不是"整包 390 个文件全被删了"。
    // 不拦住的话会装出一个 mods 全空的版本，用户更难判断问题出在哪
    if (records.empty()) {
        error = "CurseForge 没有返回任何文件信息，可能是接口暂时不可用，请稍后重试";
        return false;
    }

    int url_derived = 0;
    std::set<long long> returned_ids;
    for (const auto& record : records) {
        if (!record.is_object()) continue;
        const long long file_id = JsonInt(record, "id");
        const std::string file_name = JsonStr(record, "fileName");
        const std::string download_url = JsonStr(record, "downloadUrl");
        // 先把 id 记下来：只要接口回了这一条，就不算"被作者删除"，
        // 哪怕后面的字段有问题也换一种说法提示，别误导用户
        if (file_id > 0) returned_ids.insert(file_id);
        if (file_name.empty()) continue;

        // 资源类型判定：优先看包内文件构成，退而看扩展名
        std::string folder = "mods";
        if (record.contains("modules") && record["modules"].is_array() && !record["modules"].empty()) {
            bool is_mod = false;
            bool is_resourcepack = false;
            bool is_shader = false;
            for (const auto& module : record["modules"]) {
                const std::string name = module.is_object() ? JsonStr(module, "name") : "";
                if (name == "META-INF" || name == "mcmod.info" || name == "fabric.mod.json") is_mod = true;
                else if (name == "pack.mcmeta") is_resourcepack = true;
                else if (name == "shaders") is_shader = true;
            }
            if (!is_mod && is_resourcepack) folder = "resourcepacks";
            else if (!is_mod && !is_resourcepack && is_shader) folder = "shaderpacks";
        }

        ModFile file;
        file.name = file_name;
        // fileName 在归档里就是最后一段，但保险起见还是清掉可能带进来的路径
        file.relative_path = folder + "\\" + FileNameOf(NormalizeRelative(file_name));
        file.optional = optional_ids.count(file_id) > 0;
        file.size = JsonInt(record, "fileLength");

        if (record.contains("hashes") && record["hashes"].is_array()) {
            for (const auto& hash : record["hashes"]) {
                // algo 1 = SHA-1，2 = MD5
                if (hash.is_object() && JsonInt(hash, "algo") == 1) {
                    file.sha1 = JsonStr(hash, "value");
                    break;
                }
            }
        }

        // downloadUrl 为空（作者关闭第三方下载）时会走反推兜底，不是放弃
        if (download_url.empty()) url_derived++;
        file.urls = CurseForgeFileUrls(file_id, file_name, download_url);
        if (file.urls.empty()) {
            info.missing_files.push_back(file_name);
            continue;
        }

        // 可选文件数已在 ParseCurseForge 里按 manifest 统计过了，这里不再累加
        info.total_size += file.size;
        info.files.push_back(std::move(file));
    }

    if (url_derived > 0) {
        Log("有 " + std::to_string(url_derived) + " 个文件接口未提供下载地址，已按 CDN 规则反推");
    }

    // 接口压根没返回的 fileID = 文件已被作者删除，这个没法反推，只能让用户决定
    std::vector<std::pair<long long, long long>> missing;  // (projectID, fileID)
    for (long long id : file_ids) {
        if (returned_ids.count(id)) continue;
        const long long project_id = file_to_project.count(id) ? file_to_project[id] : 0;
        missing.emplace_back(project_id, id);
    }
    if (missing.empty()) return true;

    // 查一下工程名，把「未知项目 (文件 ID 123)」换成「Alex's Caves (文件 ID 123)」。
    // 查不到不算错，退化成只显示 ID
    std::map<long long, std::string> project_names;
    {
        nlohmann::json body;
        body["modIds"] = nlohmann::json::array();
        for (const auto& item : missing) {
            if (item.first > 0) body["modIds"].push_back(item.first);
        }
        if (!body["modIds"].empty()) {
            Models::HttpResult res = Core::HttpClient::HttpPostJson(
                L"https://mod.mcimirror.top/curseforge/v1/mods", body.dump());
            if (res.status == 200 && !res.body.empty()) {
                try {
                    nlohmann::json parsed = nlohmann::json::parse(res.body);
                    if (parsed.contains("data") && parsed["data"].is_array()) {
                        for (const auto& project : parsed["data"]) {
                            const long long id = JsonInt(project, "id");
                            const std::string name = JsonStr(project, "name");
                            if (id > 0 && !name.empty()) project_names[id] = name;
                        }
                    }
                } catch (...) {
                }
            }
        }
    }

    for (const auto& item : missing) {
        const std::string file_id_text = std::to_string(item.second);
        const auto it = project_names.find(item.first);
        if (it != project_names.end()) {
            info.missing_files.push_back(it->second + "（文件 ID " + file_id_text + "）");
        } else if (item.first > 0) {
            info.missing_files.push_back("未知 CurseForge 项目（项目 ID " + std::to_string(item.first) +
                                         "，文件 ID " + file_id_text + "）");
        } else {
            info.missing_files.push_back("未知文件（文件 ID " + file_id_text + "）");
        }
    }
    return true;
}

// ============================================================================
// 解压 overrides 到实例目录
// ============================================================================
inline bool ModpackService::ExtractOverrides(Core::ZipReader& zip, const PackInfo& info,
                                             const std::wstring& instance_dir, std::string& error) {
    if (info.override_prefixes.empty()) return true;

    for (size_t i = 0; i < info.override_prefixes.size(); i++) {
        const std::string& prefix = info.override_prefixes[i];
        const int base = PercentAt("extract", static_cast<int>(i), static_cast<int>(info.override_prefixes.size()));
        EmitProgress("extract", "正在解压整合包文件…", base);

        std::string extract_error;
        const bool ok = zip.ExtractSubtree(
            prefix, instance_dir, true, extract_error,
            [this]() { return Cancelled(); },
            [this, i, &info](const std::string&, int done, int total) {
                const int inner = static_cast<int>(info.override_prefixes.size()) * 100;
                const int progress = static_cast<int>(i) * 100 + (total > 0 ? done * 100 / total : 0);
                EmitProgress("extract", "正在解压整合包文件…", PercentAt("extract", progress, inner));
            });
        if (!ok) {
            error = Cancelled() ? "已取消" : ("解压整合包文件失败：" + extract_error);
            return false;
        }
    }
    return true;
}

// ============================================================================
// 多连接分段下载单个文件（HTTP Range）
//
// 为什么需要它：一个文件只能由一条连接拉的话，走到收尾阶段就会出现
// "其它文件都下完了，只剩一个大文件挂在一条连接上"的局面 —— 进度条看着像卡死。
// 这里把文件按 8MB 切块，交给多个连接并发拉，块内并发、块间串行，
// 所以内存峰值是 connection_count × 8MB，不会随文件变大而膨胀。
// ============================================================================
// ============================================================================
// 并发下载整合包内的文件
// ============================================================================
inline bool ModpackService::DownloadModFiles(const PackInfo& info, const std::wstring& instance_dir,
                                             std::string& error, bool include_optional) {
    std::vector<const ModFile*> queue;
    for (const auto& file : info.files) {
        if (file.optional && !include_optional) continue;
        queue.push_back(&file);
    }
    if (queue.empty()) return true;

    // 按体积从大到小排（LPT，最长处理时间优先）。
    // 同一批工作里先开最大的文件，尾部就只会剩下小文件，
    // 而不是"最后剩一个 200MB 的东西独自慢慢跑"
    std::stable_sort(queue.begin(), queue.end(), [](const ModFile* a, const ModFile* b) {
        return a->size > b->size;
    });

    int thread_count = 8;
    try {
        nlohmann::json config = nlohmann::json::parse(Core::ConfigManager::ReadConfigFile());
        thread_count = static_cast<int>(JsonInt(config, "downloadThreads", 8));
    } catch (...) {
    }
    // 整合包里的文件往往有几十上百 MB，别开太多，容易把带宽和 IO 打满
    thread_count = (std::max)(1, (std::min)(thread_count, 16));
    thread_count = (std::min)(thread_count, static_cast<int>(queue.size()));

    const int total = static_cast<int>(queue.size());
    std::atomic<int> next{0};
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    // 下载任务页要显示速度 / 剩余大小 / 剩余时间预测，这些都需要字节数。
    // size 来自 CurseForge 接口的 fileLength / Modrinth 的 fileSize，缺了就当 0
    long long planned_bytes = 0;
    for (const ModFile* file : queue) {
        if (file->size > 0) planned_bytes += file->size;
    }

    // 动态连接预算：同时下载的大文件越少，每个文件分到的连接越多。
    // 剩 2 个大文件时每个能拿到 thread_count/2 条（上限 8），
    // 剩 1 个就独占 —— 尾巴不会再有"2 条连接慢慢爬、其它线程闲着"
    Core::SegmentedDownloader::SlotBudget budget(Core::SegmentedDownloader::kMaxConnections,
                                                 thread_count);

    // 已落盘的字节数。分段下载每写完一块就往上加一次，
    // 这样一个上百 MB 的文件在下完之前也能看到速度在动
    std::atomic<long long> fetched_bytes{0};
    std::mutex error_mtx;
    std::vector<std::string> failures;

    auto report_progress = [&]() {
        const int completed = done.load() + failed.load();
        const long long done_b = fetched_bytes.load();

        // 百分比按**字节**算，不能按文件数。整合包里往往有一两个文件比前面
        // 几百个加起来还大（这里一个 390 个文件的包，最后一个就占 128MB），
        // 按文件数算的话进度会卡在 64% 好几分钟不动 —— 看着就像卡死。
        // 只有拿不到体积信息（totalFiles 有、totalBytes 为 0）时才退回按文件数。
        int inner = 0;
        if (planned_bytes > 0) {
            inner = static_cast<int>((std::min)(100LL, done_b * 100 / planned_bytes));
        } else if (total > 0) {
            inner = completed * 100 / total;
        }

        EmitProgress("download",
                     "正在下载整合包内容 " + std::to_string(completed) + "/" + std::to_string(total),
                     PercentAt("download", inner, 100),
                     {{"completedFiles", completed},
                      {"totalFiles", total},
                      {"downloadedBytes", done_b},
                      {"totalBytes", planned_bytes}});
    };

    auto worker = [&]() {
        for (;;) {
            const int index = next.fetch_add(1);
            if (index >= total) return;
            if (Cancelled()) return;

            const ModFile& file = *queue[static_cast<size_t>(index)];
            const std::wstring save_path =
                instance_dir + L"\\" + Utf8ToWide(file.relative_path);

            // 已经下过且校验通过就跳过（重装同一个整合包时能省掉大半流量）
            if (!file.sha1.empty() && Core::FileManager::FileExists(save_path) &&
                LowerAscii(Core::HashUtils::ComputeFileSHA1(save_path)) == LowerAscii(file.sha1)) {
                done.fetch_add(1);
                if (file.size > 0) fetched_bytes.fetch_add(file.size);
                report_progress();
                continue;
            }

            // 走不走分段只看两件事：体积够大、下载源扛得住并发。
            // 具体开几条连接交给 SlotBudget 动态决定 —— 它按"当前有几个大文件
            // 在同时下载"来分配，所以同一个文件在尾巴阶段能自动加宽。
            // 以前在这里按"占剩余量的比例"算一次就固定下来，导致 128MB 的
            // 文件被领取时只分到 2 条连接，到尾部仍是 2 条，其它线程干看着。
            const bool try_parallel =
                file.size >= Core::SegmentedDownloader::kMinSizeForSegments &&
                !file.urls.empty() &&
                Core::SegmentedDownloader::HostAllowsSegments(file.urls.front());

            bool ok = false;
            std::string last_error;

            // 每个文件最多两轮：镜像偶尔会抽风，一次失败就整包报废太脆
            ULONGLONG seg_last_report = 0;
            for (int attempt = 0; attempt < 2 && !ok; attempt++) {
                if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(800));

                if (try_parallel) {
                    budget.BeginFile();
                    const bool segmented = Core::SegmentedDownloader::Download(
                        file.urls, save_path, file.size, Core::SegmentedDownloader::kMaxConnections,
                        [this]() { return Cancelled(); },
                        [&](long long added) {
                            fetched_bytes.fetch_add(added);
                            // 分段正常是按 1MB 块回调；服务端不认 Range 时会退化成
                            // 逐 16KB 回调（整个文件一次返回），所以这里要节流
                            const ULONGLONG nowt = GetTickCount64();
                            if (nowt - seg_last_report >= 500) {
                                seg_last_report = nowt;
                                report_progress();
                            }
                        },
                        &budget);
                    budget.EndFile();
                    if (segmented) {
                        // 分段下载拿不到整包 hash 的中间态，落盘后统一校验一次
                        if (file.sha1.empty() ||
                            LowerAscii(Core::HashUtils::ComputeFileSHA1(save_path)) ==
                                LowerAscii(file.sha1)) {
                            ok = true;
                            break;
                        }
                        DeleteFileW(save_path.c_str());
                    }
                }

                for (const auto& url : file.urls) {
                    if (Cancelled()) return;

                    // 单连接下载也逐块上报字节。不接这个回调的话，一个 71MB
                    // 的文件在整段下载期间进度一动不动，速度会显示成 0。
                    // 每 500ms 报一次：既让速度有东西可算，也不会把日志刷爆
                    long long attempt_bytes = 0;
                    ULONGLONG last_report = GetTickCount64();
                    Models::HttpResult res = Core::HttpClient::HttpGet(
                        Utf8ToWide(url), "",
                        [&](long long chunk) {
                            attempt_bytes += chunk;
                            fetched_bytes.fetch_add(chunk);
                            const ULONGLONG nowt = GetTickCount64();
                            if (nowt - last_report >= 500) {
                                last_report = nowt;
                                report_progress();
                            }
                        });

                    if (res.status != 200 || res.body.empty()) {
                        last_error = "下载失败";
                        // 没收全，把这次已经计入的字节退回去
                        if (attempt_bytes > 0) fetched_bytes.fetch_sub(attempt_bytes);
                        continue;
                    }
                    if (!file.sha1.empty() &&
                        LowerAscii(Core::HashUtils::ComputeSHA1(res.body)) != LowerAscii(file.sha1)) {
                        last_error = "SHA-1 校验失败";
                        if (attempt_bytes > 0) fetched_bytes.fetch_sub(attempt_bytes);
                        continue;
                    }
                    if (Core::FileManager::SaveFile(save_path, res.body)) {
                        ok = true;
                        break;
                    }
                    last_error = "写入文件失败";
                    if (attempt_bytes > 0) fetched_bytes.fetch_sub(attempt_bytes);
                }
            }

            if (ok) {
                done.fetch_add(1);
            } else {
                failed.fetch_add(1);
                std::lock_guard<std::mutex> lock(error_mtx);
                failures.push_back(file.name);
                Log("文件下载失败：" + file.name + "（" + last_error + "）");
            }
            report_progress();
        }
    };

    {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(thread_count));
        for (int i = 0; i < thread_count; i++) threads.emplace_back(worker);
        for (auto& t : threads) t.join();
    }

    if (Cancelled()) {
        error = "已取消";
        return false;
    }
    if (!failures.empty()) {
        std::string names;
        for (size_t i = 0; i < failures.size() && i < 5; i++) {
            if (i > 0) names += "、";
            names += failures[i];
        }
        if (failures.size() > 5) names += " 等 " + std::to_string(failures.size()) + " 个文件";
        error = "以下文件下载失败：" + names + "。请检查网络后重试";
        return false;
    }
    return true;
}

// ============================================================================
// 装原版：下到实例目录里，让实例自包含（对齐 PCL）
//
// 原版 JSON 落在 versions\<实例名>\<mc版本>.json、客户端 jar 落在
// versions\<实例名>\<mc版本>.jar。两者都在 FinalizeInstance 里被合并进
// <实例名>.json 并清掉中间产物，最终实例目录里就是「一份 JSON + 一个 jar」。
//
// 之所以不装到共享的 versions\<mc版本>\：整合包实例不能依赖外部那份原版还
// 能不能存在，否则用户删掉/重装原版，整个实例就启动不了。代价是每个实例
// 多占一份客户端 jar（20MB 上下），这是 PCL 一直以来的取舍。
// ============================================================================
inline bool ModpackService::InstallVanilla(const PackInfo& info, const std::string& instance_name,
                                           std::string& error) {
    // 1) 版本 JSON 的地址要先从本地版本清单里查出来
    std::string version_url;
    {
        std::string manifest_text = Core::ConfigManager::ReadLocalManifest();
        if (manifest_text.empty()) {
            // 清单没同步下来就自己拉一次
            for (const auto& url : Core::DownloadSourceManager::GetMojangMetaMirrorUrls(
                     "mc/game/version_manifest_v2.json")) {
                Models::HttpResult res = Core::HttpClient::HttpGet(Utf8ToWide(url));
                if (res.status == 200 && !res.body.empty()) {
                    manifest_text = res.body;
                    break;
                }
            }
        }
        if (!manifest_text.empty()) {
            try {
                nlohmann::json manifest = nlohmann::json::parse(manifest_text);
                if (manifest.contains("versions") && manifest["versions"].is_array()) {
                    for (const auto& version : manifest["versions"]) {
                        if (JsonStr(version, "id") == info.mc_version) {
                            version_url = JsonStr(version, "url");
                            break;
                        }
                    }
                }
            } catch (...) {
            }
        }
    }
    if (version_url.empty()) {
        error = "找不到 Minecraft " + info.mc_version + " 的版本信息，请检查网络连接";
        return false;
    }

    EmitProgress("vanilla", "正在获取 Minecraft " + info.mc_version + " 版本信息…",
                 PercentAt("vanilla", 0, 100));

    // versionId 决定落地目录、actualVersionId 决定文件名：
    // 这一组参数把原版 JSON 写到 versions\<实例名>\<mc版本>.json
    nlohmann::json version_result = download_.RunStep(
        "version", {{"url", version_url}, {"versionId", instance_name}, {"actualVersionId", info.mc_version}});
    if (!version_result.value("ok", false)) {
        error = "获取 Minecraft 版本信息失败：" + version_result.value("error", std::string("未知错误"));
        return false;
    }
    if (Cancelled()) {
        error = "已取消";
        return false;
    }

    // 2) 资源文件（assets）
    if (version_result.contains("assetIndex") && version_result["assetIndex"].is_object() &&
        !version_result["assetIndex"].value("url", "").empty()) {
        EmitProgress("vanilla", "正在下载游戏资源文件…", PercentAt("vanilla", 20, 100));
        nlohmann::json assets_result = download_.RunStep(
            "assets", {{"assetIndexUrl", version_result["assetIndex"].value("url", "")},
                       {"assetIndexId", version_result["assetIndex"].value("id", "")},
                       {"profileName", instance_name}});
        if (!assets_result.value("ok", false)) {
            error = "下载游戏资源文件失败：" + assets_result.value("error", std::string("未知错误"));
            return false;
        }
    }
    if (Cancelled()) {
        error = "已取消";
        return false;
    }

    // 3) 依赖库
    if (version_result.contains("libraries") && version_result["libraries"].is_array() &&
        !version_result["libraries"].empty()) {
        EmitProgress("vanilla", "正在下载游戏依赖库…", PercentAt("vanilla", 50, 100));
        nlohmann::json libs_result =
            download_.RunStep("libraries", {{"libraries", version_result["libraries"]}});
        if (!libs_result.value("ok", false)) {
            error = "下载游戏依赖库失败：" + libs_result.value("error", std::string("未知错误"));
            return false;
        }
    }
    if (Cancelled()) {
        error = "已取消";
        return false;
    }

    // 4) 客户端核心 jar
    if (version_result.contains("clientDownload") && version_result["clientDownload"].is_object() &&
        !version_result["clientDownload"].value("url", "").empty()) {
        EmitProgress("vanilla", "正在下载游戏客户端…", PercentAt("vanilla", 80, 100));
        nlohmann::json client_result = download_.RunStep(
            "client", {{"url", version_result["clientDownload"].value("url", "")},
                       {"sha1", version_result["clientDownload"].value("sha1", "")},
                       {"profileName", instance_name},
                       {"actualVersionId", info.mc_version}});
        if (!client_result.value("ok", false)) {
            error = "下载游戏客户端失败：" + client_result.value("error", std::string("未知错误"));
            return false;
        }
    }

    // 原版不再作为独立版本登记：它只落在实例目录里，config.json 里不该出现一个
    // 指向 versions\<mc版本>\ 的条目（那个目录可能压根没被我们创建过）。
    // 实例自己会在 FinalizeInstance 里被扫描登记

    EmitProgress("vanilla", "游戏本体安装完成", PercentAt("vanilla", 100, 100));
    return true;
}

// ============================================================================
// 装加载器：直接复用 ModLoaderService 的实现
// ============================================================================
inline bool ModpackService::InstallLoader(const PackInfo& info, const std::string& instance_name,
                                          std::string& error) {
    if (info.loader_type.empty() || info.loader_version.empty()) return true;

    EmitProgress("loader", "正在安装 " + info.loader_type + " " + info.loader_version + "…",
                 PercentAt("loader", 10, 100));

    // downloadUrl 留空让 ModLoaderService 自己算 canonical 地址，
    // 它会再拼 BMCLAPI 镜像并做故障转移
    nlohmann::json result = loader_.InstallLoaderSync({{"loaderType", info.loader_type},
                                                       {"loaderVersion", info.loader_version},
                                                       {"gameVersion", info.mc_version},
                                                       {"profileName", instance_name},
                                                       {"downloadUrl", ""}});
    if (!result.value("ok", false)) {
        error = result.value("error", std::string("加载器安装失败"));
        return false;
    }
    EmitProgress("loader", "加载器安装完成", PercentAt("loader", 100, 100));
    return true;
}

// ============================================================================
// 收尾：把原版 JSON 和加载器 JSON 合并成单文件，清掉中间产物，
//       打上强制隔离 / 派生版本标记，刷新版本列表
// ============================================================================
inline bool ModpackService::FinalizeInstance(const PackInfo& info, const std::string& instance_name,
                                             std::string& error) {
    EmitProgress("finalize", "正在整理文件…", PercentAt("finalize", 10, 100));

    const std::wstring instance_dir =
        MinecraftDir() + L"\\versions\\" + Utf8ToWide(instance_name);

    // 整合包不一定带这些目录，缺了游戏自己也会建，但提前建好能让用户在
    // 版本隔离下打开文件夹时看到完整结构
    for (const wchar_t* name : {L"mods", L"resourcepacks", L"shaderpacks", L"saves", L"config"}) {
        Core::FileManager::CreateDirRecursive(instance_dir + L"\\" + name);
    }

    // 父版本：刚才下好的原版 JSON
    const std::wstring vanilla_json_path =
        instance_dir + L"\\" + Utf8ToWide(info.mc_version) + L".json";
    const nlohmann::json parent = ReadJsonFile(vanilla_json_path);
    if (!parent.contains("mainClass") || !parent["mainClass"].is_string()) {
        error = "找不到原版版本信息，实例缺少启动主类，无法启动";
        return false;
    }

    // 子版本：加载器装过才有（纯原版整合包这里读出来是空 object）。
    // 实例名不会等于游戏版本号（InstallWorkerImpl 里已经挡掉了），
    // 所以这份 JSON 和上面那份不可能是同一个文件
    const std::wstring version_json_path =
        instance_dir + L"\\" + Utf8ToWide(instance_name) + L".json";
    const nlohmann::json child = ReadJsonFile(version_json_path);

    nlohmann::json merged = MergeVersionJson(parent, child);

    // id 必须是真实的游戏版本号，不能用实例名：JavaService 按它推算需要哪个
    // Java，GameLaunchService 按它判断 LWJGL 版本、要不要走加载器分支
    merged["id"] = info.mc_version;
    // PCL 用的同名字段，写上以后这个实例在 PCL 里也能被正确识别游戏版本
    merged["clientVersion"] = info.mc_version;
    // 合并后不再有 inheritsFrom，靠这个键告诉 GameLaunchService
    // 「这是原版 + 加载器合成的版本」，加载器专属的启动参数才不会被跳过
    if (!child.empty()) merged["__tauricppDerived"] = true;
    // 强制版本隔离：整合包实例必须独占自己的 mods / saves，
    // 不能指望用户把全局 versionIsolation 打开
    merged["__tauricppForceIsolation"] = true;

    if (!Core::FileManager::SaveFile(version_json_path,
                                     merged.dump(4, ' ', false,
                                                 nlohmann::json::error_handler_t::replace))) {
        error = "写入版本信息失败，目标目录可能被占用";
        return false;
    }

    // 合并完就只剩一份 JSON，原版那份是中间产物，删掉。
    // 留着的话版本目录里会有两个 json，版本列表和启动器可能各挑各的
    DeleteFileW(vanilla_json_path.c_str());

    // 重新扫一遍 versions 目录，把新版本同步进 config.json
    VersionService::ScanInstalledVersions();

    EmitProgress("finalize", "整理完成", PercentAt("finalize", 100, 100));
    return true;
}

// ============================================================================
// 解析整合包 -> 返回确认卡片需要的信息（不落盘、不下载）
// ============================================================================
inline nlohmann::json ModpackService::HandleInspect(const nlohmann::json& args) {
    const std::string path = JsonStr(args, "path");
    Log("inspect 收到请求：" + path);
    if (path.empty()) return {{"ok", false}, {"error", "缺少整合包路径"}};

    const std::wstring archive_path = Utf8ToWide(path);
    if (!Core::FileManager::FileExists(archive_path)) {
        Log("inspect 失败：文件不存在");
        return {{"ok", false}, {"error", "文件不存在：" + path}};
    }

    PackInfo info;
    std::string error;
    if (!ParsePack(archive_path, info, error, false)) {
        Log("inspect 失败：" + error);
        return {{"ok", false}, {"error", error}};
    }
    Log("inspect 成功：" + std::string(info.kind == PackKind::Modrinth ? "modrinth" : "curseforge") +
        " mc=" + info.mc_version + " loader=" + info.loader_type + " " + info.loader_version +
        " files=" + std::to_string(info.declared_files));

    // 实例名建议值：整合包自报的名字；同名已存在时自动加序号。
    // 另外要避开游戏版本号本身 —— 那个名字会和原版 JSON 的中间产物重名
    std::string suggested = SanitizeInstanceName(info.name);
    if (suggested.empty()) suggested = "整合包";
    if (suggested.size() > 60) suggested.resize(60);

    const std::wstring versions_dir = MinecraftDir() + L"\\versions";
    std::string candidate = suggested;
    for (int suffix = 2; suffix < 1000; suffix++) {
        if (candidate != info.mc_version &&
            !Core::FileManager::FileExists(versions_dir + L"\\" + Utf8ToWide(candidate))) {
            break;
        }
        candidate = suggested + " (" + std::to_string(suffix) + ")";
    }

    nlohmann::json result = {
        {"ok", true},
        {"path", path},
        {"type", info.kind == PackKind::Modrinth ? "modrinth" : "curseforge"},
        {"packName", info.name},
        {"gameVersion", info.mc_version},
        {"loaderType", info.loader_type},
        {"loaderVersion", info.loader_version},
        // CurseForge 识别阶段不联网，只报 manifest 里声明的文件数；
        // 真实体积要等安装时查完接口才知道
        {"fileCount", info.files.empty() ? info.declared_files : static_cast<int>(info.files.size())},
        {"optionalCount", info.optional_count},
        {"totalSize", info.total_size},
        {"suggestedName", candidate},
        {"hasOverrides", !info.override_prefixes.empty()},
        {"warnings", info.unsupported},
    };
    return result;
}

// 目录名清洗：去掉 Windows 不允许的字符，并挡掉 ! 和 ;（本项目历史上的路径坑）
inline std::string ModpackService::SanitizeInstanceName(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20) continue;
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || c == '!' || c == ';') {
            continue;
        }
        out.push_back(c);
    }
    // 去掉首尾空格和点（Windows 会静默截断）
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

// ============================================================================
// 安装线程的入口
//
// 这个函数跑在裸 std::thread 上，外面没有任何框架级的 try/catch：
// 一旦有异常漏出去，std::terminate 会直接把进程打死（表现为 ucrtbase 的
// 0xc0000409，用户看到的就是"启动器闪退"）。整合包内容是外部数据，
// 解析出任何意外都不该让启动器崩掉，所以这里必须把所有异常堵死。
// ============================================================================
inline void ModpackService::InstallWorker(const nlohmann::json& args) {
    try {
        InstallWorkerImpl(args);
    } catch (const std::exception& e) {
        Log(std::string("安装线程异常：") + e.what());
        PushResult({{"ok", false}, {"error", std::string("安装过程出现异常：") + e.what()}});
    } catch (...) {
        Log("安装线程出现未知异常");
        PushResult({{"ok", false}, {"error", "安装过程出现未知异常，请查看 .minecraft/logs/modpack.log"}});
    }
}

// ============================================================================
// 安装主流程
// ============================================================================
inline void ModpackService::InstallWorkerImpl(const nlohmann::json& args) {
    const std::string path = JsonStr(args, "path");
    std::string instance_name = JsonStr(args, "instanceName");
    const bool include_optional = JsonBool(args, "downloadOptional", false);

    auto fail = [this](const std::string& message) {
        PushResult({{"ok", false}, {"error", message}});
    };

    if (path.empty()) {
        fail("缺少整合包路径");
        return;
    }
    instance_name = SanitizeInstanceName(instance_name);
    if (instance_name.empty()) {
        fail("实例名不能为空，也不能只包含 Windows 不允许的字符");
        return;
    }

    const std::wstring archive_path = Utf8ToWide(path);
    if (!Core::FileManager::FileExists(archive_path)) {
        fail("整合包文件不存在，可能已被移动或删除");
        return;
    }

    const std::wstring instance_dir = MinecraftDir() + L"\\versions\\" + Utf8ToWide(instance_name);
    if (Core::FileManager::FileExists(instance_dir)) {
        fail("版本 " + instance_name + " 已存在，请换一个实例名");
        return;
    }

    EmitProgress("parse", "正在解析整合包…", 0);

    Core::ZipReader zip;
    PackInfo info;
    std::string error;
    if (!zip.Open(archive_path, error)) {
        fail(error);
        return;
    }
    if (!ParsePack(archive_path, info, error, true)) {
        fail(error);
        return;
    }
    if (Cancelled()) {
        fail("已取消");
        return;
    }

    // 实例名不能和游戏版本号撞车：原版 JSON 的中间产物叫 <mc版本>.json，
    // 加载器 JSON 叫 <实例名>.json，两者同名时加载器会把原版那份覆盖掉，
    // 合并阶段就只剩半个版本，装出来启动不了
    if (instance_name == info.mc_version) {
        fail("实例名不能和游戏版本号相同（" + info.mc_version + "），请换一个名字");
        return;
    }

    // 有文件被作者删掉了：这些没法反推地址，缺失可能导致整合包出问题，
    // 所以像 PCL 那样让用户自己决定继续还是取消（对应 ModModpack.vb:256）
    if (!info.missing_files.empty()) {
        std::string message = "未找到该整合包所需的以下内容，它们可能已被原作者删除：\r\n";
        for (size_t i = 0; i < info.missing_files.size() && i < 12; i++) {
            message += "- " + info.missing_files[i] + "\r\n";
        }
        if (info.missing_files.size() > 12) {
            message += "- 等共 " + std::to_string(info.missing_files.size()) + " 项\r\n";
        }
        message += "\r\n是否继续安装？缺失这些内容可能导致整合包出现异常！";

        Log("缺失 " + std::to_string(info.missing_files.size()) + " 个文件，等待用户确认");
        if (!AskContinue("整合包内容缺失", message)) {
            fail(Cancelled() ? "已取消" : "已取消安装");
            return;
        }
    }

    // 把实例目录建出来，后面所有步骤都往这里落
    Core::FileManager::CreateDirRecursive(instance_dir);

    if (!ExtractOverrides(zip, info, instance_dir, error)) {
        // 解压失败就把半成品目录清掉，别留一个装了一半的版本给用户
        zip.Close();
        Core::FileManager::RemoveDirectoryRecursive(instance_dir);
        fail(error);
        return;
    }
    zip.Close();

    if (Cancelled()) {
        Core::FileManager::RemoveDirectoryRecursive(instance_dir);
        fail("已取消");
        return;
    }

    if (!DownloadModFiles(info, instance_dir, error, include_optional)) {
        Core::FileManager::RemoveDirectoryRecursive(instance_dir);
        fail(error);
        return;
    }

    if (!InstallVanilla(info, instance_name, error)) {
        Core::FileManager::RemoveDirectoryRecursive(instance_dir);
        fail(error);
        return;
    }

    if (!InstallLoader(info, instance_name, error)) {
        // 原版已经装好了，这里失败的话保留目录 —— 用户补上 Java 之后
        // 还能手动把加载器装上，不用从零重来
        fail(error);
        return;
    }

    if (!FinalizeInstance(info, instance_name, error)) {
        fail(error);
        return;
    }

    PushResult({{"ok", true},
                {"instanceName", instance_name},
                {"gameVersion", info.mc_version},
                {"loaderType", info.loader_type},
                {"loaderVersion", info.loader_version}});
}

} // namespace Services
