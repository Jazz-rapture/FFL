#include "tauricpp/drop_target.hpp"
#include "tauricpp/bridge.hpp"

#include <ole2.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace tauricpp {
namespace {

std::wstring LowerExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return std::wstring();
    std::wstring ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext;
}

// 只认整合包压缩包。rar 也放进来：我们的解压器读不了 rar，但让前端
// 弹一句"请解压后重新打包成 zip"，比让用户看着光标变禁止符号要好懂
bool IsSupportedArchive(const std::wstring& path) {
    const std::wstring ext = LowerExt(path);
    return ext == L"zip" || ext == L"mrpack" || ext == L"rar";
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

// 从 IDataObject 取 CF_HDROP 里的路径列表。
// 数据对象只在 Drop/DragEnter 回调期间有效，所以必须当场读完。
std::vector<std::wstring> ReadHdropPaths(IDataObject* data) {
    std::vector<std::wstring> out;
    if (!data) return out;

    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stgm = {};
    if (FAILED(data->GetData(&fmt, &stgm))) return out;

    HDROP drop = static_cast<HDROP>(GlobalLock(stgm.hGlobal));
    if (drop) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; i++) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            if (DragQueryFileW(drop, i, buf.data(), len + 1) > 0) {
                buf.resize(len);
                out.push_back(std::move(buf));
            }
        }
        GlobalUnlock(stgm.hGlobal);
    }
    ReleaseStgMedium(&stgm);
    return out;
}

std::vector<std::wstring> FilterSupported(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> out;
    for (const auto& p : paths) {
        if (IsSupportedArchive(p)) out.push_back(p);
    }
    return out;
}

std::string PointerText(HWND hwnd) {
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%p", static_cast<void*>(hwnd));
    return buf;
}

// 拖放接不上时用户只会看到"拖进去没反应"，没有任何线索，所以留一份日志。
// 和整合包安装写在同一个文件里，排查时只看一个地方
void LogDrop(const std::string& message) {
    WCHAR exe_path[MAX_PATH];
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    WCHAR* last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) *last_slash = L'\0';

    const std::wstring dir = std::wstring(exe_path) + L"\\.minecraft\\logs";
    std::wstring current;
    size_t start = 0;
    while (start <= dir.size()) {
        const size_t pos = dir.find_first_of(L"\\/", start);
        const size_t end = (pos == std::wstring::npos) ? dir.size() : pos;
        if (end > start) {
            current = dir.substr(0, end);
            CreateDirectoryW(current.c_str(), nullptr);
        }
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }

    const std::string line = "[drop] " + message + "\r\n";
    HANDLE file = CreateFileW((dir + L"\\modpack.log").c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

// 这次拖放当前是否可接受 / 是否已经通知过前端。
// 必须是全局的：拖放目标注册在整棵窗口树上，光标在父子窗口之间移动时
// OLE 会切换到另一个目标实例，状态放在实例里就会错乱
std::atomic<bool> g_drag_acceptable{false};
std::atomic<bool> g_drag_announced{false};

class FileDropTargetImpl : public IDropTarget {
public:
    FileDropTargetImpl(HWND hwnd, Bridge& bridge) : hwnd_(hwnd), bridge_(bridge) {}

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    // ---- IDropTarget ----
    // 拖放目标会注册到整棵窗口树的每个 HWND 上（见 InstallFileDropTree），
    // 而 OLE 是按 WindowFromPoint 逐层切换目标的：光标从父窗口挪到子窗口时，
    // 新目标会收到 DragEnter、旧目标收到 DragLeave。所以"这次拖放算不算数"
    // 不能放在实例里，必须是全局的一份状态，否则 DragOver 会因为本实例
    // 没收到过 DragEnter 而返回 NONE，拖放直接被判定为不可接受
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        const auto supported = FilterSupported(ReadHdropPaths(data));
        const bool acceptable = !supported.empty();
        g_drag_acceptable = acceptable;

        if (effect) *effect = acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        if (!acceptable) return S_OK;

        LogDrop("DragEnter hwnd=" + PointerText(hwnd_) + " files=" + std::to_string(supported.size()));
        // 父子窗口之间切换会重复触发 DragEnter，前端只需要一次
        if (!g_drag_announced.exchange(true)) {
            Emit("app.fileDragEnter", {{"count", supported.size()},
                                       {"first", WideToUtf8(supported.front())}});
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = g_drag_acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        // 离开时一定要把"可接受"清掉：不然拖进来又拖出去（没有松手）时，
        // 后续的 DragEnter/DragOver 会带着上一轮的残留状态判断，
        // 前端那边的遮罩也可能再也不收起
        g_drag_acceptable = false;
        if (g_drag_announced.exchange(false)) {
            LogDrop("DragLeave hwnd=" + PointerText(hwnd_));
            Emit("app.fileDragLeave", nlohmann::json::object());
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        const bool was_announced = g_drag_announced.exchange(false);
        const bool was_acceptable = g_drag_acceptable.exchange(false);

        const auto supported = FilterSupported(ReadHdropPaths(data));
        if (supported.empty()) {
            if (effect) *effect = DROPEFFECT_NONE;
            if (was_announced) Emit("app.fileDragLeave", nlohmann::json::object());
            return S_OK;
        }
        (void)was_acceptable;

        LogDrop("Drop hwnd=" + PointerText(hwnd_) + " files=" + std::to_string(supported.size()));

        nlohmann::json paths = nlohmann::json::array();
        for (const auto& p : supported) paths.push_back(WideToUtf8(p));
        if (effect) *effect = DROPEFFECT_COPY;
        Emit("app.fileDrop", {{"paths", paths}});
        return S_OK;
    }

private:
    void Emit(const char* event, const nlohmann::json& data) {
        // COM 回调边界不能漏异常出去
        try {
            bridge_.Emit(event, data);
        } catch (...) {
        }
    }

    HWND hwnd_;
    Bridge& bridge_;
    std::atomic<ULONG> refs_{1};
};

std::mutex g_drop_mutex;
std::set<HWND> g_drop_registered;

bool InstallFileDrop(HWND hwnd, Bridge& bridge) {
    if (!hwnd) return false;

    std::lock_guard<std::mutex> lock(g_drop_mutex);
    if (g_drop_registered.count(hwnd)) return true;

    auto* target = new FileDropTargetImpl(hwnd, bridge);
    // 起始引用计数 1 是我们自己的，RegisterDragDrop 会再 AddRef 一次；
    // 成功后把自己的那份还回去，之后所有权只归 OLE，RevokeDragDrop 时析构
    HRESULT hr = RegisterDragDrop(hwnd, target);
    target->Release();
    if (FAILED(hr)) {
        // DRAGDROP_E_ALREADYREGISTERED 说明这一层已经被别人（多半是 WebView2
        // 自己）占了，属于预期情况：整棵树注册时总有几层轮不到我们
        char code[16];
        snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(hr));
        LogDrop("RegisterDragDrop 跳过 hwnd=" + PointerText(hwnd) + " HRESULT=0x" + code);
        return false;
    }

    g_drop_registered.insert(hwnd);
    LogDrop("拖放已注册 hwnd=" + PointerText(hwnd));
    return true;
}

void UninstallFileDrop(HWND hwnd) {
    if (!hwnd) return;

    std::lock_guard<std::mutex> lock(g_drop_mutex);
    if (g_drop_registered.erase(hwnd)) {
        RevokeDragDrop(hwnd);
    }
}

struct EnumContext {
    Bridge* bridge = nullptr;
    int installed = 0;
    int skipped = 0;
};

BOOL CALLBACK RegisterChildProc(HWND child, LPARAM param) {
    auto* ctx = reinterpret_cast<EnumContext*>(param);
    if (InstallFileDrop(child, *ctx->bridge)) {
        ctx->installed++;
    } else {
        ctx->skipped++;
    }
    // 继续枚举所有后代（EnumChildWindows 本身是递归的）
    return TRUE;
}

} // namespace

bool InstallFileDropTree(HWND root, Bridge& bridge) {
    if (!root) {
        LogDrop("安装失败：没有窗口句柄");
        return false;
    }
    if (!IsWindow(root)) return false;

    int installed = 0;
    int skipped = 0;

    // 宿主窗口自己也要算一个：拖到窗口边缘（WebView2 覆盖不到的地方）时
    // WindowFromPoint 会落在宿主窗口上
    if (InstallFileDrop(root, bridge)) installed++; else skipped++;

    EnumContext ctx;
    ctx.bridge = &bridge;
    EnumChildWindows(root, RegisterChildProc, reinterpret_cast<LPARAM>(&ctx));
    installed += ctx.installed;
    skipped += ctx.skipped;

    LogDrop("拖放注册完成：成功 " + std::to_string(installed) +
            " 个窗口，跳过 " + std::to_string(skipped) + " 个");
    return installed > 0;
}

void UninstallFileDropTree(HWND root) {
    if (!root) return;

    std::vector<HWND> children;
    EnumChildWindows(root, [](HWND child, LPARAM param) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(param)->push_back(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&children));

    for (HWND child : children) UninstallFileDrop(child);
    UninstallFileDrop(root);
}

} // namespace tauricpp
