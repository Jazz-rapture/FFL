#pragma once
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include "Models/MavenCoord.h"

namespace Services {

class MavenUtils {
public:
    // 解析 Maven 坐标 "groupId:artifactId:version[:classifier]"
    static Models::MavenCoord ParseMavenName(const std::string& name) {
        Models::MavenCoord coord;
        std::istringstream ss(name);
        std::string token;
        std::vector<std::string> parts;
        while (std::getline(ss, token, ':')) {
            parts.push_back(token);
        }
        if (parts.size() >= 3) {
            coord.groupId = parts[0];
            coord.artifactId = parts[1];
            coord.version = parts[2];
        }
        if (parts.size() >= 4) {
            coord.classifier = parts[3];
        }
        return coord;
    }

    // 构建 Maven 本地路径
    static std::string BuildMavenPath(const Models::MavenCoord& coord) {
        std::string path;
        for (char c : coord.groupId) {
            path += (c == '.') ? '/' : c;
        }
        path += "/" + coord.artifactId + "/" + coord.version + "/";
        path += coord.artifactId + "-" + coord.version;
        if (!coord.classifier.empty()) {
            path += "-" + coord.classifier;
        }
        path += ".jar";
        return path;
    }

    // 检查 rules 是否匹配当前 OS
    static bool CheckRulesMatch(const nlohmann::json& lib) {
        if (!lib.contains("rules") || !lib["rules"].is_array() || lib["rules"].empty()) {
            return true;
        }

        std::string currentOs;
#if defined(_WIN32)
        currentOs = "windows";
#elif defined(__APPLE__)
        currentOs = "osx";
#else
        currentOs = "linux";
#endif

        bool allowed = false;
        for (const auto& rule : lib["rules"]) {
            std::string action = rule.value("action", "allow");

            if (rule.contains("os") && rule["os"].is_object()) {
                std::string ruleOs = rule["os"].value("name", "");
                if (!ruleOs.empty() && ruleOs != currentOs) {
                    continue;
                }
            }

            if (action == "allow") {
                allowed = true;
            } else if (action == "disallow") {
                allowed = false;
            }
        }
        return allowed;
    }

    // 获取当前 OS 对应的 classifier 后缀
    static std::string GetOsClassifier() {
#if defined(_WIN32)
        return "natives-windows";
#elif defined(__APPLE__)
        return "natives-osx";
#else
        return "natives-linux";
#endif
    }

    // 检查库是否在忽略列表中
    static bool IsLibraryIgnored(const std::string& name, const nlohmann::json& config) {
        if (config.contains("ignoredLibraries") && config["ignoredLibraries"].is_array()) {
            for (const auto& item : config["ignoredLibraries"]) {
                if (item.is_string() && item.get<std::string>() == name) {
                    return true;
                }
            }
        }
        return false;
    }

    // 判断版本号是否低于 1.13（用于区分 LWJGL 2.x / 3.x）
    static bool IsVersionBelow1_13(const std::string& versionId) {
        int major = 0, minor = 0;
        std::istringstream ss(versionId);
        std::string token;
        if (std::getline(ss, token, '.')) {
            try { major = std::stoi(token); } catch (...) { return true; }
        }
        if (std::getline(ss, token, '.')) {
            try { minor = std::stoi(token); } catch (...) { return true; }
        }
        return major < 1 || (major == 1 && minor < 13);
    }
};

} // namespace Services
