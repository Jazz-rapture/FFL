#pragma once
// ZipReader.h - 只读 zip 归档
//
// 整合包安装要的三件事：知道包里有哪些文件（判断这是哪种整合包）、
// 读几个小 JSON（manifest.json / modrinth.index.json）、把 overrides 子树
// 解出来。原来那个 ZipExtractor 只会 spawn PowerShell 抽 jar 里的动态库，
// 既列不了条目也读不了单文件，所以这里基于 miniz 重写一个。
//
// 用 CreateFileW + FILE_FLAG_OVERLAPPED 自己喂读取回调，而不是 miniz 的
// mz_zip_reader_init_file：后者吃 const char*，中文路径必然打不开。

#include <Windows.h>
#include <miniz.h>

#include <functional>
#include <string>
#include <vector>

namespace Core {

class ZipReader {
public:
    struct Entry {
        std::string name;      // 归档内相对路径，'/' 分隔
        std::wstring wname;    // 同上，宽字符版（落盘用）
        bool is_dir = false;
        size_t size = 0;
    };

    // 返回 true 表示"请取消"
    using CancelFn = std::function<bool()>;
    // (当前条目名, 已完成条目数, 总条目数)
    using ProgressFn = std::function<void(const std::string&, int, int)>;

    ZipReader() = default;
    ~ZipReader() { Close(); }
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool Open(const std::wstring& path, std::string& error) {
        Close();

        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = "无法打开文件（可能被其他程序占用，或路径不存在）";
            return false;
        }

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size)) {
            error = "无法读取文件大小";
            Close();
            return false;
        }

        archive_ = {};
        archive_.m_pRead = &ZipReader::ReadCallback;
        archive_.m_pIO_opaque = file_;
        if (!mz_zip_reader_init(&archive_, static_cast<mz_uint64>(size.QuadPart), 0)) {
            error = "不是有效的 zip 压缩包（文件可能损坏，或为 rar/7z 等不支持的格式）";
            Close();
            return false;
        }
        opened_ = true;

        const mz_uint count = mz_zip_reader_get_num_files(&archive_);
        entries_.reserve(count);
        for (mz_uint i = 0; i < count; i++) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive_, i, &stat)) continue;

            Entry entry;
            entry.name = stat.m_filename;
            // Windows 上压缩包里的目录条目常以 '\\' 结尾，统一成 '/'
            for (auto& ch : entry.name) {
                if (ch == '\\') ch = '/';
            }
            entry.is_dir = stat.m_is_directory != 0 || (!entry.name.empty() && entry.name.back() == '/');
            entry.size = static_cast<size_t>(stat.m_uncomp_size);
            entry.wname = Utf8ToWideEx(entry.name);
            entries_.push_back(std::move(entry));
        }
        return true;
    }

    void Close() {
        if (opened_) {
            mz_zip_reader_end(&archive_);
            opened_ = false;
        }
        archive_ = {};
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        entries_.clear();
    }

    bool IsOpen() const { return opened_; }
    const std::vector<Entry>& Entries() const { return entries_; }

    bool HasEntry(const std::string& name) const { return IndexOf(name) >= 0; }

    // 读单个条目到内存。整合包里的 JSON 都很小，不需要流式
    bool ReadAll(const std::string& name, std::string& out, std::string& error) {
        const int index = IndexOf(name);
        if (index < 0) {
            error = "归档中找不到文件：" + name;
            return false;
        }
        const size_t size = entries_[static_cast<size_t>(index)].size;
        out.assign(size, '\0');
        if (size == 0) return true;
        if (!mz_zip_reader_extract_to_mem(&archive_, static_cast<mz_uint>(index),
                                          out.data(), size, 0)) {
            error = "读取归档内容失败：" + name;
            out.clear();
            return false;
        }
        return true;
    }

    // 解压匹配 prefix 的条目到 dest_dir。
    // strip_prefix 为 true 时去掉这段前缀再落盘 —— overrides/ 里那一堆
    // 文件正是不该在版本目录下多出一层 overrides 文件夹。
    // prefix 为空即整个包。
    bool ExtractSubtree(const std::string& prefix,
                        const std::wstring& dest_dir,
                        bool strip_prefix,
                        std::string& error,
                        CancelFn cancelled = nullptr,
                        ProgressFn on_progress = nullptr) const {
        std::vector<size_t> targets;
        targets.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); i++) {
            if (prefix.empty() || StartsWith(entries_[i].name, prefix)) {
                targets.push_back(i);
            }
        }
        if (targets.empty()) {
            error = prefix.empty() ? "压缩包是空的" : ("压缩包里没有 " + prefix + " 目录");
            return false;
        }

        EnsureDirectory(dest_dir);

        const int total = static_cast<int>(targets.size());
        int done = 0;
        for (size_t idx : targets) {
            if (cancelled && cancelled()) {
                error = "已取消";
                return false;
            }

            const Entry& entry = entries_[idx];
            std::string relative = entry.name;
            if (strip_prefix && StartsWith(relative, prefix)) {
                relative = relative.substr(prefix.size());
            }
            // 前缀校验：不管包里的名字长什么样，落盘路径都不能跑出 dest_dir。
            // 只认独立的 ".." 段，"my..mod.jar" 这种合法文件名不能被误杀
            if (!relative.empty() && relative[0] == '/') {
                done++;
                continue;
            }
            if (HasParentSegment(relative)) {
                done++;
                continue;
            }

            if (on_progress && (done % 20 == 0)) on_progress(entry.name, done, total);

            if (entry.is_dir || relative.empty()) {
                if (!relative.empty()) CreateDirTree(dest_dir, Utf8ToWideEx(relative));
                done++;
                continue;
            }

            const std::wstring relativeW = Utf8ToWideEx(relative);
            if (!WriteEntryToFile(static_cast<mz_uint>(idx), dest_dir, relativeW, error)) {
                return false;
            }
            done++;
        }

        if (on_progress) on_progress(std::string(), total, total);
        return true;
    }

private:
    static size_t ReadCallback(void* opaque, mz_uint64 offset, void* buffer, size_t size) {
        HANDLE file = static_cast<HANDLE>(opaque);
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        // 句柄是 FILE_FLAG_OVERLAPPED 开的，ReadFile 会先返回 ERROR_IO_PENDING，
        // 必须等 GetOverlappedResult 把这次传输收掉才能拿到数据
        if (!ReadFile(file, buffer, static_cast<DWORD>(size), &read, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) return 0;
            if (!GetOverlappedResult(file, &ov, &read, TRUE)) return 0;
        }
        return read;
    }

    static bool StartsWith(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    // 路径里有没有独立的 ".." 段（决定能不能落到目标目录之外）
    static bool HasParentSegment(const std::string& relative) {
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

    static std::wstring Utf8ToWideEx(const std::string& s) {
        if (s.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring out(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
        return out;
    }

    static void CreateDirTree(const std::wstring& base, const std::wstring& relative) {
        // relative 里可能有多级目录，逐段建
        std::wstring path = base;
        size_t start = 0;
        while (start <= relative.size()) {
            const size_t slash = relative.find_first_of(L"/\\", start);
            const std::wstring segment =
                relative.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
            if (!segment.empty() && segment != L"..") {
                path += L'\\';
                path += segment;
                CreateDirectoryW(path.c_str(), nullptr);
            }
            if (slash == std::wstring::npos) break;
            start = slash + 1;
        }
    }

    // 递归建目录。解压前目标目录本身可能还不存在
    static void EnsureDirectory(const std::wstring& path) {
        const DWORD attr = GetFileAttributesW(path.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
        const size_t pos = path.find_last_of(L"\\/");
        if (pos != std::wstring::npos && pos > 2) EnsureDirectory(path.substr(0, pos));
        CreateDirectoryW(path.c_str(), nullptr);
    }

    bool WriteEntryToFile(mz_uint index, const std::wstring& dest_dir,
                          const std::wstring& relative, std::string& error) const {
        // 建父目录
        std::wstring file_path = dest_dir;
        const size_t last_slash = relative.find_last_of(L"/\\");
        if (last_slash != std::wstring::npos) {
            CreateDirTree(dest_dir, relative.substr(0, last_slash));
        }
        file_path += L'\\';
        file_path += relative;

        HANDLE out = CreateFileW(file_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            error = "写入文件失败，路径可能是只读或被占用";
            return false;
        }

        struct Sink { HANDLE handle; bool failed; };
        Sink sink{out, false};
        const mz_bool ok = mz_zip_reader_extract_to_callback(
            &archive_, index,
            [](void* opaque, mz_uint64, const void* buf, size_t len) -> size_t {
                auto* s = static_cast<Sink*>(opaque);
                DWORD written = 0;
                if (!WriteFile(s->handle, buf, static_cast<DWORD>(len), &written, nullptr) ||
                    static_cast<size_t>(written) != len) {
                    s->failed = true;
                    return 0;
                }
                return len;
            },
            &sink, 0);

        CloseHandle(out);
        if (!ok || sink.failed) {
            error = "解压失败：一个或多个文件写入出错";
            return false;
        }
        return true;
    }

    int IndexOf(const std::string& name) const {
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].name == name) return static_cast<int>(i);
        }
        // 大小写不敏感再找一次：有的打包工具会把文件名改成全大写
        for (size_t i = 0; i < entries_.size(); i++) {
            if (EqualsIgnoreCase(entries_[i].name, name)) return static_cast<int>(i);
        }
        return -1;
    }

    static bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) return false;
        }
        return true;
    }

    HANDLE file_ = INVALID_HANDLE_VALUE;
    bool opened_ = false;
    mutable mz_zip_archive archive_{};
    std::vector<Entry> entries_;
};

} // namespace Core
