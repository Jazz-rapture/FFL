#include "tauricpp/window.hpp"
#include "tauricpp/bridge.hpp"
#include "tauricpp/drop_target.hpp"
#include "tauricpp/virtual_fs.hpp"

#include <WebView2.h>
#include <wrl.h>
#include <ole2.h>          // OleInitialize：OLE 拖放的前置条件
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <queue>
#include <mutex>
#include <cstdio>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")   // 用于 GetDpiForMonitor / SetProcessDpiAwarenessContext
#pragma comment(lib, "dwmapi.lib")   // 用于 DwmSetWindowAttribute (圆角)

using namespace Microsoft::WRL;

// 自定义窗口消息 - 用于投递JS到UI线程执行
static const UINT WM_TAURICP_EXECUTE_JS = WM_APP + 1;
// 自定义窗口消息 - 用于投递WebView消息到UI线程发送
static const UINT WM_TAURICP_POST_MESSAGE = WM_APP + 2;
// 「就绪后再显示」的兜底计时器：WebView2 迟迟不报导航完成时，也不能让窗口永远看不见
static const UINT_PTR kRevealFallbackTimer = 1;
static const UINT kRevealFallbackMs = 5000;


// ============================================================================
// DPI 辅助：Per-Monitor V2 感知（运行时兜底，即便 manifest 缺失也能正常渲染）
// ============================================================================
static void EnsurePerMonitorDpiAwareV2() {
    // 优先使用 Win10+ 的 PerMonitorV2：系统自动缩放非客户区 + 子窗口 DPI 通知
    typedef BOOL(WINAPI* PSetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT);
    auto setContext = reinterpret_cast<PSetProcessDpiAwarenessContext>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setContext) {
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        if (setContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4))) {
            return;
        }
    }
    // Win8.1 兜底：PROCESS_PER_MONITOR_DPI_AWARE = 2
    typedef HRESULT(WINAPI* PSetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);
    auto setAwareness = reinterpret_cast<PSetProcessDpiAwareness>(
        ::GetProcAddress(::GetModuleHandleW(L"shcore.dll"), "SetProcessDpiAwareness"));
    if (setAwareness) {
        setAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
    }
}

namespace tauricpp {

// ============================================================================
// EventTokens::Impl - 持有WebView2事件注册令牌
// ============================================================================
struct Window::EventTokens::Impl {
    EventRegistrationToken webResourceRequestedToken = {};
    EventRegistrationToken webMessageReceivedToken = {};
    EventRegistrationToken navigationCompletedToken = {};
    EventRegistrationToken sourceChangedToken = {};
    EventRegistrationToken focusChangedToken = {};
};

Window::EventTokens::EventTokens() : impl(std::make_unique<Impl>()) {}
Window::EventTokens::~EventTokens() = default;

// ============================================================================
// 构造/析构
// ============================================================================
Window::Window(const Config& config) : config_(config) {}

Window::~Window() {
    shutting_down_ = true;

    // 清理JS队列
    {
        std::lock_guard<std::mutex> lock(jsQueueMutex_);
        while (!jsQueue_.empty()) jsQueue_.pop();
    }

    // 清理WebView消息队列，并注销回调避免后台线程再投递
    {
        std::lock_guard<std::mutex> lock(webMsgQueueMutex_);
        while (!webMsgQueue_.empty()) webMsgQueue_.pop();
    }
    Bridge::Instance().SetPostMessageCallback(nullptr);
    Bridge::Instance().SetExecuteJsCallback(nullptr);

    // 注销WebView2事件处理器
    if (webview_) {
        ComPtr<ICoreWebView2_2> webview2;
        if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
            webview2->remove_WebResourceRequested(event_tokens_.impl->webResourceRequestedToken);
            webview2->remove_WebMessageReceived(event_tokens_.impl->webMessageReceivedToken);
        }
        webview_->remove_NavigationCompleted(event_tokens_.impl->navigationCompletedToken);
    }

    if (controller_) {
        controller_->Release();
        controller_ = nullptr;
    }
    if (webview_) {
        webview_->Release();
        webview_ = nullptr;
    }
    if (env_) {
        env_->Release();
        env_ = nullptr;
    }
    if (bg_brush_) {
        DeleteObject(bg_brush_);
        bg_brush_ = nullptr;
    }
}

// ============================================================================
// 辅助函数 - 正确的UTF-8 <-> Wide转换
// ============================================================================
static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, result.data(), len);
    result.pop_back();
    return result;
}

static std::string WideToUtf8(const std::wstring& str) {
    if (str.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, result.data(), len, nullptr, nullptr);
    result.pop_back();
    return result;
}

// ============================================================================
// WebView2 初始化失败时的报错
// ============================================================================
// 以前失败是静默的：窗口照样显示、界面全空，看上去就是"卡死了、什么都没起来"，
// 完全查不出原因（2026-10-07 实际踩过）。这里把 HRESULT 直接摆到用户面前
static void ReportWebView2Failure(HRESULT hr, const wchar_t* stage) {
    wchar_t text[512];
    swprintf_s(text, L"%s失败。\n\nHRESULT：0x%08X\n\nWebView2 运行时可能缺失、损坏或被占用。",
               stage, static_cast<unsigned int>(hr));
    MessageBoxW(nullptr, text, L"TauriCPP", MB_OK | MB_ICONERROR);
}

// ============================================================================
// 创建Win32原生窗口
// ============================================================================
bool Window::CreateNativeWindow() {
    EnsurePerMonitorDpiAwareV2();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 用配置的背景色当窗口类背景，并留一份句柄给 WM_ERASEBKGND 自己填
    bg_brush_ = CreateSolidBrush(config_.bg_color);
    wc.hbrBackground = bg_brush_;
    wc.lpszClassName = L"TauriCPPWindowClass";

    static bool registered = false;
    if (!registered) {
        RegisterClassExW(&wc);
        registered = true;
    }

    DWORD style;
    if (config_.borderless) {
        // 无边框窗口：WS_POPUP 无标题栏，WS_THICKFRAME 允许从边缘拖拽调整大小
        style = WS_POPUP | WS_THICKFRAME;
        if (!config_.resizable) {
            style &= ~WS_THICKFRAME;
        }
    } else {
        style = WS_OVERLAPPEDWINDOW;
        if (!config_.resizable) {
            style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        }
    }

    // 根据窗口目标显示位置的 DPI 将"设计尺寸"换算为实际物理像素尺寸
    // 设计尺寸约定：以 96 DPI (100%) 为基准（即 CSS 逻辑像素）
    UINT targetDpi = 96;
    {
        // 创建一个隐藏临时窗口以获得准确的 WMDPICHANGED 前的屏幕DPI；
        // 更简单：直接用主显示器的 DPI，窗口创建后系统会再次发送 WM_DPICHANGED。
        typedef UINT(WINAPI* PGetDpiForSystem)();
        auto getDpiForSystem = reinterpret_cast<PGetDpiForSystem>(
            ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForSystem"));
        if (getDpiForSystem) {
            targetDpi = getDpiForSystem();
        }
    }
    const int baseDpi = 96;
    int scaledWidth = MulDiv(config_.width, targetDpi, baseDpi);
    int scaledHeight = MulDiv(config_.height, targetDpi, baseDpi);

    RECT rect = { 0, 0, scaledWidth, scaledHeight };
    // 注意：AdjustWindowRect 非 DPI 感知，使用 DPI 感知版本（Win10 1607+），
    // 若不可用则退回 AdjustWindowRect，DPI 差异由 WM_DPICHANGED 补偿。
    typedef BOOL(WINAPI* PAdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto adjustForDpi = reinterpret_cast<PAdjustWindowRectExForDpi>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
    DWORD exStyle = config_.always_on_top ? WS_EX_TOPMOST : 0;
    if (config_.borderless) {
        // 无边框窗口：WS_EX_APPWINDOW 确保出现在任务栏和 Alt+Tab 列表中
        exStyle |= WS_EX_APPWINDOW;
    }
    if (config_.reveal_when_ready) {
        // 「就绪后再显示」靠整窗 alpha 实现，所以必须带 WS_EX_LAYERED
        exStyle |= WS_EX_LAYERED;
    }
    if (adjustForDpi) {
        adjustForDpi(&rect, style, FALSE, exStyle, targetDpi);
    } else {
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
    }

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (config_.center) {
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        x = (screenW - (rect.right - rect.left)) / 2;
        y = (screenH - (rect.bottom - rect.top)) / 2;
    }

    // 关键：创建时不显示窗口（不传WS_VISIBLE）。窗口要等 WebView2 就绪后
    // 由 RevealWindow() 显示，见 InitWebView / Run
    hwnd_ = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        Utf8ToWide(config_.title).c_str(),
        style, x, y,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, wc.hInstance, this
    );

    if (!hwnd_) return false;

    // 无边框窗口：启用 DWM 圆角（Windows 11+）
    if (config_.borderless) {
        // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2
        const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        const DWORD DWMWCP_ROUND = 2;
        DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &DWMWCP_ROUND, sizeof(DWMWCP_ROUND));

        // 扩展窗口边框到客户区，使圆角区域透明而非黑色
        MARGINS margins = { 1, 1, 1, 1 };
        DwmExtendFrameIntoClientArea(hwnd_, &margins);
    }

    return true;
}

// ============================================================================
// 窗口过程
// ============================================================================
LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_SIZE: {
        if (self && self->controller_) {
            RECT bounds;
            GetClientRect(hwnd, &bounds);
            self->controller_->put_Bounds(bounds);
        }
        if (self && self->on_resize_ && wParam != SIZE_MINIMIZED) {
            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            self->on_resize_(clientRect.right, clientRect.bottom);
        }
        if (self) {
            if (wParam == SIZE_MINIMIZED && self->on_minimize_) {
                self->on_minimize_();
            }
            if (wParam == SIZE_MAXIMIZED && self->on_maximize_) {
                self->on_maximize_();
            }
        }
        return 0;
    }

    case WM_DPICHANGED: {
        // DPI 变化（拖到不同缩放比例的屏幕）：重新调整窗口尺寸 + WebView2 bounds
        // 确保 WebView2 始终以物理像素渲染，避免位图拉伸导致模糊
        if (self) {
            UINT newDpi = LOWORD(wParam);
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                    suggested->left, suggested->top,
                    suggested->right - suggested->left,
                    suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (self->controller_) {
                RECT bounds;
                GetClientRect(hwnd, &bounds);
                self->controller_->put_Bounds(bounds);
            }
            (void)newDpi;
        }
        return 0;
    }

    case WM_ACTIVATE:
        if (self && self->on_focus_ && LOWORD(wParam) != WA_INACTIVE) {
            self->on_focus_();
        }
        return 0;

    case WM_TIMER:
        // 「就绪后再显示」的兜底：万一 WebView2 一直不报导航完成（或已经失败），
        // 也必须把窗口显示出来 —— 否则窗口永远不可见，用户只能去任务管理器杀。
        // 这条兜底是因为 2026-10-07 真的踩过一次"窗口再也不出现"的坑
        if (self && wParam == kRevealFallbackTimer) {
            self->RevealWindow();
            return 0;
        }
        break;

    case WM_TAURICP_EXECUTE_JS:
        if (self) {
            self->ExecuteJsFromQueue();
        }
        return 0;
    case WM_TAURICP_POST_MESSAGE:
        if (self) {
            self->PostWebMessageFromQueue();
        }
        return 0;

    case WM_CLOSE:
        if (self && self->on_close_) {
            if (!self->on_close_()) {
                // 用户回调拒绝关闭
                return 0;
            }
        }
        break;

    case WM_DESTROY:
        if (self) {
            self->shutting_down_ = true;
            // 窗口临死前必须撤掉拖放注册，否则 OLE 还攥着一个已经没了的 HWND
            tauricpp::UninstallFileDropTree(hwnd);
        }
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        // F12 切换 DevTools
        if (self && wParam == VK_F12 && self->config_.devtools) {
            self->ToggleDevTools();
        }
        return 0;

    case WM_ERASEBKGND: {
        // 客户区必须被真正填上一次颜色。窗口是先于 WebView2 显示的（见 Run），
        // 而 WebView2 又是透明背景，所以从窗口显示到页面首次绘制这段时间里，
        // 屏幕上就是这块客户区：不填的话它是一块从未绘制过的表面，Windows 会
        // 露出白色，也就是启动时那一段白屏
        if (!self) break;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, self->bg_brush_);
        return 1;
    }

    case WM_NCCALCSIZE:
        // 无边框窗口：去掉整个非客户区，窗口内容占满整个窗口
        if (self && self->config_.borderless && wParam) {
            return 0;  // 返回0表示客户端区域就是整个窗口
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_NCHITTEST: {
        // 无边框窗口：手动实现边缘拖拽缩放
        if (self && self->config_.borderless) {
            LRESULT hit = DefWindowProcW(hwnd, msg, wParam, lParam);
            // 如果 DefWindowProc 返回 HTCLIENT（客户区），检查是否在边缘
            if (hit == HTCLIENT) {
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                ScreenToClient(hwnd, &pt);

                RECT rc;
                GetClientRect(hwnd, &rc);

                // 边缘检测区域（8像素）
                const int edgeSize = 8;
                bool top = pt.y < edgeSize;
                bool bottom = pt.y >= rc.bottom - edgeSize;
                bool left = pt.x < edgeSize;
                bool right = pt.x >= rc.right - edgeSize;

                // 角落优先
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                // 边缘
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
            }
            return hit;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// 设置WebView2默认背景色 - 消除白屏核心方案
// ============================================================================
void Window::SetWebViewBackgroundColor() {
    if (!controller_) return;

    // 使用 ICoreWebView2Controller2 设置默认背景色
    // 这样 WebView2 在页面加载前就显示指定颜色而非白色
    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller_->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        COREWEBVIEW2_COLOR bgColor;
        // 无边框圆角窗口：WebView2 背景设为透明，让 DWM 圆角裁剪生效
        if (config_.borderless) {
            bgColor.A = 0;  // 完全透明
            bgColor.R = 0;
            bgColor.G = 0;
            bgColor.B = 0;
        } else {
            bgColor.A = 255;
            bgColor.R = GetRValue(config_.bg_color);
            bgColor.G = GetGValue(config_.bg_color);
            bgColor.B = GetBValue(config_.bg_color);
        }
        controller2->put_DefaultBackgroundColor(bgColor);
    }
}

// ============================================================================
// 初始化WebView2
// ============================================================================
bool Window::InitWebView() {
    // 使用exe名称作为稳定的基础路径，确保IndexedDB等持久数据跨启动保留
    WCHAR tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    WCHAR exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    WCHAR* exeName = wcsrchr(exePath, L'\\');
    exeName = exeName ? exeName + 1 : exePath;
    std::wstring userDataFolder = std::wstring(tempDir) + L"tauricpp_" + exeName;
    CreateDirectoryW(userDataFolder.c_str(), nullptr);

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                // WebView2 起不来时也得把窗口显示出来：否则进程在后台活着，
                // 屏幕上却什么都没有，用户只能去任务管理器杀
                if (FAILED(result)) {
                    RevealWindow();
                    ReportWebView2Failure(result, L"创建 WebView2 环境");
                    return result;
                }

                env_ = env;
                env_->AddRef();

                env->CreateCoreWebView2Controller(hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                         [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                             // 同上：控制器创建失败也要让窗口可见并报错
                             if (FAILED(result)) {
                                 RevealWindow();
                                 ReportWebView2Failure(result, L"创建 WebView2 控制器");
                                 return result;
                             }

                            controller_ = controller;
                            controller_->AddRef();

                            controller_->get_CoreWebView2(&webview_);
                            if (webview_) {
                                webview_->AddRef();
                            }

                            // ★ 关键：默认背景色必须在 put_Bounds 之前设好。
                            // put_Bounds 一调 WebView2 就开始参与合成，此时背景若还是
                            // 默认的白色就会闪一下
                            SetWebViewBackgroundColor();

                            // 设置WebView填满窗口
                            RECT bounds;
                            GetClientRect(hwnd_, &bounds);
                            controller_->put_Bounds(bounds);

                            // 设置虚拟主机名映射（将tauricpp://映射到虚拟文件系统）
                            SetupResourceInterception();

                            // 设置通信桥接
                            SetupBridge();

                            // 接管原生文件拖放（必须在 WebView2 就绪后，
                            // 此时它的子窗口才存在）
                            SetupFileDrop();

                            // 注册窗口操作命令
                            RegisterWindowCommands();

                            // 导航到起始URL
                            webview_->Navigate(Utf8ToWide(config_.start_url).c_str());

                            // ★ 导航完成后刷新WebView布局
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        // 导航完成后重新设置bounds确保WebView正确布局
                                        if (controller_) {
                                            RECT bounds;
                                            GetClientRect(hwnd_, &bounds);
                                            controller_->put_Bounds(bounds);
                                        }
                                        // 页面已经有了内容，这时候才把透明窗口显示出来
                                        RevealWindow();
                                        // 首次 SetupFileDrop 时 WebView2 的子窗口
                                        // 可能还没建出来，导航完成后再补一次。
                                        // 已经注册过的话这里是空操作
                                        SetupFileDrop();
                                        return S_OK;
                                    }
                                ).Get(),
                                &event_tokens_.impl->navigationCompletedToken
                            );

                            webview_ready_ = true;

                            return S_OK;
                        }
                    ).Get()
                );
                return S_OK;
            }
        ).Get()
    );

    // 异步初始化，不等待（WebView2回调在消息循环中分发）
    if (FAILED(hr)) {
        ReportWebView2Failure(hr, L"创建 WebView2 环境");
        return false;
    }
    return true;
}

// ============================================================================
// 设置WebResourceRequested拦截 - 核心防泄露机制
// ============================================================================
void Window::SetupResourceInterception() {
    if (!webview_) return;

    // 方案：SetVirtualHostNameToFolderMapping 映射到实际前端目录
    // 这样 WebView2 识别 tauricpp.app 为本地域名，不会发起真实网络请求
    // WebResourceRequested 拦截请求，优先从 VirtualFS 内存提供内容
    // 如果 VirtualFS 中没有（开发阶段未加载），则回退到文件系统映射

    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview3)))) {
        // 映射到 exe 旁边的 sample/frontend 目录（作为兜底）
        // 生产环境中 VirtualFS 从 exe 资源加载，此目录可以不存在
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        // 获取 exe 所在目录
        WCHAR* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = L'\0';

        // 始终映射虚拟主机名
        std::wstring frontendDir;
        std::vector<std::wstring> candidates = {
            std::wstring(exePath) + L"\\..\\..\\sample\\frontend",
            std::wstring(exePath) + L"\\frontend",
        };

        for (const auto& cand : candidates) {
            WCHAR absPath[MAX_PATH];
            if (GetFullPathNameW(cand.c_str(), MAX_PATH, absPath, nullptr)) {
                if (GetFileAttributesW(absPath) != INVALID_FILE_ATTRIBUTES) {
                    frontendDir = absPath;
                    break;
                }
            }
        }

        // 没找到前端目录时，将VirtualFS内容写入临时目录
        if (frontendDir.empty()) {
            WCHAR tempDir[MAX_PATH];
            GetTempPathW(MAX_PATH, tempDir);
            std::wstring vfsDir = std::wstring(tempDir) + L"tauricpp_vfs_" + std::to_wstring(GetCurrentProcessId());
            CreateDirectoryW(vfsDir.c_str(), nullptr);
            frontendDir = vfsDir;

            // 将VirtualFS中的所有文件写入临时目录
            auto paths = VirtualFS::Instance().GetAllPaths();
            for (const auto& vpath : paths) {
                VirtualFS::VFile file;
                if (VirtualFS::Instance().FindFile(vpath, file)) {
                    // 使用正确的UTF-8转Wide转换
                    std::wstring wPath = Utf8ToWide(vpath);
                    // 去掉开头的 /
                    if (!wPath.empty() && wPath[0] == L'/') wPath = wPath.substr(1);
                    // 将 / 替换为 \，确保 Windows 路径正确
                    for (auto& ch : wPath) {
                        if (ch == L'/') ch = L'\\';
                    }
                    std::wstring fullPath = vfsDir + L"\\" + wPath;

                    // 创建子目录
                    std::wstring dirPart = fullPath.substr(0, fullPath.rfind(L'\\'));
                    for (size_t pos = vfsDir.size(); pos < dirPart.size(); ) {
                        pos = dirPart.find(L'\\', pos);
                        if (pos == std::wstring::npos) pos = dirPart.size();
                        std::wstring subDir = dirPart.substr(0, pos);
                        CreateDirectoryW(subDir.c_str(), nullptr);
                        pos++;
                    }

                    // 写入文件
                    HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_WRITE, 0,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        WriteFile(hFile, file.data.data(), (DWORD)file.data.size(), &written, nullptr);
                        CloseHandle(hFile);
                    }
                }
            }
        }

        webview3->SetVirtualHostNameToFolderMapping(
            L"tauricpp.app",
            frontendDir.c_str(),
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
        );

        // 额外映射的本地目录（用户素材）：目录先建出来，前端"打开文件夹"才点得开。
        // 这些主机名不在下面的 WebResourceRequested 过滤器里，由 WebView2 直接
        // 从磁盘读，不走 VirtualFS
        for (const auto& folderHost : config_.folder_hosts) {
            if (folderHost.first.empty() || folderHost.second.empty()) continue;

            CreateDirectoryW(folderHost.second.c_str(), nullptr);

            std::wstring hostW = Utf8ToWide(folderHost.first);
            if (hostW.empty()) continue;

            webview3->SetVirtualHostNameToFolderMapping(
                hostW.c_str(),
                folderHost.second.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
            );
        }
    }

    // 拦截所有 tauricpp.app 请求，从内存VirtualFS提供内容
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        webview2->AddWebResourceRequestedFilter(L"https://tauricpp.app/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

        webview2->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);

                    LPWSTR uriW = nullptr;
                    request->get_Uri(&uriW);
                    std::string uri = WideToUtf8(uriW);
                    CoTaskMemFree(uriW);

                    // 解析虚拟路径：https://tauricpp.app/path -> /path
                    const std::string scheme = "https://tauricpp.app";
                    std::string vpath;
                    if (uri.find(scheme) == 0) {
                        vpath = uri.substr(scheme.size());
                        auto qpos = vpath.find('?');
                        if (qpos != std::string::npos) vpath = vpath.substr(0, qpos);
                        auto hpos = vpath.find('#');
                        if (hpos != std::string::npos) vpath = vpath.substr(0, hpos);
                        if (vpath.empty() || vpath[0] != '/') vpath = "/" + vpath;
                    } else {
                        return S_OK;
                    }

                    // 从虚拟文件系统查找
                    VirtualFS::VFile file;
                    if (!VirtualFS::Instance().FindFile(vpath, file)) {
                        // SPA回退：如果找不到文件且不是资源文件，返回index.html
                        // 这使得前端路由（如 /settings, /about）可以正常工作
                        bool isStaticAsset = vpath.rfind('.') != std::string::npos;
                        // 排除常见静态资源扩展名
                        static const char* assetExts[] = {
                            ".js", ".css", ".png", ".jpg", ".jpeg", ".gif",
                            ".svg", ".ico", ".woff", ".woff2", ".ttf", ".otf",
                            ".wasm", ".json", ".xml", ".txt", ".webp", ".map", nullptr
                        };
                        bool isAsset = false;
                        for (auto ext = assetExts; *ext; ++ext) {
                            if (vpath.size() >= strlen(*ext) &&
                                _stricmp(vpath.c_str() + vpath.size() - strlen(*ext), *ext) == 0) {
                                isAsset = true;
                                break;
                            }
                        }

                        if (!isAsset && VirtualFS::Instance().FindFile("/index.html", file)) {
                            // SPA fallback - 返回index.html
                        } else {
                            // VirtualFS 中没有，让 WebView2 从文件系统映射获取（兜底）
                            return S_OK;
                        }
                    }

                    // 从内存提供内容，覆盖文件系统映射
                    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, file.data.size());
                    if (!hGlobal) return E_OUTOFMEMORY;
                    void* pData = GlobalLock(hGlobal);
                    memcpy(pData, file.data.data(), file.data.size());
                    GlobalUnlock(hGlobal);

                    ComPtr<IStream> stream;
                    CreateStreamOnHGlobal(hGlobal, TRUE, &stream);

                    std::wstring headers = L"Content-Type: " + Utf8ToWide(file.mime_type) + L"\r\n"
                                         + L"Access-Control-Allow-Origin: *\r\n"
                                         + L"Cache-Control: no-cache";

                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (env_) {
                        env_->CreateWebResourceResponse(
                            stream.Get(), 200, L"OK", headers.c_str(), &response
                        );
                        args->put_Response(response.Get());
                    }

                    return S_OK;
                }
            ).Get(),
            &event_tokens_.impl->webResourceRequestedToken
        );
    }
}

// ============================================================================
// 注册窗口操作命令（供前端调用）
// ============================================================================
void Window::RegisterWindowCommands() {
    auto& bridge = Bridge::Instance();

    // window.startDrag - 前端拖拽移动窗口
    // ReleaseCapture 只释放“当前线程”的鼠标捕获，必须留在UI线程执行
    bridge.RegisterCommand("window.startDrag", [this](const nlohmann::json&) -> nlohmann::json {
        // 释放鼠标捕获后发送WM_NCLBUTTONDOWN触发系统窗口拖拽
        ReleaseCapture();
        SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return {{"success", true}};
    }, true);

    // window.close - 关闭窗口
    bridge.RegisterCommand("window.close", [this](const nlohmann::json&) -> nlohmann::json {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return {{"success", true}};
    }, true);

    // window.minimize - 最小化窗口
    bridge.RegisterCommand("window.minimize", [this](const nlohmann::json&) -> nlohmann::json {
        ShowWindow(hwnd_, SW_MINIMIZE);
        return {{"success", true}};
    }, true);

    // window.maximize - 最大化/恢复窗口
    bridge.RegisterCommand("window.maximize", [this](const nlohmann::json&) -> nlohmann::json {
        if (IsZoomed(hwnd_)) {
            ShowWindow(hwnd_, SW_RESTORE);
        } else {
            ShowWindow(hwnd_, SW_MAXIMIZE);
        }
        return {{"success", true}};
    }, true);
}

// ============================================================================
// 设置通信桥接
// ============================================================================
void Window::SetupBridge() {
    if (!webview_) return;

    // 设置Bridge的JS执行回调
    Bridge::Instance().SetExecuteJsCallback([this](const std::string& js) {
        ExecuteJs(js);
    });

    // 设置Bridge的消息投递回调（后台线程的命令结果经此回到UI线程发送）
    Bridge::Instance().SetPostMessageCallback([this](const std::string& json) {
        PostWebMessage(json);
    });

    // 监听来自前端的消息
    ComPtr<ICoreWebView2_2> webview2;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview2)))) {
        webview2->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR msgW = nullptr;
                    args->get_WebMessageAsJson(&msgW);
                    std::string msgJson = WideToUtf8(msgW);
                    CoTaskMemFree(msgW);

                    try {
                        auto parsed = nlohmann::json::parse(msgJson);
                        nlohmann::json msg;
                        if (parsed.is_string()) {
                            msg = nlohmann::json::parse(parsed.get<std::string>());
                        } else {
                            msg = parsed;
                        }

                        if (msg.contains("__tauricpp_invoke") && msg["__tauricpp_invoke"].get<bool>()) {
                            int id = msg["id"].get<int>();
                            std::string cmd = msg["cmd"].get<std::string>();
                            std::string argsStr = msg["args"].dump();

                            // ★ 异步分发：命令在后台线程执行，避免网络/磁盘IO阻塞UI线程
                            // （结果通过Bridge::PostMessage回到UI线程再发给前端）
                            Bridge::Instance().InvokeAsync(cmd, argsStr, [id](const std::string& resultJson) {
                                // 将结果发回前端
                                nlohmann::json response;
                                response["__tauricpp_result"] = true;
                                response["id"] = id;
                                try {
                                    response["result"] = nlohmann::json::parse(resultJson);
                                } catch (...) {
                                    response["result"] = resultJson;
                                }

                                // Use replace error handler to avoid type_error.316 on non-UTF-8 strings
                                Bridge::Instance().PostMessageToWebview(
                                    response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
                            });
                        }
                    } catch (const std::exception&) {}

                    return S_OK;
                }
            ).Get(),
            &event_tokens_.impl->webMessageReceivedToken
        );
    }

    // 在文档创建时注入桥接JS（在页面脚本执行之前）
    ComPtr<ICoreWebView2_5> webview5;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview5)))) {
        std::string bridgeJs = Bridge::GetBridgeJs();
        webview5->AddScriptToExecuteOnDocumentCreated(
            Utf8ToWide(bridgeJs).c_str(), nullptr
        );
    } else {
        // 回退：导航完成后注入桥接JS
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                    std::string bridgeJs = Bridge::GetBridgeJs();
                    sender->ExecuteScript(Utf8ToWide(bridgeJs).c_str(), nullptr);
                    return S_OK;
                }
            ).Get(),
            &event_tokens_.impl->sourceChangedToken  // 复用sourceChangedToken作为回退令牌
        );
    }
}

// ============================================================================
// 接管原生文件拖放
// ============================================================================
// WebView2 不会把被拖入文件的真实路径交给页面（File 对象没有 path），
// 所以只能关掉它内建的外部拖放，自己注册 OLE 拖放目标。
//
// 注意：OLE 是按 WindowFromPoint（光标下最深的那个窗口）去找拖放目标的。
// WebView2 的窗口层级在不同 SDK 版本下不一样，只猜一个"看起来对"的子窗口
// 注册，很可能注册到一个永远收不到拖放的窗口上 —— 症状就是拖进去毫无反应。
// 所以这里直接把宿主的整棵窗口树全注册一遍，注册到哪一层由运行时决定。
void Window::SetupFileDrop() {
    if (!hwnd_ || !controller_) return;

    // 关掉 WebView2 自己的外部拖放，让它撤掉内建的 IDropTarget，
    // 否则我们注册不上去
    ComPtr<ICoreWebView2Controller4> controller4;
    if (SUCCEEDED(controller_->QueryInterface(IID_PPV_ARGS(&controller4)))) {
        controller4->put_AllowExternalDrop(FALSE);
    }

    // hwnd_ 及其所有后代窗口
    InstallFileDropTree(hwnd_, Bridge::Instance());
}

// ============================================================================
void Window::ExecuteJs(const std::string& js) {
    if (shutting_down_ || !hwnd_) return;

    // 入队JS字符串
    {
        std::lock_guard<std::mutex> lock(jsQueueMutex_);
        jsQueue_.push(js);
    }
    // 通知UI线程处理
    PostMessageW(hwnd_, WM_TAURICP_EXECUTE_JS, 0, 0);
}

// 在UI线程处理JS执行队列
void Window::ExecuteJsFromQueue() {
    if (!webview_ || !webview_ready_ || shutting_down_) return;
    
    while (true) {
        std::string js;
        {
            std::lock_guard<std::mutex> lock(jsQueueMutex_);
            if (jsQueue_.empty()) break;
            js = jsQueue_.front();
            jsQueue_.pop();
        }
        // Log first 80 chars for debugging
        std::string jsPreview = js.substr(0, 80);
        char buf[256];
        snprintf(buf, sizeof(buf), "[TauriCPP] ExecuteJsFromQueue: (len=%zu): %s\n", js.size(), jsPreview.c_str());
        OutputDebugStringA(buf);
        webview_->ExecuteScript(Utf8ToWide(js).c_str(), nullptr);
    }
}

// ============================================================================
// 向WebView投递消息 - 通过PostMessage投递到UI线程安全发送
// ============================================================================
void Window::PostWebMessage(const std::string& json) {
    if (shutting_down_ || !hwnd_) return;

    // 入队消息JSON
    {
        std::lock_guard<std::mutex> lock(webMsgQueueMutex_);
        webMsgQueue_.push(json);
    }
    // 通知UI线程处理
    PostMessageW(hwnd_, WM_TAURICP_POST_MESSAGE, 0, 0);
}

// 在UI线程处理WebView消息队列
void Window::PostWebMessageFromQueue() {
    if (!webview_ || !webview_ready_ || shutting_down_) return;

    while (true) {
        std::string json;
        {
            std::lock_guard<std::mutex> lock(webMsgQueueMutex_);
            if (webMsgQueue_.empty()) break;
            json = std::move(webMsgQueue_.front());
            webMsgQueue_.pop();
        }
        webview_->PostWebMessageAsJson(Utf8ToWide(json).c_str());
    }
}

// ============================================================================
// 窗口操作API
// ============================================================================
void Window::SetTitle(const std::string& title) {
    if (hwnd_) {
        SetWindowTextW(hwnd_, Utf8ToWide(title).c_str());
    }
}

void Window::SetSize(int width, int height) {
    if (!hwnd_) return;
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    RECT rect = { 0, 0, width, height };
    AdjustWindowRect(&rect, style, FALSE);
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void Window::SetPosition(int x, int y) {
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void Window::SetAlwaysOnTop(bool on_top) {
    if (hwnd_) {
        HWND zIndex = on_top ? HWND_TOPMOST : HWND_NOTOPMOST;
        SetWindowPos(hwnd_, zIndex, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void Window::SetResizable(bool resizable) {
    if (!hwnd_) return;
    DWORD style = GetWindowLongW(hwnd_, GWL_STYLE);
    if (resizable) {
        style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
        style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLongW(hwnd_, GWL_STYLE, style);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

void Window::SetIcon(HICON icon) {
    if (hwnd_ && icon) {
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    }
}

void Window::Minimize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
}

void Window::Maximize() {
    if (hwnd_) ShowWindow(hwnd_, SW_MAXIMIZE);
}

void Window::Restore() {
    if (hwnd_) ShowWindow(hwnd_, SW_RESTORE);
}

// 取消 Windows 宏 IsMinimized / IsMaximized / IsIconic，避免与类方法名冲突
#ifdef IsMinimized
#undef IsMinimized
#endif
#ifdef IsMaximized
#undef IsMaximized
#endif
#ifdef IsIconic
#undef IsIconic
#endif

bool Window::IsMinimized() const {
    return hwnd_ ? ::IsIconic(hwnd_) : false;
}

bool Window::IsMaximized() const {
    return hwnd_ ? ::IsZoomed(hwnd_) : false;
}

bool Window::IsFocused() const {
    return hwnd_ ? (GetForegroundWindow() == hwnd_) : false;
}

void Window::ToggleDevTools() {
    if (!webview_) return;
    ComPtr<ICoreWebView2_3> webview3;
    if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview3)))) {
        ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> receiver;
        // 使用 CDP 命令切换 DevTools
        static bool devtools_open = false;
        if (devtools_open) {
            webview3->CallDevToolsProtocolMethod(L"Page.close", L"{}", nullptr);
        } else {
            webview3->CallDevToolsProtocolMethod(L"Page.enable", L"{}", nullptr);
            // 打开DevTools窗口
            ComPtr<ICoreWebView2_6> webview6;
            if (SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&webview6)))) {
                webview6->OpenDevToolsWindow();
            }
        }
        devtools_open = !devtools_open;
    }
}

// ============================================================================
// 从队列取出并执行JS（必须在UI线程调用）
// ============================================================================
// ============================================================================
// 运行
// ============================================================================
int Window::Run() {
    // OLE 拖放（RegisterDragDrop）要求本线程做过 OleInitialize。
    // 必须在 WebView2 之前调：WebView2Loader 内部可能已经把线程做成 STA，
    // 此时这里返回 S_FALSE（已初始化），同样满足要求。
    // 返回 RPC_E_CHANGED_MODE 说明线程是 MTA，拖放用不了，但不该拖累整个启动
    const HRESULT oleHr = OleInitialize(nullptr);
    const bool ole_ready = SUCCEEDED(oleHr);

    if (!CreateNativeWindow()) {
        if (ole_ready) OleUninitialize();
        return -1;
    }

    // 记录主线程ID
    mainThreadId_ = GetCurrentThreadId();

    // 窗口必须**先显示**，WebView2 才起得来。实测 2026-10-07：父窗口隐藏时
    // CreateCoreWebView2EnvironmentWithOptions 同步返回 S_OK，但异步回调拿到失败，
    // 连 msedgewebview2.exe 都不会被拉起来（用户数据目录里一个文件都不建），
    // 用户只会看到一个空窗口 —— 所以"把窗口藏着等 WebView2"这条走不通。
    //
    // reveal_when_ready 时改成"可见但全透明"：对 WebView2 而言窗口是可见的，
    // 对用户而言什么都看不到；等导航完成后由 RevealWindow() 拉回不透明。
    // 白屏的两条治本是：客户区真实填充（WM_ERASEBKGND）+ 背景色与页面底色一致
    if (config_.reveal_when_ready) {
        SetWindowAlpha(0);          // 必须在 ShowWindow 之前，否则会闪一下不透明
        ShowWindow(hwnd_, SW_SHOW);
        SetTimer(hwnd_, kRevealFallbackTimer, kRevealFallbackMs, nullptr);
    } else {
        ShowWindow(hwnd_, SW_SHOW);
    }
    UpdateWindow(hwnd_);

    if (!InitWebView()) {
        if (ole_ready) OleUninitialize();
        return -1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 撤销拖放注册并释放 OLE，必须在窗口销毁之后、线程退出之前
    tauricpp::UninstallFileDropTree(hwnd_);
    if (ole_ready) OleUninitialize();

    return static_cast<int>(msg.wParam);
}

// ============================================================================
// 显示窗口
// ============================================================================
// 「就绪后再显示」的窗口是 WS_EX_LAYERED + alpha=0：按 Windows 的定义它是"可见"的
// （WebView2 只能在这种窗口上起来），但用户什么都看不到。这里把 alpha 拉回 255，
// 顺带撤掉兜底计时器
void Window::RevealWindow() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kRevealFallbackTimer);
    if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOW);
    }
    if (config_.reveal_when_ready) {
        SetWindowAlpha(255);
    }
    UpdateWindow(hwnd_);
}

void Window::SetWindowAlpha(BYTE alpha) {
    if (!hwnd_) return;
    SetLayeredWindowAttributes(hwnd_, 0, alpha, LWA_ALPHA);
}

void Window::Close() {
    if (hwnd_) {
        PostMessage(hwnd_, WM_CLOSE, 0, 0);
    }
}

} // namespace tauricpp
