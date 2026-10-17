#pragma once
// SegmentedDownloader.h - 多连接分段下载（HTTP Range）
//
// 为什么需要它：不少镜像（BMCLAPI、mcimirror）是**按连接限速**的，实测单连接
// 只有 20~150 KB/s。一个上百 MB 的文件如果只能由一条连接拉，就要十几分钟，
// 整个任务都吊在它上面 —— 表现就是"进度卡死在最后一个文件"。
//
// 做法：把文件按 1MB 切块，多个连接并发拉，块间用原子游标抢任务。
// 关键设计（都是踩过坑才加的）：
//   * 块要小：8MB 块在 130KB/s 的链路上要 60 秒才完成一次请求，失败代价太大；
//     1MB 块约 8 秒，重试便宜得多。
//   * 块要重试：以前任何一块失败就放弃整个文件，然后调用方回退成单连接、
//     把已经下完的字节全丢掉重来。现在是每块最多试 4 次，还能在多个下载源之间轮换。
//   * 只有整块写成功才计进度，所以进度不会虚高。

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Core/FileManager.h"
#include "Core/HttpClient.h"
#include "Core/StringUtils.h"
#include "Models/DownloadTypes.h"

namespace Core {

class SegmentedDownloader {
public:
    // 返回 true 表示"请取消"
    using CancelFn = std::function<bool()>;
    // 每写完一块回调一次（参数为本次新增的字节数）
    using ProgressFn = std::function<void(long long)>;

    // 小于这个体积不值得分段（请求数上去了、收益不明显）
    static constexpr long long kMinSizeForSegments = 4LL * 1024 * 1024;
    static constexpr long long kBlockSize = 1LL * 1024 * 1024;
    static constexpr int kMaxConnections = 8;
    static constexpr int kMaxAttemptsPerBlock = 4;

    // ------------------------------------------------------------------
    // 动态连接预算
    //
    // 一个文件的连接数**不能只在开始下载时算一次**。整合包下载按体积降序
    // （大文件先开工），那个 128MB 的文件被领取时剩余总量还有 2GB，
    // 按占比只分到 2 条连接；等它变成"最后两个文件之一"时，另外 14 条
    // 线程全闲着，它还是在用 2 条连接慢慢爬 —— 这就是尾巴卡半天的原因。
    //
    // 这里改成：连接数 = 总线程数 / 当前正在下的大文件数，在**每个大文件
    // 开始/结束时重算**。剩 2 个大文件就各分 8 条，剩 1 个就独占。
    // ------------------------------------------------------------------
    class SlotBudget {
    public:
        SlotBudget(int max_slots, int total_threads)
            : max_(max_slots < 1 ? 1 : max_slots),
              threads_(total_threads < 1 ? 1 : total_threads) {
            Recompute();
        }

        std::atomic<int>* Slots() { return &slots_; }

        void BeginFile() { inflight_.fetch_add(1); Recompute(); }
        void EndFile() { inflight_.fetch_sub(1); Recompute(); }

    private:
        void Recompute() {
            const int inflight = (std::max)(1, inflight_.load());
            int slots = threads_ / inflight;
            if (slots < 1) slots = 1;
            if (slots > max_) slots = max_;
            slots_.store(slots);
        }

        std::atomic<int> slots_{1};
        std::atomic<int> inflight_{0};
        int max_;
        int threads_;
    };

    // 哪些下载源**不能**分段。
    //
    // 这不是保守，是实测出来的：同一个 BMCLAPI 资源
    //   单连接 4MB：3.3 秒（1235 KB/s）
    //   4 连接各 1MB：15.5 秒（265 KB/s）—— 反而慢 4.7 倍
    // 它对同一 IP 的并发连接很敏感，多开会互相挤。PCL 也是直接把
    // bmclapi / github / optifine 这些排除在多线程分段之外（ModNet.vb:1035）。
    //
    // 反例：模组 CDN（forgecdn / mcimirror）是**按连接限速**的，
    // 单连接 223 KB/s、4 连接 605 KB/s，那里分段才有意义。
    static bool HostAllowsSegments(const std::string& url) {
        static const char* banned[] = {
            "bmclapi", "bangbang93", "github.com", "githubusercontent",
            "optifine.net", "meloong.com", "momot.rs", "pcl2-server",
        };
        for (const char* key : banned) {
            if (url.find(key) != std::string::npos) return false;
        }
        return true;
    }

    // 下载一个文件。urls 是候选下载源（按优先级），分段会在它们之间轮换重试。
    //
    // 返回 true  = 文件已完整落盘（**未做 hash 校验**，调用方自己校验）
    // 返回 false = 没能下完；调用方可以退回单连接下载
    //
    // 失败时会把写了一半的文件删掉，避免后续被当成"已存在"而跳过。
    //
    // budget 非空时用它给出的连接数（会随"同时在下载的大文件数"变化），
    // 此时固定开 kMaxConnections 个 worker，超出的按预算挂起不发请求。
    static bool Download(const std::vector<std::string>& urls, const std::wstring& save_path,
                         long long total_size, int connections,
                         CancelFn cancelled, ProgressFn on_progress,
                         SlotBudget* budget = nullptr) {
        if (urls.empty() || total_size <= 0) return false;
        if (!budget && connections <= 1) return false;
        if (total_size < kMinSizeForSegments) return false;
        if (!HostAllowsSegments(urls.front())) return false;

        // ---- 1. 探针：确认第一个源支持 Range ----
        // 只取 1 个字节，代价可以忽略
        const std::wstring probe_url = StringUtils::Utf8ToWide(urls.front());
        Models::HttpResult probe = HttpClient::HttpGetRange(probe_url, 0, 0, on_progress);
        if (probe.status == 200 && static_cast<long long>(probe.body.size()) == total_size) {
            // 服务端忽略了 Range、直接把整个文件给了我们。这时探针其实就是
            // 一次完整的单连接下载，已经通过 on_progress 汇报过进度了
            if (FileManager::SaveFile(save_path, probe.body)) {
                return true;
            }
            return false;
        }
        if (probe.status != 206) return false;

        // ---- 2. 建文件并预分配到最终大小，各块才能直接按偏移写 ----
        HANDLE out = CreateFileW(save_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER final_size;
        final_size.QuadPart = total_size;
        if (!SetFilePointerEx(out, final_size, nullptr, FILE_BEGIN) || !SetEndOfFile(out)) {
            CloseHandle(out);
            DeleteFileW(save_path.c_str());
            return false;
        }

        const long long block_count = (total_size + kBlockSize - 1) / kBlockSize;
        std::atomic<long long> next_block{0};
        std::atomic<bool> abort{false};
        std::atomic<long long> written_bytes{0};
        std::mutex write_mtx;

        auto block_worker = [&](int worker_index) {
            for (;;) {
                if (abort.load()) return;
                if (cancelled && cancelled()) return;
                if (next_block.load() >= block_count) return;

                // 动态预算：超出当前份额的 worker 暂时不发请求，等大文件
                // 下完、份额变宽之后再参与。这样同一个文件在"尾巴"阶段
                // 能自动占满空闲线程
                if (budget) {
                    const int slots = budget->Slots()->load();
                    if (worker_index >= slots) {
                        // 挂起 300ms 再看。必须开满 worker 而不是按当前份额开，
                        // 否则一个文件若在"大文件很多"时启动，就永远没机会加宽
                        std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        continue;
                    }
                }

                const long long block = next_block.fetch_add(1);
                if (block >= block_count) return;

                const long long from = block * kBlockSize;
                const long long to = (std::min)(from + kBlockSize - 1, total_size - 1);
                const long long expected = to - from + 1;

                bool block_ok = false;
                for (int attempt = 0; attempt < kMaxAttemptsPerBlock && !block_ok; attempt++) {
                    if (abort.load()) return;
                    if (cancelled && cancelled()) return;
                    if (attempt > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
                    }

                    // 换源重试：不同镜像对同一 Range 的表现可能不一样
                    const std::string& url = urls[static_cast<size_t>(attempt) % urls.size()];
                    Models::HttpResult res =
                        HttpClient::HttpGetRange(StringUtils::Utf8ToWide(url), from, to);
                    if (res.status != 206 ||
                        static_cast<long long>(res.body.size()) != expected) {
                        continue;
                    }

                    std::lock_guard<std::mutex> lock(write_mtx);
                    LARGE_INTEGER pos;
                    pos.QuadPart = from;
                    DWORD written = 0;
                    if (!SetFilePointerEx(out, pos, nullptr, FILE_BEGIN) ||
                        !WriteFile(out, res.body.data(),
                                   static_cast<DWORD>(res.body.size()), &written, nullptr) ||
                        static_cast<long long>(written) != expected) {
                        // 写盘失败不是网络问题，重试没意义
                        abort = true;
                        return;
                    }
                    block_ok = true;
                }

                if (!block_ok) {
                    abort = true;
                    return;
                }

                written_bytes.fetch_add(expected);
                if (on_progress) on_progress(expected);
            }
        };

        {
            // 有预算时开满 worker（由预算决定谁真正发请求），否则按调用方给的连接数。
            // 上限再按块数收一下：只有 4 块的 4MB 文件没必要开 8 个线程
            int workers = budget ? kMaxConnections : connections;
            const int blocks = static_cast<int>((std::min)(block_count, (long long)kMaxConnections));
            if (workers > blocks) workers = blocks;
            if (workers < 1) workers = 1;

            std::vector<std::thread> threads;
            threads.reserve(static_cast<size_t>(workers));
            for (int i = 0; i < workers; i++) threads.emplace_back(block_worker, i);
            for (auto& t : threads) t.join();
        }

        CloseHandle(out);

        if (abort.load() || written_bytes.load() != total_size) {
            // 半成品必须删掉：留着会被"已存在且 hash 匹配"之外的逻辑误判，
            // 而且调用方回退成单连接时会从头覆盖写
            DeleteFileW(save_path.c_str());
            return false;
        }
        return true;
    }
};

} // namespace Core
