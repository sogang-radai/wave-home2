#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <drogon/orm/DbClient.h>

#include "trigger_types.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

struct RuleView
{
    Rule rule;
    std::string actionDeviceName;
    std::string triggerDeviceName;
};

class RuleStore
{
public:
    using ChangedCallback = std::function<void()>;

    void setDatabaseClient(const drogon::orm::DbClientPtr& client);
    void setDefaultUserId(int64_t user_id);
    void setLegacyImportPath(const std::filesystem::path& path);
    void setOnChanged(ChangedCallback callback);

    bool loadFromDatabase(std::string& out_error);

    std::vector<RuleView> list() const;
    std::vector<RuleView> listForDevice(const std::string& device_id) const;
    std::optional<RuleView> get(const std::string& rule_id) const;
    size_t activeCount() const;

    std::future<RuleView> createAsync(const json& payload);
    std::future<RuleView> updateAsync(const std::string& rule_id, const json& patch);
    std::future<bool> deleteAsync(const std::string& rule_id);
    std::future<RuleView> setEnabledAsync(const std::string& rule_id, bool enabled);

    TriggerIndexSnapshot snapshot() const;

    static RuleView toView(const Rule& rule, const std::unordered_map<std::string, Trigger>& triggers);
    static bool validatePayload(const json& payload, std::string& out_error);

private:
    bool importLegacyRulesFile(std::string& out_error);
    bool insertRuleToDatabase(const Rule& rule, int64_t user_id, std::string& out_error);
    bool updateRuleInDatabase(const Rule& rule, int64_t user_id, std::string& out_error);
    bool deleteRuleFromDatabase(const std::string& external_id, std::string& out_error);

    void rebuildIndex();
    std::string nextRuleId() const;

    bool applyCreate(const json& payload, RuleView& out_view, std::string& out_error);
    bool applyUpdate(const std::string& rule_id, const json& patch, RuleView& out_view, std::string& out_error);
    bool applyDelete(const std::string& rule_id, std::string& out_error);
    bool applySetEnabled(const std::string& rule_id, bool enabled, RuleView& out_view, std::string& out_error);

    mutable std::shared_mutex m_mutex;
    std::vector<Rule> m_rules;
    std::unordered_map<std::string, Trigger> m_triggers;
    TriggerIndexSnapshot m_index;
    drogon::orm::DbClientPtr m_db;
    int64_t m_defaultUserId = 1;
    std::filesystem::path m_legacyImportPath;
    ChangedCallback m_onChanged;
};

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
