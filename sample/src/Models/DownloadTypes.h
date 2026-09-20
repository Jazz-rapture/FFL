#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace Models {

// 资源类型枚举
enum class ResourceType {
    ASSETS,     // 资产文件（官方源优先）
    LIBRARY,    // 库文件（镜像源优先）
    FORGE,      // Forge依赖（BMCLAPI专用镜像）
    VERSION_JAR // 版本JAR（镜像源优先）
};

// 下载进度追踪器
struct DownloadProgress {
    std::atomic<int> totalFiles{0};
    std::atomic<int> completedFiles{0};
    std::atomic<int> failedFiles{0};
    std::atomic<long long> totalBytes{0};
    std::atomic<long long> downloadedBytes{0};
    std::mutex mtx;

    void Reset() {
        totalFiles = 0;
        completedFiles = 0;
        failedFiles = 0;
        totalBytes = 0;
        downloadedBytes = 0;
    }

    void AddBytes(long long bytes) {
        downloadedBytes += bytes;
    }

    nlohmann::json ToJson(const std::string& phase) const {
        return {
            {"phase", phase},
            {"totalFiles", (int)totalFiles},
            {"completedFiles", (int)completedFiles},
            {"failedFiles", (int)failedFiles},
            {"totalBytes", (long long)totalBytes},
            {"downloadedBytes", (long long)downloadedBytes}
        };
    }
};

// 失败文件信息
struct FailedFile {
    std::string name;
    std::string url;
    std::string error;
};

// 下载任务
struct DownloadTask {
    std::string name;
    std::wstring url;              // 主下载URL
    std::vector<std::string> mirrorUrls;  // 镜像URL列表 (用于故障转移)
    std::wstring savePath;
    std::string expectedSha1;
    long long size = 0;
    int maxRetries = 3;
    int retryDelayMs = 2000;
    ResourceType resourceType = ResourceType::ASSETS;  // 资源类型，决定下载源优先级
};

// HTTP 请求结果
struct HttpResult {
    std::string body;
    std::string lastModified;
    int status = 0;
};

} // namespace Models
