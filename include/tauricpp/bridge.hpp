#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <utility>
#include <optional>

// 抑制 nlohmann/json.hpp 的 fallthrough 警告（第三方库，无法修改）
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 26819)  // es.78: fallthrough
#endif
#include <nlohmann/json.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace tauricpp {

/// 前后端通信桥接
/// JS -> C++: window.__tauricpp__.invoke(cmd, args) -> 返回Promise
/// C++ -> JS: bridge.Emit(event, data) -> 前端通过 window.__tauricpp__.listen(event, callback) 接收
///
/// 命令默认在后台工作线程串行执行：网络/磁盘操作可能耗时数秒，
/// 若在UI线程执行会阻塞WebView2消息循环，导致整个窗口无响应。
/// 需要线程亲和的命令（鼠标捕获、模态窗口等）用 ui_thread=true 注册，仍在UI线程同步执行。
class Bridge {
public:
    using InvokeHandler = std::function<nlohmann::json(const nlohmann::json& args)>;
    using EventHandler = std::function<void(const nlohmann::json& data)>;
    /// 命令执行完毕的回调，可能在UI线程或后台工作线程被调用
    using ResponseCallback = std::function<void(const std::string& result_json)>;

    static Bridge& Instance();

    // 禁止拷贝和移动
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    /// 注册一个可被前端调用的命令
    /// @param ui_thread 为true时命令在UI线程同步执行（如窗口拖拽、鼠标捕获），
    ///                  为false（默认）时在后台工作线程串行执行
    void RegisterCommand(const std::string& cmd, InvokeHandler handler, bool ui_thread = false);

    /// 注销一个命令
    void UnregisterCommand(const std::string& cmd);

    /// 类型安全的命令注册
    /// 用法: bridge.RegisterCommand<Args, Result>("cmd", [](const Args& args) -> Result { ... });
    /// Args和Result必须是可被nlohmann/json序列化/反序列化的类型
    template<typename ArgsT, typename ResultT>
    void RegisterCommand(const std::string& cmd, std::function<ResultT(const ArgsT&)> handler) {
        RegisterCommand(cmd, [handler = std::move(handler)](const nlohmann::json& args) -> nlohmann::json {
            ArgsT typed_args = args.get<ArgsT>();
            ResultT result = handler(typed_args);
            return nlohmann::json(result);
        });
    }

    /// 简化版：无参数命令注册
    template<typename ResultT>
    void RegisterCommand(const std::string& cmd, std::function<ResultT()> handler) {
        RegisterCommand(cmd, [handler = std::move(handler)](const nlohmann::json&) -> nlohmann::json {
            ResultT result = handler();
            return nlohmann::json(result);
        });
    }

    /// 处理来自前端的调用请求（同步执行，由WebView2的WebMessageReceived触发）
    std::string HandleInvoke(const std::string& cmd, const std::string& args_json);

    /// 异步分发调用请求：ui_thread命令立即同步执行，其余命令交给后台工作线程串行执行。
    /// on_done 可能在任意线程被调用，用于把结果回传给前端。
    void InvokeAsync(const std::string& cmd, const std::string& args_json, ResponseCallback on_done);

    /// 向前端发送事件
    void Emit(const std::string& event, const nlohmann::json& data);

    /// 设置执行JS的回调（由Window设置，用于Emit）
    using ExecuteJsCallback = std::function<void(const std::string& js)>;
    void SetExecuteJsCallback(ExecuteJsCallback cb);

    /// 设置向WebView投递消息的回调（由Window设置，用于异步返回调用结果）
    using PostMessageCallback = std::function<void(const std::string& json)>;
    void SetPostMessageCallback(PostMessageCallback cb);

    /// 向WebView投递一条消息（线程安全，可由任意线程调用）
    /// 命名避开 winuser.h 的 PostMessage 宏
    void PostMessageToWebview(const std::string& json);

    /// 获取注入到前端的桥接JS代码
    static std::string GetBridgeJs();

private:
    Bridge() = default;

    struct Command {
        InvokeHandler handler;
        bool ui_thread = false;
    };

    /// 命令是否要求UI线程执行
    bool RequiresUiThread(const std::string& cmd) const;

    /// 后台工作线程主体：取出队列中的命令并串行执行
    void WorkerLoop();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Command> commands_;
    ExecuteJsCallback execute_js_;
    PostMessageCallback post_message_;

    // 后台命令队列：保证命令串行执行，语义与过去在UI线程串行执行一致
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::queue<std::function<void()>> invoke_queue_;
    bool worker_started_ = false;
};

} // namespace tauricpp
