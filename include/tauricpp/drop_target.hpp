#pragma once
#include <Windows.h>

namespace tauricpp {

class Bridge;

/// 原生文件拖放接收器。
///
/// WebView2 出于安全不把被拖入文件的真实路径交给页面 —— `DataTransfer.files`
/// 里的 File 对象没有 path，`webkitGetAsEntry()` 也拿不到，所以"拖整合包进窗口"
/// 这件事在 JS 层根本做不了，必须在原生层接管拖放。
///
/// 只放行压缩包（zip / mrpack / rar）；其它文件一律不给放置光标反馈。
///
/// 事件：
///   app.fileDragEnter  { count, first }   文件悬停在窗口上
///   app.fileDragLeave  {}                 拖出窗口
///   app.fileDrop       { paths: [...] }   松手，附真实路径
///
/// 实现要点：OLE 是按 WindowFromPoint（光标下最深的那个窗口）去找拖放目标的，
/// 而 WebView2 的窗口层级在不同 SDK 版本下并不一样，只注册"看起来对"的那一个
/// 子窗口会收不到任何拖放。所以这里把整棵窗口树都注册上，拖放落到哪一层都能命中。

/// 把 root 及其所有后代窗口都注册成拖放目标。重复调用是安全的。
bool InstallFileDropTree(HWND root, Bridge& bridge);

/// 撤销整棵窗口树上的拖放注册。必须先于窗口销毁调用，
/// 否则 OLE 会持有已失效的 HWND。
void UninstallFileDropTree(HWND root);

} // namespace tauricpp
