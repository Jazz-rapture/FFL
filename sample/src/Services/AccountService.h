#pragma once
#include <nlohmann/json.hpp>
#include "Core/ConfigManager.h"
#include "Core/HashUtils.h"
#include "Bridge/WebviewBridge.h"

namespace Services {

class AccountService {
public:
    AccountService(Bridge::WebviewBridge& bridge) : bridge_(bridge) {
        RegisterCommands();
    }

private:
    void RegisterCommands() {
        // account.read - 读取配置文件
        bridge_.RegisterCommand("account.read", [](const nlohmann::json&) -> nlohmann::json {
            std::string content = Core::ConfigManager::ReadConfigFile();
            try {
                return nlohmann::json::parse(content);
            } catch (...) {
                return {{"accounts", nlohmann::json::array()}};
            }
        });

        // account.write - 写入配置文件（指定卡片的登录信息）
        bridge_.RegisterCommand("account.write", [](const nlohmann::json& args) -> nlohmann::json {
            int index = args.value("index", -1);
            std::string id = args.value("id", "");

            if (index < 0 || index > 2 || id.empty()) {
                return {{"success", false}, {"error", "Invalid parameters"}};
            }

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                config = {{"accounts", nlohmann::json::array()}};
            }

            if (!config.contains("accounts") || !config["accounts"].is_array()) {
                config["accounts"] = nlohmann::json::array();
            }
            while (config["accounts"].size() < 3) {
                config["accounts"].push_back("");
            }

            std::string uuid = Core::HashUtils::GenerateOfflineUUID(id);
            config["accounts"][index] = id;

            if (!config.contains("uuids") || !config["uuids"].is_array()) {
                config["uuids"] = nlohmann::json::array();
            }
            while (config["uuids"].size() < 3) {
                config["uuids"].push_back("");
            }
            config["uuids"][index] = uuid;

            bool ok = Core::ConfigManager::WriteConfigFile(config.dump(4));
            return {{"success", ok}};
        });

        // account.check - 检查指定卡片是否有登录信息
        bridge_.RegisterCommand("account.check", [](const nlohmann::json& args) -> nlohmann::json {
            int index = args.value("index", -1);
            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                return {{"has_account", false}};
            }
            return {{"has_account", Core::ConfigManager::HasAccount(config, index)}};
        });
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
