#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <nlohmann/json.hpp>
#include "tauricpp/app.hpp"

namespace Bridge {

// Webview 桥接接口 - 封装与前端的通信
class WebviewBridge {
public:
    using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

    WebviewBridge(tauricpp::App& app) : app_(app) {}

    // 注册命令
    void RegisterCommand(const std::string& cmd, CommandHandler handler) {
        app_.GetBridge().RegisterCommand(cmd, std::move(handler));
    }

    // 发送事件到前端
    void Emit(const std::string& event, const nlohmann::json& data) {
        app_.GetBridge().Emit(event, data);
    }

    // 获取 App 引用（用于特殊场景）
    tauricpp::App& GetApp() { return app_; }

private:
    tauricpp::App& app_;
};

} // namespace Bridge
