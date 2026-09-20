#include "tauricpp/bridge.hpp"
#include <sstream>

namespace tauricpp {

Bridge& Bridge::Instance() {
    // 有意不析构：后台工作线程在进程存活期间阻塞在条件变量上，
    // 若在进程退出时析构队列/条件变量会与工作线程产生未定义行为
    static Bridge* instance = new Bridge();
    return *instance;
}

void Bridge::RegisterCommand(const std::string& cmd, InvokeHandler handler, bool ui_thread) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_[cmd] = Command{std::move(handler), ui_thread};
}

void Bridge::UnregisterCommand(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.erase(cmd);
}

bool Bridge::RequiresUiThread(const std::string& cmd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = commands_.find(cmd);
    return it != commands_.end() && it->second.ui_thread;
}

std::string Bridge::HandleInvoke(const std::string& cmd, const std::string& args_json) {
    // ★ 修复：先在锁内查找handler，再在锁外调用
    // 之前整个函数持有mutex_调用handler，如果handler调用Emit会死锁
    InvokeHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = commands_.find(cmd);
        if (it == commands_.end()) {
            nlohmann::json result;
            result["error"] = "Unknown command: " + cmd;
            return result.dump();
        }
        handler = it->second.handler;
    }
    // handler在锁外执行，避免死锁

    try {
        nlohmann::json args = nlohmann::json::parse(args_json);
        nlohmann::json result = handler(args);
        // Use replace error handler to avoid type_error.316 on non-UTF-8 strings
        std::string dumped = result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return dumped;
    } catch (const std::exception& e) {
        nlohmann::json result;
        result["error"] = std::string("Exception: ") + e.what();
        return result.dump();
    }
}

void Bridge::InvokeAsync(const std::string& cmd, const std::string& args_json, ResponseCallback on_done) {
    // UI线程命令（窗口拖拽、鼠标捕获等有线程亲和的命令）必须同步在当前线程执行
    if (RequiresUiThread(cmd)) {
        on_done(HandleInvoke(cmd, args_json));
        return;
    }

    // 其余命令投递到后台工作线程，避免网络/磁盘IO阻塞UI线程
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        invoke_queue_.push([this, cmd, args_json, on_done]() {
            on_done(HandleInvoke(cmd, args_json));
        });
        if (!worker_started_) {
            worker_started_ = true;
            // 工作线程随进程存活，无需回收（Bridge为进程级单例）
            std::thread([this]() { WorkerLoop(); }).detach();
        }
    }
    queue_cv_.notify_one();
}

void Bridge::WorkerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this]() { return !invoke_queue_.empty(); });
            job = std::move(invoke_queue_.front());
            invoke_queue_.pop();
        }
        job();
    }
}

void Bridge::SetPostMessageCallback(PostMessageCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_message_ = std::move(cb);
}

void Bridge::PostMessageToWebview(const std::string& json) {
    PostMessageCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = post_message_;
    }
    if (cb) cb(json);
}

void Bridge::Emit(const std::string& event, const nlohmann::json& data) {
    // ★ 修复：用atomic风格的读取（execute_js_只设置一次后不变）
    // 先拷贝到局部变量，避免竞态
    ExecuteJsCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = execute_js_;
    }

    if (!cb) return;

    // ★ 修复XSS：使用JSON.stringify构造安全的JS调用，而非字符串拼接
    // 将event名称也用JSON编码，防止注入单引号等特殊字符
    nlohmann::json callArgs;
    callArgs["event"] = event;
    callArgs["data"] = data;
    std::string js = "__tauricpp_internal_emit(" + callArgs.dump() + ");";

    cb(js);
}

void Bridge::SetExecuteJsCallback(ExecuteJsCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    execute_js_ = std::move(cb);
}

std::string Bridge::GetBridgeJs() {
    return R"js(
(function() {
    const __tauricpp__ = {
        _listeners: {},
        _invokeId: 0,
        _invokeCallbacks: {},

        invoke: function(cmd, args) {
            return new Promise((resolve, reject) => {
                const id = ++this._invokeId;
                this._invokeCallbacks[id] = { resolve, reject };
                window.chrome.webview.postMessage(JSON.stringify({
                    __tauricpp_invoke: true,
                    id: id,
                    cmd: cmd,
                    args: args || {}
                }));
            });
        },

        listen: function(event, callback) {
            if (!this._listeners[event]) {
                this._listeners[event] = [];
            }
            this._listeners[event].push(callback);
            // 返回取消监听函数
            return function() {
                __tauricpp__._listeners[event] = __tauricpp__._listeners[event].filter(cb => cb !== callback);
            };
        },

        removeListener: function(event, callback) {
            if (this._listeners[event]) {
                this._listeners[event] = this._listeners[event].filter(cb => cb !== callback);
            }
        }
    };

    // ★ 安全的事件分发：从JSON对象中读取event和data，不使用字符串拼接
    window.__tauricpp_internal_emit = function(payload) {
        console.log('[TauriCPP] __tauricpp_internal_emit called, payload:', JSON.stringify(payload).substring(0,200));
        if (!payload || typeof payload !== 'object') {
            console.error('[TauriCPP] Invalid payload:', payload);
            return;
        }
        var event = payload.event;
        var data = payload.data;
        console.log('[TauriCPP] Dispatching event "' + event + '", listeners:', __tauricpp__._listeners[event] ? __tauricpp__._listeners[event].length : 0);
        if (__tauricpp__._listeners[event]) {
            __tauricpp__._listeners[event].forEach(function(cb) {
                try {
                    cb(data);
                } catch(e) {
                    console.error('[TauriCPP] Event listener error for "' + event + '":', e);
                }
            });
        }
    };

    // 兼容旧版：直接传event字符串+data的调用方式（从C++ Emit升级后的安全版本）
    window.__tauricpp_internal_onEvent = function(event, data) {
        __tauricpp_internal_emit({event: event, data: data});
    };

    window.chrome.webview.addEventListener('message', function(e) {
        try {
            var msg = (typeof e.data === 'string') ? JSON.parse(e.data) : e.data;
            if (msg.__tauricpp_result && msg.id) {
                var cb = __tauricpp__._invokeCallbacks[msg.id];
                if (cb) {
                    if (msg.error) {
                        cb.reject(new Error(msg.error));
                    } else {
                        cb.resolve(msg.result);
                    }
                    delete __tauricpp__._invokeCallbacks[msg.id];
                }
            }
        } catch(ex) {
            console.error('[TauriCPP] Message processing error:', ex);
        }
    });

    window.__tauricpp__ = __tauricpp__;

    // 屏蔽默认右键菜单
    document.addEventListener('contextmenu', function(e) { e.preventDefault(); }, true);
})();
)js";
}

} // namespace tauricpp
