#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <Windows.h>
#include <nlohmann/json.hpp>
#include "Core/HttpClient.h"
#include "Core/HashUtils.h"
#include "Core/FileManager.h"
#include "Core/StringUtils.h"
#include "Bridge/WebviewBridge.h"
#include "Services/GameLaunchService.h"
#include "tauricpp/dialog.hpp"

namespace Services {

// Modrinth 资源浏览 + 下载。分类（模组 / 资源包 / 光影包）共用同一套搜索与版本列表，
// 只在 project_type 和保存目录上分叉。
class ModrinthService {
public:
    ModrinthService(Bridge::WebviewBridge& bridge) : bridge_(bridge), g_app_ptr(nullptr) {
        RegisterCommands();
    }

    void SetApp(tauricpp::App* app) { g_app_ptr = app; }

private:
    tauricpp::App* g_app_ptr;
    Bridge::WebviewBridge& bridge_;

    // Modrinth 的命令都要走网络，而 Bridge 的后台工作线程是**单线程串行**的
    // （见 src/bridge.cpp 的 WorkerLoop）。如果在 handler 里直接发 HTTP，
    // 整个工作线程会被这一次请求占住，排在后面的 version.list / config.* 等
    // 立即返回的命令全部被堵住 —— 表现就是快速切换下载分类时，
    // 新页面一直卡在"加载中"（进程没死，只是没人执行它的命令）。
    // 所以这里统一改成"丢到独立线程 + 前端轮询结果"的异步模式，
    // 与 DownloadService 的 download.poll / modloader.poll 保持一致。
    struct AsyncState {
        std::mutex mtx;
        // 筛选标签按资源类型分开缓存：模组、资源包、光影包的分类完全是三套
        std::map<std::string, nlohmann::json> tags;
        std::set<std::string> tagsLoading;
        // 最近一次搜索的结果连同它的请求号一起留着：前端按请求号取，
        // 不用"取走即清空"，免得旧视图的轮询把新视图要的结果吃掉
        nlohmann::json searchResult;
        long long searchRequestId = 0;
        long long pendingSearchRequestId = 0;
        // 某个项目的文件列表（选完版本再另存为），同样是请求号匹配
        nlohmann::json versionsResult;
        long long versionsRequestId = 0;
        long long pendingVersionsRequestId = 0;
        // 下载任务结果：按任务号取，下载完成前一直留着等前端来拿
        std::map<long long, nlohmann::json> downloads;
        long long nextRequestId = 1;
        long long nextTaskId = 1;
    };

    static AsyncState& State() {
        static AsyncState state;
        return state;
    }

    // 只认这三种资源类型，其它一律按模组处理
    static std::string NormalizeProjectType(const std::string& type) {
        if (type == "resourcepack" || type == "shader") return type;
        return "mod";
    }

    static std::string ProjectTypeLabel(const std::string& type) {
        if (type == "resourcepack") return "资源包";
        if (type == "shader") return "光影包";
        return "模组";
    }

    static nlohmann::json FetchTags(const std::string& projectType) {
        std::wstring tagsBase = L"https://api.modrinth.com/v3";

        // 获取游戏版本列表：三种资源类型都要按游戏版本过滤。
        // v3 这个接口返回的是对象数组（{"value":"1.21.4","type":"release",...}），
        // 直接塞进下拉框会变成一堆 [object Object]，必须取 value。
        // 只保留正式版（快照/alpha/beta 有 800 多个，塞进下拉框没法用）。
        Models::HttpResult verRes = Core::HttpClient::HttpGet(tagsBase + L"/loader_field?loader_field=game_versions");
        nlohmann::json versions = nlohmann::json::array();
        if (verRes.status == 200) {
            try {
                nlohmann::json verData = nlohmann::json::parse(verRes.body);
                if (verData.is_array()) {
                    int count = 0;
                    for (const auto& v : verData) {
                        if (count >= 60) break;
                        if (v.value("type", "") != "release") continue;
                        std::string value = v.value("value", "");
                        if (value.empty()) continue;
                        versions.push_back(value);
                        count++;
                    }
                }
            } catch (...) {}
        }

        // 获取加载器列表：只有模组分加载器，资源包和光影包对加载器没有要求
        nlohmann::json loaders = nlohmann::json::array();
        if (projectType == "mod") {
            Models::HttpResult loaderRes = Core::HttpClient::HttpGet(tagsBase + L"/tag/loader");
            if (loaderRes.status == 200) {
                try {
                    nlohmann::json loaderData = nlohmann::json::parse(loaderRes.body);
                    if (loaderData.is_array()) {
                        std::vector<std::string> allowed = {"fabric", "forge", "quilt", "neoforge"};
                        for (const auto& l : loaderData) {
                            std::string name = l.value("name", l.value("slug", ""));
                            for (const auto& a : allowed) {
                                if (name == a) {
                                    loaders.push_back({{"value", name}, {"label", name}});
                                    break;
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }

        // 获取分类列表：优先按条目自带的 project_type 过滤（模组/资源包/光影包各一套），
        // 拿不到这个字段时才退回模组的白名单
        Models::HttpResult catRes = Core::HttpClient::HttpGet(tagsBase + L"/tag/category");
        nlohmann::json categories = nlohmann::json::array();
        if (catRes.status == 200) {
            try {
                nlohmann::json catData = nlohmann::json::parse(catRes.body);
                if (catData.is_array()) {
                    bool anyTypeField = false;
                    for (const auto& c : catData) {
                        if (c.contains("project_type")) {
                            anyTypeField = true;
                            break;
                        }
                    }

                    std::vector<std::string> modCats = {"adventure", "technology", "optimization", "utility", "magic", "library", "decoration", "equipment", "food", "mobs", "storage", "transportation"};
                    for (const auto& c : catData) {
                        std::string name = c.value("name", c.value("slug", ""));
                        if (name.empty()) continue;
                        std::string lowerName = name;
                        for (auto& ch : lowerName) ch = (char)tolower(ch);

                        if (anyTypeField) {
                            if (c.value("project_type", "") != projectType) continue;
                        } else if (projectType == "mod") {
                            bool allowed = false;
                            for (const auto& mc : modCats) {
                                if (lowerName == mc) {
                                    allowed = true;
                                    break;
                                }
                            }
                            if (!allowed) continue;
                        }
                        categories.push_back({{"value", lowerName}, {"label", name}});
                    }
                }
            } catch (...) {}
        }

        // 一个都没拿到就当失败，让前端提示而不是给一堆空下拉框
        if (versions.empty() && loaders.empty() && categories.empty()) {
            return {{"ok", false}, {"error", "Failed to load filter tags"}};
        }
        return {{"ok", true}, {"versions", versions}, {"loaders", loaders}, {"categories", categories}};
    }

    static nlohmann::json RunSearch(const nlohmann::json& args) {
        std::string query = args.value("query", "");
        std::string index = args.value("index", "relevance");
        int offset = args.value("offset", 0);
        int limit = args.value("limit", 20);
        std::string projectType = NormalizeProjectType(args.value("projectType", "mod"));

        // 构建facets
        nlohmann::json facets = nlohmann::json::array();
        facets.push_back(nlohmann::json::array({"project_type:" + projectType}));

        if (args.contains("versions") && args["versions"].is_string() && !args["versions"].get<std::string>().empty()) {
            facets.push_back(nlohmann::json::array({"versions:" + args["versions"].get<std::string>()}));
        }
        if (args.contains("loaders") && args["loaders"].is_string() && !args["loaders"].get<std::string>().empty()) {
            facets.push_back(nlohmann::json::array({"categories:" + args["loaders"].get<std::string>()}));
        }
        if (args.contains("categories") && args["categories"].is_string() && !args["categories"].get<std::string>().empty()) {
            facets.push_back(nlohmann::json::array({"categories:" + args["categories"].get<std::string>()}));
        }

        // 构建URL参数
        std::wstring url = L"https://api.modrinth.com/v2/search?";
        if (!query.empty()) {
            url += L"query=" + Core::StringUtils::Utf8ToWide(query) + L"&";
        }
        url += L"facets=" + Core::StringUtils::Utf8ToWide(facets.dump()) + L"&";
        url += L"index=" + Core::StringUtils::Utf8ToWide(index) + L"&";
        url += L"offset=" + Core::StringUtils::Utf8ToWide(std::to_string(offset)) + L"&";
        url += L"limit=" + Core::StringUtils::Utf8ToWide(std::to_string(limit));

        Models::HttpResult res = Core::HttpClient::HttpGet(url);
        if (res.status != 200) {
            return {{"ok", false}, {"error", "Search failed"}};
        }

        try {
            nlohmann::json data = nlohmann::json::parse(res.body);
            nlohmann::json hits = nlohmann::json::array();

            if (data.contains("hits") && data["hits"].is_array()) {
                for (const auto& hit : data["hits"]) {
                    nlohmann::json item;
                    item["title"] = hit.value("title", "");
                    item["slug"] = hit.value("slug", "");
                    item["project_id"] = hit.value("project_id", "");
                    item["description"] = hit.value("description", "");
                    item["icon_url"] = hit.value("icon_url", "");
                    item["downloads"] = hit.value("downloads", 0);
                    item["date_modified"] = hit.value("date_modified", "");
                    item["categories"] = hit.value("categories", nlohmann::json::array());
                    item["versions"] = hit.value("versions", nlohmann::json::array());
                    hits.push_back(item);
                }
            }

            return {{"ok", true}, {"hits", hits}, {"total_hits", data.value("total_hits", 0)}};
        } catch (...) {
            return {{"ok", false}, {"error", "Failed to parse search results"}};
        }
    }

    // 某个项目的文件列表。按游戏版本 / 加载器过滤后只保留每个文件的下载信息，
    // 前端拿到的每一项都能直接丢给 resource.download。
    static nlohmann::json RunVersions(const nlohmann::json& args) {
        std::string projectId = args.value("projectId", "");
        std::string slug = args.value("slug", "");
        std::string key = projectId.empty() ? slug : projectId;
        if (key.empty()) return {{"ok", false}, {"error", "Missing project"}};

        // project/{id|slug}/version 两种都收，优先用搜索结果里的 project_id
        std::wstring url = L"https://api.modrinth.com/v2/project/" +
                           Core::StringUtils::Utf8ToWide(key) + L"/version";

        Models::HttpResult res = Core::HttpClient::HttpGet(url);
        if (res.status != 200) {
            return {{"ok", false}, {"error", "Failed to load file list"}};
        }

        std::string filterGameVersion = args.value("gameVersion", "");
        std::string filterLoader = args.value("loader", "");

        try {
            nlohmann::json data = nlohmann::json::parse(res.body);
            if (!data.is_array()) {
                return {{"ok", false}, {"error", "Failed to parse file list"}};
            }

            nlohmann::json files = nlohmann::json::array();
            for (const auto& v : data) {
                nlohmann::json gameVersions = v.value("game_versions", nlohmann::json::array());
                nlohmann::json loaders = v.value("loaders", nlohmann::json::array());

                // 用户在搜索栏选了游戏版本 / 加载器，就只留对得上的文件
                if (!filterGameVersion.empty()) {
                    bool match = false;
                    for (const auto& gv : gameVersions) {
                        if (gv.is_string() && gv.get<std::string>() == filterGameVersion) {
                            match = true;
                            break;
                        }
                    }
                    if (!match) continue;
                }
                if (!filterLoader.empty()) {
                    bool match = false;
                    for (const auto& ld : loaders) {
                        if (ld.is_string() && ld.get<std::string>() == filterLoader) {
                            match = true;
                            break;
                        }
                    }
                    if (!match) continue;
                }

                // 一个版本可能有多个文件，优先取 primary，没有就取第一个
                nlohmann::json file = nlohmann::json::object();
                if (v.contains("files") && v["files"].is_array() && !v["files"].empty()) {
                    const nlohmann::json* picked = &v["files"][0];
                    for (const auto& f : v["files"]) {
                        if (f.value("primary", false)) {
                            picked = &f;
                            break;
                        }
                    }
                    nlohmann::json hashes = picked->value("hashes", nlohmann::json::object());
                    file = {
                        {"url", picked->value("url", "")},
                        {"filename", picked->value("filename", "")},
                        {"size", picked->value("size", (long long)0)},
                        {"sha1", hashes.value("sha1", "")}
                    };
                }

                files.push_back({
                    {"versionId", v.value("id", "")},
                    {"versionNumber", v.value("version_number", "")},
                    {"name", v.value("name", "")},
                    {"versionType", v.value("version_type", "release")},
                    {"gameVersions", gameVersions},
                    {"loaders", loaders},
                    {"datePublished", v.value("date_published", "")},
                    {"downloads", v.value("downloads", 0)},
                    {"file", file}
                });
            }

            return {{"ok", true}, {"files", files}};
        } catch (...) {
            return {{"ok", false}, {"error", "Failed to parse file list"}};
        }
    }

    // 落盘前先写 .part 再改名：中途失败不会在 mods 目录里留下半个 jar，
    // 而游戏目录里出现半个 jar 会直接让启动崩掉
    static nlohmann::json RunDownload(const std::string& url, const std::string& path,
                                     const std::string& sha1) {
        Models::HttpResult res = Core::HttpClient::HttpGet(Core::StringUtils::Utf8ToWide(url));
        if (res.status != 200) {
            return {{"ok", false}, {"error", "下载失败, 状态码: " + std::to_string(res.status)}};
        }

        if (!sha1.empty()) {
            std::string actual = Core::HashUtils::ComputeSHA1(res.body);
            std::string expected = sha1;
            for (auto& c : actual) c = (char)tolower(c);
            for (auto& c : expected) c = (char)tolower(c);
            if (actual != expected) {
                return {{"ok", false}, {"error", "文件校验失败（SHA1 不一致），已放弃写入"}};
            }
        }

        std::wstring wpath = Core::StringUtils::Utf8ToWide(path);
        std::wstring partPath = wpath + L".part";
        if (!Core::FileManager::SaveFile(partPath, res.body)) {
            return {{"ok", false}, {"error", "写入文件失败，请检查目录权限"}};
        }
        // 覆盖已存在的同名文件：先删旧的，否则 MoveFileW 会失败
        DeleteFileW(wpath.c_str());
        if (!MoveFileW(partPath.c_str(), wpath.c_str())) {
            DeleteFileW(partPath.c_str());
            return {{"ok", false}, {"error", "重命名文件失败，请检查文件是否被占用"}};
        }

        return {{"ok", true}, {"path", path}, {"size", (long long)res.body.size()}};
    }

    // 从参数里取字符串数组（前端会把文件的 game_versions / loaders 一起传过来）
    static std::vector<std::string> ReadStringArray(const nlohmann::json& args, const std::string& key) {
        std::vector<std::string> out;
        if (args.contains(key) && args[key].is_array()) {
            for (const auto& item : args[key]) {
                if (item.is_string()) out.push_back(item.get<std::string>());
            }
        }
        return out;
    }

    // 另存为。默认目录取当前版本的游戏目录下对应子目录（与 PCL 的 PathIndie 一致）。
    // 文件如果适配当前实例，先问一句要不要直接放进游戏文件夹（放进去重启游戏就会被加载），
    // 用户拒绝或文件不适配时再走系统的另存为对话框
    nlohmann::json HandleSaveAs(const nlohmann::json& args) {
        std::string projectType = NormalizeProjectType(args.value("projectType", "mod"));
        std::string fileName = args.value("fileName", "");

        std::string defaultDir;
        std::string instanceName;
        std::string instanceVersion;
        std::vector<std::string> instanceLoaders;
        try {
            nlohmann::json paths = GameLaunchService::GetResourcePaths();
            if (paths.contains("paths") && paths["paths"].is_object()) {
                defaultDir = paths["paths"].value(projectType, "");
            }
            instanceName = paths.value("instanceName", "");
            instanceVersion = paths.value("gameVersion", "");
            instanceLoaders = ReadStringArray(paths, "loaders");
        } catch (...) {}

        HWND hwnd = g_app_ptr ? g_app_ptr->GetWindow().GetHwnd() : nullptr;

        if (!defaultDir.empty() && !fileName.empty() &&
            GameLaunchService::IsFileCompatibleWithInstance(
                projectType, ReadStringArray(args, "gameVersions"), ReadStringArray(args, "loaders"),
                instanceVersion, instanceLoaders)) {
            std::string target = defaultDir + "\\" + fileName;
            std::string message =
                "该文件适配当前版本：\n\n  " + instanceName + "（" + instanceVersion + "）\n\n"
                "选「是」直接放入游戏文件夹，重启游戏即可加载：\n  " + target + "\n\n"
                "选「否」改为另存到其它位置。";
            if (tauricpp::Dialog::AskConfirm(hwnd, "直接加入游戏文件夹？", message)) {
                Core::FileManager::CreateDirRecursive(Core::StringUtils::Utf8ToWide(defaultDir));
                return {{"ok", true}, {"canceled", false}, {"path", target}, {"direct", true}};
            }
        }

        // 目录不存在时 IFileDialog 的 SetDefaultFolder 会失败，退回到上次用的位置
        if (!defaultDir.empty()) {
            Core::FileManager::CreateDirRecursive(Core::StringUtils::Utf8ToWide(defaultDir));
        }

        tauricpp::Dialog::SaveOptions options;
        options.title = "保存" + ProjectTypeLabel(projectType);
        options.default_path = defaultDir;
        options.default_filename = fileName;
        options.overwrite_prompt = true;
        if (!fileName.empty()) {
            std::string ext = fileName.substr(fileName.find_last_of('.') + 1);
            if (!ext.empty() && ext != fileName) {
                options.filters = {{ProjectTypeLabel(projectType) + " 文件", "*." + ext}};
            }
        }

        std::optional<std::string> chosen = tauricpp::Dialog::SaveFile(hwnd, options);
        if (!chosen.has_value()) {
            return {{"ok", true}, {"canceled", true}};
        }
        return {{"ok", true}, {"canceled", false}, {"path", *chosen}};
    }

    void RegisterCommands() {
        // modrinth.getTags - 获取筛选选项。标签抓到过一次就一直留着（按资源类型分别缓存）
        bridge_.RegisterCommand("modrinth.getTags", [](const nlohmann::json& args) -> nlohmann::json {
            std::string projectType = NormalizeProjectType(args.value("projectType", "mod"));
            AsyncState& state = State();
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                auto cached = state.tags.find(projectType);
                if (cached != state.tags.end()) {
                    return {{"ok", true}, {"tags", cached->second}};
                }
                // 已经有请求在跑就复用它，别让快速来回切换堆出一串重复抓取
                if (state.tagsLoading.count(projectType)) {
                    return {{"ok", true}, {"async", true}};
                }
                state.tagsLoading.insert(projectType);
            }

            std::thread([projectType]() {
                // 无论成败都要把 loading 标记清掉，否则一次异常会让标签永远抓不上，
                // 每次进这个分类都卡在轮询里
                nlohmann::json tags;
                try {
                    tags = FetchTags(projectType);
                } catch (...) {
                    tags = {{"ok", false}, {"error", "Failed to load filter tags"}};
                }

                AsyncState& state = State();
                std::lock_guard<std::mutex> lock(state.mtx);
                state.tagsLoading.erase(projectType);
                if (tags.value("ok", false)) {
                    state.tags[projectType] = tags;
                }
            }).detach();

            return {{"ok", true}, {"async", true}};
        });

        // modrinth.poll - 取异步结果。标签是幂等的（抓到就一直给同一份，旧的轮询不会把它吃掉）；
        // 搜索与文件列表按各自的请求号匹配，只有发起它的那个视图拿得到
        bridge_.RegisterCommand("modrinth.poll", [](const nlohmann::json& args) -> nlohmann::json {
            std::string kind = args.value("kind", "");
            AsyncState& state = State();
            std::lock_guard<std::mutex> lock(state.mtx);

            if (kind == "tags") {
                std::string projectType = NormalizeProjectType(args.value("projectType", "mod"));
                auto cached = state.tags.find(projectType);
                if (cached == state.tags.end()) {
                    return {{"ok", true}, {"hasResult", false}};
                }
                return {{"ok", true}, {"hasResult", true}, {"data", cached->second}};
            }
            if (kind == "search") {
                long long requestId = args.value("requestId", (long long)0);
                if (requestId == 0 || state.searchRequestId != requestId || state.searchResult.is_null()) {
                    return {{"ok", true}, {"hasResult", false}};
                }
                return {{"ok", true}, {"hasResult", true}, {"data", state.searchResult}};
            }
            if (kind == "versions") {
                long long requestId = args.value("requestId", (long long)0);
                if (requestId == 0 || state.versionsRequestId != requestId || state.versionsResult.is_null()) {
                    return {{"ok", true}, {"hasResult", false}};
                }
                return {{"ok", true}, {"hasResult", true}, {"data", state.versionsResult}};
            }
            if (kind == "download") {
                long long taskId = args.value("taskId", (long long)0);
                auto done = state.downloads.find(taskId);
                if (done == state.downloads.end()) {
                    return {{"ok", true}, {"hasResult", false}};
                }
                nlohmann::json data = done->second;
                state.downloads.erase(done);   // 结果只交付一次
                return {{"ok", true}, {"hasResult", true}, {"data", data}};
            }
            return {{"ok", false}, {"error", "Unknown poll kind"}};
        });

        // modrinth.search - 搜索资源；同样丢线程，立即把请求号交回前端去轮询
        bridge_.RegisterCommand("modrinth.search", [](const nlohmann::json& args) -> nlohmann::json {
            AsyncState& state = State();
            long long requestId = 0;
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                requestId = state.nextRequestId++;
                state.pendingSearchRequestId = requestId;
            }

            nlohmann::json query = args;
            std::thread([query, requestId]() {
                // 异常也要落一个结果下去，不然前端那份轮询会一直空转到超时
                nlohmann::json result;
                try {
                    result = RunSearch(query);
                } catch (...) {
                    result = {{"ok", false}, {"error", "Search failed"}};
                }

                AsyncState& state = State();
                std::lock_guard<std::mutex> lock(state.mtx);
                // 已经被更晚的搜索取代就直接丢掉，避免旧结果盖掉新结果
                if (state.pendingSearchRequestId != requestId) return;
                state.searchRequestId = requestId;
                state.searchResult = result;
            }).detach();

            return {{"ok", true}, {"async", true}, {"requestId", requestId}};
        });

        // modrinth.versions - 某个项目的文件列表（选完版本再另存为）
        bridge_.RegisterCommand("modrinth.versions", [](const nlohmann::json& args) -> nlohmann::json {
            AsyncState& state = State();
            long long requestId = 0;
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                requestId = state.nextRequestId++;
                state.pendingVersionsRequestId = requestId;
            }

            nlohmann::json query = args;
            std::thread([query, requestId]() {
                nlohmann::json result;
                try {
                    result = RunVersions(query);
                } catch (...) {
                    result = {{"ok", false}, {"error", "Failed to load file list"}};
                }

                AsyncState& state = State();
                std::lock_guard<std::mutex> lock(state.mtx);
                if (state.pendingVersionsRequestId != requestId) return;
                state.versionsRequestId = requestId;
                state.versionsResult = result;
            }).detach();

            return {{"ok", true}, {"async", true}, {"requestId", requestId}};
        });

        // resource.saveAs - 弹系统的另存为对话框，返回用户选定的路径（取消则 canceled=true）
        bridge_.RegisterCommand("resource.saveAs", [this](const nlohmann::json& args) -> nlohmann::json {
            return HandleSaveAs(args);
        });

        // resource.download - 下载到指定路径并校验 SHA1；几十 MB 的文件不能占住命令队列
        bridge_.RegisterCommand("resource.download", [](const nlohmann::json& args) -> nlohmann::json {
            std::string url = args.value("url", "");
            std::string path = args.value("path", "");
            std::string sha1 = args.value("sha1", "");
            if (url.empty() || path.empty()) {
                return {{"ok", false}, {"error", "Missing url or path"}};
            }

            AsyncState& state = State();
            long long taskId = 0;
            {
                std::lock_guard<std::mutex> lock(state.mtx);
                taskId = state.nextTaskId++;
            }

            std::thread([taskId, url, path, sha1]() {
                nlohmann::json result;
                try {
                    result = RunDownload(url, path, sha1);
                } catch (...) {
                    result = {{"ok", false}, {"error", "下载失败"}};
                }

                AsyncState& state = State();
                std::lock_guard<std::mutex> lock(state.mtx);
                state.downloads[taskId] = result;
            }).detach();

            return {{"ok", true}, {"async", true}, {"taskId", taskId}};
        });
    }
};

} // namespace Services
