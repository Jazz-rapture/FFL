#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"

namespace Core {

// 下载模式 —— 对应前端「下载设置」中的三个策略。
// 文件下载与版本列表可分别配置，见 SourceScope。
enum class DownloadMode {
    MirrorFirst,   // 镜像优先，官方兜底
    OfficialOnly,  // 全部官方
    MirrorOnly     // 全部镜像
};

// 设置作用域：
//   File        -> downloadSettings.source          （文件下载源）
//   VersionList -> downloadSettings.versionListSource（版本列表源）
enum class SourceScope {
    File,
    VersionList
};

// 下载源管理器：把「官方地址 + 镜像地址」按当前模式组合成候选列表。
// 调用方约定：返回列表的 [0] 为主下载地址，其余为故障转移备选。
class DownloadSourceManager {
public:
    // 已实测可用的 BMCLAPI 主节点与高校镜像（代码历史中的 hkmc.online 主机已失效）
    static const char* Bmclapi() { return "https://bmclapi2.bangbang93.com"; }
    static const char* QluBmclapi() { return "https://mirrors4.qlu.edu.cn/bmclapi"; }

    // 读取指定作用域的下载模式
    static DownloadMode GetMode(SourceScope scope) {
        nlohmann::json config = ReadConfig();
        std::string key = (scope == SourceScope::File) ? "source" : "versionListSource";
        std::string value = "mirror_first";
        if (config.contains("downloadSettings") && config["downloadSettings"].is_object()) {
            value = config["downloadSettings"].value(key, "mirror_first");
        }
        if (value == "mirror_only") return DownloadMode::MirrorOnly;
        if (value == "mirror_first" || value == "bmclapi") return DownloadMode::MirrorFirst;  // bmclapi 为旧配置值
        return DownloadMode::OfficialOnly;
    }

    // 按模式组合候选地址（返回顺序即下载优先级，[0] 为主源）
    //   全部官方：仅官方
    //   全部镜像：仅镜像；若该资源没有可用镜像，退回官方（本就没有镜像可选）
    //   镜像优先：镜像在前，官方兜底
    static std::vector<std::string> Arrange(const std::vector<std::string>& official,
                                            const std::vector<std::string>& mirror,
                                            DownloadMode mode) {
        std::vector<std::string> out;
        auto append = [&out](const std::vector<std::string>& list) {
            for (const auto& url : list) {
                bool dup = false;
                for (const auto& seen : out) {
                    if (seen == url) { dup = true; break; }
                }
                if (!dup) out.push_back(url);
            }
        };
        switch (mode) {
            case DownloadMode::OfficialOnly: append(official); break;
            case DownloadMode::MirrorOnly:   append(mirror.empty() ? official : mirror); break;
            case DownloadMode::MirrorFirst:  append(mirror); append(official); break;
        }
        return out;
    }

    // 文件下载源（downloadSettings.source）
    static std::vector<std::string> ArrangeFile(const std::vector<std::string>& official,
                                                const std::vector<std::string>& mirror) {
        return Arrange(official, mirror, GetMode(SourceScope::File));
    }

    // 版本列表源（downloadSettings.versionListSource）
    static std::vector<std::string> ArrangeVersionList(const std::vector<std::string>& official,
                                                       const std::vector<std::string>& mirror) {
        return Arrange(official, mirror, GetMode(SourceScope::VersionList));
    }

    // ---- 兼容旧接口：读写配置中的原始字符串（日志 / config.getDownloadSource / setDownloadSource）----
    static std::string GetCurrentSource() {
        nlohmann::json config = ReadConfig();
        if (config.contains("downloadSettings") && config["downloadSettings"].is_object()) {
            return config["downloadSettings"].value("source", "mirror_first");
        }
        return "mirror_first";
    }

    static void SetCurrentSource(const std::string& source) {
        nlohmann::json config = ReadConfig();
        if (!config.contains("downloadSettings") || !config["downloadSettings"].is_object()) {
            config["downloadSettings"] = nlohmann::json::object();
        }
        config["downloadSettings"]["source"] = source;
        ConfigManager::WriteConfigFile(config.dump(4));
    }

    // ==================== 各类资源的下载地址 ====================

    // 库文件（maven）
    static std::vector<std::string> GetLibraryMirrorUrls(const std::string& path) {
        std::vector<std::string> official = {"https://libraries.minecraft.net/" + path};
        std::vector<std::string> mirror = {
            std::string(Bmclapi()) + "/maven/" + path,
            std::string(QluBmclapi()) + "/" + path
        };
        std::vector<std::string> result = ArrangeFile(official, mirror);
        LogUrls("LIBRARY", path, result);
        return result;
    }

    // 资源文件及资产文件（同一对象存储，hash 前两位为子目录）
    static std::vector<std::string> GetResourceMirrorUrls(const std::string& prefix, const std::string& hash) {
        std::vector<std::string> official = {"https://resources.download.minecraft.net/" + prefix + "/" + hash};
        std::vector<std::string> mirror = {std::string(Bmclapi()) + "/assets/" + prefix + "/" + hash};
        return ArrangeFile(official, mirror);
    }

    // 资产文件（与资源文件同一对象存储）
    static std::vector<std::string> GetAssetsDownloadUrls(const std::string& prefix, const std::string& hash) {
        return GetResourceMirrorUrls(prefix, hash);
    }

    // Forge 依赖（maven）
    static std::vector<std::string> GetForgeDownloadUrls(const std::string& mavenPath) {
        std::vector<std::string> official = {"https://maven.minecraftforge.net/" + mavenPath};
        std::vector<std::string> mirror = {
            std::string(Bmclapi()) + "/maven/" + mavenPath,
            std::string(QluBmclapi()) + "/" + mavenPath
        };
        std::vector<std::string> result = ArrangeFile(official, mirror);
        LogUrls("FORGE", mavenPath, result);
        return result;
    }

    // Mojang 元数据 / 版本清单（元数据路径不带 /maven/ 前缀）。
    // 由 ManifestService 用于同步版本清单，因此遵循「版本列表源」。
    static std::vector<std::string> GetMojangMetaMirrorUrls(const std::string& path) {
        std::vector<std::string> official = {"https://launchermeta.mojang.com/" + path};
        std::vector<std::string> mirror = {std::string(Bmclapi()) + "/" + path};
        return ArrangeVersionList(official, mirror);
    }

    // 所有可用的下载源选项（供前端选择器渲染）
    static nlohmann::json GetAvailableSources() {
        return {
            {"sources", {
                {{"id", "mirror_first"}, {"name", "镜像优先，官方兜底"}, {"description", "优先 BMCLAPI 镜像，失败回退官方源（推荐国内用户）"}},
                {{"id", "official"},     {"name", "全部官方"},           {"description", "全部使用 Mojang / 加载器官方源"}},
                {{"id", "mirror_only"},  {"name", "全部镜像"},           {"description", "全部使用 BMCLAPI 镜像（Quilt 等无镜像的资源仍走官方）"}}
            }}
        };
    }

private:
    static nlohmann::json ReadConfig() {
        std::string content = ConfigManager::ReadConfigFile();
        try {
            return nlohmann::json::parse(content);
        } catch (...) {
            return nlohmann::json::object();
        }
    }

    static const char* ModeName(DownloadMode mode) {
        switch (mode) {
            case DownloadMode::MirrorFirst:  return "mirror_first";
            case DownloadMode::MirrorOnly:   return "mirror_only";
            default:                         return "official";
        }
    }

    static void LogUrls(const char* tag, const std::string& path, const std::vector<std::string>& urls) {
        std::string line = std::string("[") + tag + "] " + path + " (模式: " + ModeName(GetMode(SourceScope::File)) + ")\n";
        for (size_t i = 0; i < urls.size(); i++) {
            line += std::string("[") + tag + "]   URL[" + std::to_string(i) + "]: " + urls[i] + "\n";
        }
        OutputDebugStringA(line.c_str());
    }
};

} // namespace Core
