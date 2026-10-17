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
        // 原样返回 config.json。accountTokens 里是 DPAPI 密文，明文令牌不会离开进程。
        bridge_.RegisterCommand("account.read", [](const nlohmann::json&) -> nlohmann::json {
            std::string content = Core::ConfigManager::ReadConfigFile();
            try {
                return nlohmann::json::parse(content);
            } catch (...) {
                return {{"accounts", nlohmann::json::array()}};
            }
        });

        // account.write - 写入指定槽位的登录信息。id 为空表示清空该槽位
        // （删除角色 / 管理账户都走这条路径，之前空 id 被直接拒绝，导致清不掉）。
        // type = "offline"：UUID 由名字推导；type = "msa"：uuid / token 由调用方给定。
        bridge_.RegisterCommand("account.write", [](const nlohmann::json& args) -> nlohmann::json {
            int index = args.value("index", -1);
            std::string id = args.value("id", "");

            if (index < 0 || index >= Core::ConfigManager::kAccountSlots) {
                return {{"success", false}, {"error", "Invalid parameters"}};
            }

            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                config = nlohmann::json::object();
            }

            if (id.empty()) {
                Core::ConfigManager::WriteAccountSlot(config, index, "", "", "", "");
            } else if (args.value("type", std::string("offline")) == "msa") {
                std::string uuid = args.value("uuid", "");
                std::string token = args.value("token", "");
                if (uuid.empty()) {
                    return {{"success", false}, {"error", "Missing uuid"}};
                }
                Core::ConfigManager::WriteAccountSlot(config, index, id, uuid, "msa", token);
            } else {
                Core::ConfigManager::WriteAccountSlot(
                    config, index, id, Core::HashUtils::GenerateOfflineUUID(id), "offline", "");
            }

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

        // account.select - 记住用户选中的槽位，启动游戏时按它取账号
        bridge_.RegisterCommand("account.select", [](const nlohmann::json& args) -> nlohmann::json {
            int index = args.value("index", -1);
            std::string content = Core::ConfigManager::ReadConfigFile();
            nlohmann::json config;
            try {
                config = nlohmann::json::parse(content);
            } catch (...) {
                config = nlohmann::json::object();
            }
            Core::ConfigManager::SetSelectedAccountIndex(config, index);
            return {{"ok", Core::ConfigManager::WriteConfigFile(config.dump(4))}};
        });
    }

    Bridge::WebviewBridge& bridge_;
};

} // namespace Services
