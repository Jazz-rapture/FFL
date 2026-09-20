#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Core/HttpClient.h"
#include "Core/StringUtils.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

class ModrinthService {
public:
    ModrinthService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    void RegisterCommands() {
        // modrinth.getTags - 获取筛选选项（版本、加载器、分类）
        bridge_.RegisterCommand("modrinth.getTags", [](const nlohmann::json&) -> nlohmann::json {
            std::wstring tagsBase = L"https://api.modrinth.com/v3";

            // 获取游戏版本列表
            Models::HttpResult verRes = Core::HttpClient::HttpGet(tagsBase + L"/loader_field?loader_field=game_versions");
            nlohmann::json versions = nlohmann::json::array();
            if (verRes.status == 200) {
                try {
                    nlohmann::json verData = nlohmann::json::parse(verRes.body);
                    if (verData.is_array()) {
                        int count = 0;
                        for (const auto& v : verData) {
                            if (count >= 30) break;
                            versions.push_back(v);
                            count++;
                        }
                    }
                } catch (...) {}
            }

            // 获取加载器列表
            Models::HttpResult loaderRes = Core::HttpClient::HttpGet(tagsBase + L"/tag/loader");
            nlohmann::json loaders = nlohmann::json::array();
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

            // 获取分类列表
            Models::HttpResult catRes = Core::HttpClient::HttpGet(tagsBase + L"/tag/category");
            nlohmann::json categories = nlohmann::json::array();
            if (catRes.status == 200) {
                try {
                    nlohmann::json catData = nlohmann::json::parse(catRes.body);
                    if (catData.is_array()) {
                        std::vector<std::string> modCats = {"adventure", "technology", "optimization", "utility", "magic", "library", "decoration", "equipment", "food", "mobs", "storage", "transportation"};
                        for (const auto& c : catData) {
                            std::string name = c.value("name", c.value("slug", ""));
                            std::string lowerName = name;
                            for (auto& ch : lowerName) ch = (char)tolower(ch);
                            for (const auto& mc : modCats) {
                                if (lowerName == mc) {
                                    categories.push_back({{"value", lowerName}, {"label", name}});
                                    break;
                                }
                            }
                        }
                    }
                } catch (...) {}
            }

            return {{"ok", true}, {"versions", versions}, {"loaders", loaders}, {"categories", categories}};
        });

        // modrinth.search - 搜索模组
        bridge_.RegisterCommand("modrinth.search", [](const nlohmann::json& args) -> nlohmann::json {
            std::string query = args.value("query", "");
            std::string index = args.value("index", "relevance");
            int offset = args.value("offset", 0);
            int limit = args.value("limit", 20);

            // 构建facets
            nlohmann::json facets = nlohmann::json::array();
            facets.push_back(nlohmann::json::array({"project_type:mod"}));

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
        });
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
