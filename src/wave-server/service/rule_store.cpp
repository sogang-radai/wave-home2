#include "rule_store.h"

#include <fstream>
#include <sstream>

#include "../core/logger.h"
#include "../core/task_queue.h"
#include "../core/time_util.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    json ruleToJson(const Rule& rule, const std::unordered_map<std::string, Trigger>& triggers)
    {
        json out = json::object();
        out["id"] = rule.id;
        out["name"] = rule.name;
        out["enabled"] = rule.enabled;
        out["cooldownMs"] = rule.cooldownMs;
        out["repeatIntervalMs"] = rule.repeatIntervalMs;
        out["execMode"] = execModeToString(rule.execMode);
        out["action"] = json::object();
        out["action"]["deviceId"] = rule.action.deviceId;
        out["action"]["name"] = rule.action.name;
        out["action"]["params"] = rule.action.params;

        if (!rule.triggerId.empty())
        {
            const auto it = triggers.find(rule.triggerId);
            out["trigger"] = it != triggers.end() ? triggerToJson(it->second) : rule.triggerJson;
        }
        else
        {
            out["trigger"] = nullptr;
        }

        if (rule.schedule)
            out["schedule"] = ruleScheduleToJson(*rule.schedule);
        else
            out["schedule"] = nullptr;

        return out;
    }

    bool parseRuleAction(const json& value, RuleAction& out_action, std::string& out_error)
    {
        if (!value.is_object())
        {
            out_error = "action must be an object";
            return false;
        }
        if (!value.contains("deviceId") || !value["deviceId"].is_string())
        {
            out_error = "action.deviceId is required";
            return false;
        }
        if (!value.contains("name") || !value["name"].is_string())
        {
            out_error = "action.name is required";
            return false;
        }
        out_action.deviceId = value["deviceId"].get<std::string>();
        out_action.name = value["name"].get<std::string>();
        out_action.params = value.contains("params") && value["params"].is_object()
            ? value["params"]
            : json::object();
        return true;
    }

    json actionsPayloadFromRule(const Rule& rule)
    {
        json out = json::object();
        out["deviceId"] = rule.action.deviceId;
        out["name"] = rule.action.name;
        out["params"] = rule.action.params;
        out["execMode"] = execModeToString(rule.execMode);
        out["repeatIntervalMs"] = rule.repeatIntervalMs;
        return out;
    }

    bool parseActionsPayload(
        const std::string& text,
        RuleAction& out_action,
        ExecMode& out_exec_mode,
        uint32_t& out_repeat_interval_ms,
        std::string& out_error)
    {
        json value;
        try
        {
            value = json::parse(text);
        }
        catch (const std::exception& e)
        {
            out_error = std::string("invalid actions_json: ") + e.what();
            return false;
        }

        if (!value.is_object())
        {
            out_error = "actions_json must be an object";
            return false;
        }

        if (!parseRuleAction(value, out_action, out_error))
            return false;

        out_exec_mode = parseExecMode(value.value("execMode", std::string("once")));
        out_repeat_interval_ms = value.value("repeatIntervalMs", 0u);
        return true;
    }

    bool parseRuleFromJson(const json& value, Rule& out_rule, std::string& out_error)
    {
        if (!value.is_object())
        {
            out_error = "rule must be an object";
            return false;
        }
        if (!value.contains("id") || !value["id"].is_string())
        {
            out_error = "rule.id is required";
            return false;
        }
        if (!value.contains("name") || !value["name"].is_string())
        {
            out_error = "rule.name is required";
            return false;
        }

        out_rule = Rule{};
        out_rule.id = value["id"].get<std::string>();
        out_rule.name = value["name"].get<std::string>();
        out_rule.enabled = value.value("enabled", true);
        out_rule.cooldownMs = value.value("cooldownMs", 0u);
        out_rule.repeatIntervalMs = value.value("repeatIntervalMs", 0u);
        out_rule.execMode = parseExecMode(value.value("execMode", std::string("once")));

        if (!parseRuleAction(value.at("action"), out_rule.action, out_error))
            return false;

        const bool has_trigger = value.contains("trigger") && !value["trigger"].is_null();
        const bool has_schedule = value.contains("schedule") && !value["schedule"].is_null();
        if (!has_trigger && !has_schedule)
        {
            out_error = "rule must have trigger or schedule";
            return false;
        }

        if (has_trigger)
        {
            out_rule.triggerJson = value["trigger"];
            Trigger trigger;
            if (!parseTriggerFromJson(out_rule.triggerJson, trigger, out_error))
                return false;
            out_rule.triggerId = trigger.id;
        }

        if (has_schedule)
        {
            RuleSchedule schedule;
            if (!parseRuleScheduleFromJson(value["schedule"], schedule, out_error))
                return false;
            out_rule.schedule = schedule;
        }

        return true;
    }

    bool hydrateTriggersFromRule(Rule& rule, std::unordered_map<std::string, Trigger>& triggers, std::string& out_error)
    {
        if (rule.triggerId.empty() || !rule.triggerJson.is_object())
            return true;

        Trigger trigger;
        if (!parseTriggerFromJson(rule.triggerJson, trigger, out_error))
            return false;

        triggers[trigger.id] = trigger;
        rule.triggerId = trigger.id;
        return true;
    }

    bool rowToRule(const drogon::orm::Row& row, Rule& out_rule, std::string& out_error)
    {
        out_rule = Rule{};
        out_rule.id = row["external_id"].as<std::string>();
        out_rule.name = row["name"].as<std::string>();
        out_rule.enabled = row["enabled"].as<int>() != 0;
        out_rule.cooldownMs = static_cast<uint32_t>(row["cooldown_ms"].as<int>());

        if (!parseActionsPayload(
                row["actions_json"].as<std::string>(),
                out_rule.action,
                out_rule.execMode,
                out_rule.repeatIntervalMs,
                out_error))
        {
            return false;
        }

        if (!row["trigger_json"].isNull())
        {
            try
            {
                out_rule.triggerJson = json::parse(row["trigger_json"].as<std::string>());
            }
            catch (const std::exception& e)
            {
                out_error = std::string("invalid trigger_json: ") + e.what();
                return false;
            }
        }

        if (!row["schedule_json"].isNull())
        {
            try
            {
                const auto schedule_json = json::parse(row["schedule_json"].as<std::string>());
                RuleSchedule schedule;
                if (!parseRuleScheduleFromJson(schedule_json, schedule, out_error))
                    return false;
                out_rule.schedule = schedule;
            }
            catch (const std::exception& e)
            {
                out_error = std::string("invalid schedule_json: ") + e.what();
                return false;
            }
        }

        return true;
    }

    void insertAutomationRuleRow(
        const drogon::orm::DbClientPtr& db,
        int64_t user_id,
        const Rule& rule,
        const std::string& created_at,
        const std::string& updated_at)
    {
        const std::string trigger_text = rule.triggerJson.is_object() ? rule.triggerJson.dump() : std::string();
        const std::string schedule_text = rule.schedule ? ruleScheduleToJson(*rule.schedule).dump() : std::string();
        const std::string actions_text = actionsPayloadFromRule(rule).dump();

        auto binder = *db
                      << "INSERT INTO automation_rule "
                         "(user_id, external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json, created_at, updated_at) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        binder << user_id;
        binder << rule.id;
        binder << rule.name;
        binder << (rule.enabled ? 1 : 0);
        binder << static_cast<int>(rule.cooldownMs);
        if (trigger_text.empty())
            binder << nullptr;
        else
            binder << trigger_text;
        if (schedule_text.empty())
            binder << nullptr;
        else
            binder << schedule_text;
        binder << actions_text;
        binder << created_at;
        binder << updated_at;
        binder << drogon::orm::Mode::Blocking;
        binder.exec();
    }

    void updateAutomationRuleRow(
        const drogon::orm::DbClientPtr& db,
        int64_t user_id,
        const Rule& rule,
        const std::string& updated_at)
    {
        const std::string trigger_text = rule.triggerJson.is_object() ? rule.triggerJson.dump() : std::string();
        const std::string schedule_text = rule.schedule ? ruleScheduleToJson(*rule.schedule).dump() : std::string();
        const std::string actions_text = actionsPayloadFromRule(rule).dump();

        drogon::orm::Result result(nullptr);
        auto binder = *db
                      << "UPDATE automation_rule SET "
                         "user_id = ?, name = ?, enabled = ?, cooldown_ms = ?, trigger_json = ?, schedule_json = ?, actions_json = ?, updated_at = ? "
                         "WHERE external_id = ?";
        binder << user_id;
        binder << rule.name;
        binder << (rule.enabled ? 1 : 0);
        binder << static_cast<int>(rule.cooldownMs);
        if (trigger_text.empty())
            binder << nullptr;
        else
            binder << trigger_text;
        if (schedule_text.empty())
            binder << nullptr;
        else
            binder << schedule_text;
        binder << actions_text;
        binder << updated_at;
        binder << rule.id;
        binder << drogon::orm::Mode::Blocking;
        binder >> [&result](const drogon::orm::Result& r) { result = r; };
        binder.exec();
        if (result.affectedRows() == 0)
            throw std::runtime_error("rule not found");
    }
}

void RuleStore::setDatabaseClient(const drogon::orm::DbClientPtr& client)
{
    std::unique_lock lock(m_mutex);
    m_db = client;
}

void RuleStore::setDefaultUserId(int64_t user_id)
{
    std::unique_lock lock(m_mutex);
    m_defaultUserId = user_id;
}

void RuleStore::setLegacyImportPath(const std::filesystem::path& path)
{
    std::unique_lock lock(m_mutex);
    m_legacyImportPath = path;
}

void RuleStore::setOnChanged(ChangedCallback callback)
{
    std::unique_lock lock(m_mutex);
    m_onChanged = std::move(callback);
}

bool RuleStore::loadFromDatabase(std::string& out_error)
{
    std::unique_lock lock(m_mutex);
    m_rules.clear();
    m_triggers.clear();

    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    try
    {
        auto count_rows = m_db->execSqlSync("SELECT COUNT(*) AS cnt FROM automation_rule");
        if (!count_rows.empty() && count_rows[0]["cnt"].as<int64_t>() == 0 && !m_legacyImportPath.empty())
        {
            lock.unlock();
            if (!importLegacyRulesFile(out_error))
                return false;
            lock.lock();
        }

        auto rows = m_db->execSqlSync(
            "SELECT external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json "
            "FROM automation_rule ORDER BY id ASC");

        for (const auto& row : rows)
        {
            Rule rule;
            if (!rowToRule(row, rule, out_error))
                return false;

            if (!hydrateTriggersFromRule(rule, m_triggers, out_error))
                return false;

            m_rules.push_back(std::move(rule));
        }

        rebuildIndex();
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

bool RuleStore::importLegacyRulesFile(std::string& out_error)
{
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    if (m_legacyImportPath.empty() || !std::filesystem::exists(m_legacyImportPath))
    {
        out_error = "legacy rules file not found";
        return false;
    }

    try
    {
        json root;
        {
            std::ifstream in(m_legacyImportPath);
            in >> root;
        }

        if (!root.contains("rules") || !root["rules"].is_array())
        {
            out_error = "legacy rules file must contain a rules array";
            return false;
        }

        const auto now = formatTimestamp();
        for (const auto& item : root["rules"])
        {
            Rule rule;
            if (!parseRuleFromJson(item, rule, out_error))
                return false;

            std::unordered_map<std::string, Trigger> triggers;
            if (!hydrateTriggersFromRule(rule, triggers, out_error))
                return false;

            insertAutomationRuleRow(m_db, m_defaultUserId, rule, now, now);
        }

        LOG_INFO("Imported {} rules from {} into automation_rule", root["rules"].size(), m_legacyImportPath.string());
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

bool RuleStore::insertRuleToDatabase(const Rule& rule, int64_t user_id, std::string& out_error)
{
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    try
    {
        const auto now = formatTimestamp();
        insertAutomationRuleRow(m_db, user_id, rule, now, now);
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

bool RuleStore::updateRuleInDatabase(const Rule& rule, int64_t user_id, std::string& out_error)
{
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    try
    {
        const auto now = formatTimestamp();
        updateAutomationRuleRow(m_db, user_id, rule, now);
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

bool RuleStore::deleteRuleFromDatabase(const std::string& external_id, std::string& out_error)
{
    if (!m_db)
    {
        out_error = "database client is not set";
        return false;
    }

    try
    {
        auto result = m_db->execSqlSync("DELETE FROM automation_rule WHERE external_id = ?", external_id);
        if (result.affectedRows() == 0)
        {
            out_error = "rule not found";
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        out_error = e.what();
        return false;
    }
}

void RuleStore::rebuildIndex()
{
    auto index = std::make_shared<TriggerIndex>();
    index->triggers = m_triggers;

    for (const auto& rule : m_rules)
    {
        if (!rule.enabled)
            continue;

        if (rule.schedule)
        {
            index->scheduleRules.push_back(rule);
            continue;
        }

        if (rule.triggerId.empty())
            continue;

        const auto trigger_it = m_triggers.find(rule.triggerId);
        if (trigger_it == m_triggers.end())
            continue;

        TriggerBinding binding;
        binding.triggerId = rule.triggerId;
        binding.ruleId = rule.id;
        binding.ruleName = rule.name;
        binding.execMode = rule.execMode;
        binding.cooldownMs = rule.cooldownMs;
        binding.repeatIntervalMs = rule.repeatIntervalMs;
        binding.action = rule.action;
        binding.gestureClassId = trigger_it->second.classId;

        const Trigger& trigger = trigger_it->second;
        switch (trigger.kind)
        {
        case TriggerKind::Gesture:
        {
            GestureIndexKey key{trigger.sourceDeviceId, trigger.gestureSetPath};
            index->gesture[key].push_back(binding);
            break;
        }
        case TriggerKind::DeviceState:
        {
            DeviceStateIndexKey key{trigger.sourceDeviceId, trigger.query};
            index->deviceState[key].push_back(binding);
            break;
        }
        case TriggerKind::IrRecv:
        {
            IrRecvIndexKey key{trigger.sourceDeviceId, trigger.commandId};
            index->irRecv[key].push_back(binding);
            break;
        }
        }
    }

    m_index = index;
}

std::string RuleStore::nextRuleId() const
{
    uint64_t max_num = 0;
    for (const auto& rule : m_rules)
    {
        const auto pos = rule.id.find_last_of('_');
        if (pos == std::string::npos)
            continue;
        try
        {
            max_num = std::max(max_num, std::stoull(rule.id.substr(pos + 1)));
        }
        catch (...)
        {
        }
    }
    return "rule_" + std::to_string(max_num + 1);
}

std::vector<RuleView> RuleStore::list() const
{
    std::shared_lock lock(m_mutex);
    std::vector<RuleView> out;
    out.reserve(m_rules.size());
    for (const auto& rule : m_rules)
        out.push_back(toView(rule, m_triggers));
    return out;
}

std::vector<RuleView> RuleStore::listForDevice(const std::string& device_id) const
{
    std::shared_lock lock(m_mutex);
    std::vector<RuleView> out;
    for (const auto& rule : m_rules)
    {
        bool match = rule.action.deviceId == device_id;
        if (!match && rule.triggerJson.is_object() && rule.triggerJson.contains("deviceId"))
            match = rule.triggerJson["deviceId"].get<std::string>() == device_id;
        if (match)
            out.push_back(toView(rule, m_triggers));
    }
    return out;
}

std::optional<RuleView> RuleStore::get(const std::string& rule_id) const
{
    std::shared_lock lock(m_mutex);
    for (const auto& rule : m_rules)
    {
        if (rule.id == rule_id)
            return toView(rule, m_triggers);
    }
    return std::nullopt;
}

size_t RuleStore::activeCount() const
{
    std::shared_lock lock(m_mutex);
    size_t count = 0;
    for (const auto& rule : m_rules)
    {
        if (rule.enabled)
            ++count;
    }
    return count;
}

TriggerIndexSnapshot RuleStore::snapshot() const
{
    std::shared_lock lock(m_mutex);
    return m_index;
}

RuleView RuleStore::toView(const Rule& rule, const std::unordered_map<std::string, Trigger>& triggers)
{
    RuleView view;
    view.rule = rule;
    view.actionDeviceName = rule.action.deviceId;

    if (!rule.triggerId.empty())
    {
        const auto it = triggers.find(rule.triggerId);
        if (it != triggers.end())
            view.triggerDeviceName = it->second.sourceDeviceId;
        else if (rule.triggerJson.is_object() && rule.triggerJson.contains("deviceId"))
            view.triggerDeviceName = rule.triggerJson["deviceId"].get<std::string>();
    }

    return view;
}

bool RuleStore::validatePayload(const json& payload, std::string& out_error)
{
    if (!payload.is_object())
    {
        out_error = "rule payload must be an object";
        return false;
    }
    if (!payload.contains("name") || !payload["name"].is_string() || payload["name"].get<std::string>().empty())
    {
        out_error = "rule name is required";
        return false;
    }
    if (!payload.contains("action") || !payload["action"].is_object())
    {
        out_error = "action is required";
        return false;
    }

    RuleAction action;
    if (!parseRuleAction(payload["action"], action, out_error))
        return false;

    const bool has_trigger = payload.contains("trigger") && !payload["trigger"].is_null();
    const bool has_schedule = payload.contains("schedule") && !payload["schedule"].is_null();
    if (!has_trigger && !has_schedule)
    {
        out_error = "trigger or schedule is required";
        return false;
    }

    if (has_trigger)
    {
        Trigger trigger;
        if (!parseTriggerFromJson(payload["trigger"], trigger, out_error))
            return false;
    }

    if (has_schedule)
    {
        RuleSchedule schedule;
        if (!parseRuleScheduleFromJson(payload["schedule"], schedule, out_error))
            return false;
    }

    return true;
}

bool RuleStore::applyCreate(const json& payload, RuleView& out_view, std::string& out_error)
{
    if (!validatePayload(payload, out_error))
        return false;

    Rule rule;
    rule.id = payload.contains("id") && payload["id"].is_string()
        ? payload["id"].get<std::string>()
        : nextRuleId();
    rule.name = payload["name"].get<std::string>();
    rule.enabled = payload.value("enabled", true);
    rule.cooldownMs = payload.value("cooldownMs", 0u);
    rule.repeatIntervalMs = payload.value("repeatIntervalMs", 0u);
    rule.execMode = parseExecMode(payload.value("execMode", std::string("once")));

    if (!parseRuleAction(payload["action"], rule.action, out_error))
        return false;

    if (payload.contains("trigger") && !payload["trigger"].is_null())
    {
        rule.triggerJson = payload["trigger"];
        if (!hydrateTriggersFromRule(rule, m_triggers, out_error))
            return false;
    }

    if (payload.contains("schedule") && !payload["schedule"].is_null())
    {
        RuleSchedule schedule;
        if (!parseRuleScheduleFromJson(payload["schedule"], schedule, out_error))
            return false;
        rule.schedule = schedule;
    }

    if (!insertRuleToDatabase(rule, m_defaultUserId, out_error))
        return false;

    m_rules.push_back(rule);
    rebuildIndex();
    out_view = toView(rule, m_triggers);
    return true;
}

bool RuleStore::applyUpdate(const std::string& rule_id, const json& patch, RuleView& out_view, std::string& out_error)
{
    auto it = std::find_if(m_rules.begin(), m_rules.end(), [&](const Rule& rule)
    {
        return rule.id == rule_id;
    });
    if (it == m_rules.end())
    {
        out_error = "rule not found";
        return false;
    }

    json merged = ruleToJson(*it, m_triggers);
    for (auto& [key, value] : patch.items())
        merged[key] = value;
    merged["id"] = rule_id;

    Rule updated;
    if (!parseRuleFromJson(merged, updated, out_error))
        return false;

    if (!hydrateTriggersFromRule(updated, m_triggers, out_error))
        return false;

    if (!updateRuleInDatabase(updated, m_defaultUserId, out_error))
        return false;

    *it = std::move(updated);
    rebuildIndex();
    out_view = toView(*it, m_triggers);
    return true;
}

bool RuleStore::applyDelete(const std::string& rule_id, std::string& out_error)
{
    const auto it = std::find_if(m_rules.begin(), m_rules.end(), [&](const Rule& rule)
    {
        return rule.id == rule_id;
    });
    if (it == m_rules.end())
    {
        out_error = "rule not found";
        return false;
    }

    if (!deleteRuleFromDatabase(rule_id, out_error))
        return false;

    m_rules.erase(it);
    rebuildIndex();
    return true;
}

bool RuleStore::applySetEnabled(const std::string& rule_id, bool enabled, RuleView& out_view, std::string& out_error)
{
    for (auto& rule : m_rules)
    {
        if (rule.id == rule_id)
        {
            rule.enabled = enabled;
            if (!updateRuleInDatabase(rule, m_defaultUserId, out_error))
                return false;
            rebuildIndex();
            out_view = toView(rule, m_triggers);
            return true;
        }
    }
    out_error = "rule not found";
    return false;
}

std::future<RuleView> RuleStore::createAsync(const json& payload)
{
    return TaskQueue::enqueueAsync([this, payload]()
    {
        RuleView view;
        std::string error;
        ChangedCallback callback;
        {
            std::unique_lock lock(m_mutex);
            if (!applyCreate(payload, view, error))
                throw std::runtime_error(error);
            callback = m_onChanged;
        }
        if (callback)
            callback();
        return view;
    });
}

std::future<RuleView> RuleStore::updateAsync(const std::string& rule_id, const json& patch)
{
    return TaskQueue::enqueueAsync([this, rule_id, patch]()
    {
        RuleView view;
        std::string error;
        ChangedCallback callback;
        {
            std::unique_lock lock(m_mutex);
            if (!applyUpdate(rule_id, patch, view, error))
                throw std::runtime_error(error);
            callback = m_onChanged;
        }
        if (callback)
            callback();
        return view;
    });
}

std::future<bool> RuleStore::deleteAsync(const std::string& rule_id)
{
    return TaskQueue::enqueueAsync([this, rule_id]()
    {
        std::string error;
        ChangedCallback callback;
        bool removed = false;
        {
            std::unique_lock lock(m_mutex);
            removed = applyDelete(rule_id, error);
            if (!removed)
                throw std::runtime_error(error.empty() ? "rule not found" : error);
            callback = m_onChanged;
        }
        if (callback)
            callback();
        return removed;
    });
}

std::future<RuleView> RuleStore::setEnabledAsync(const std::string& rule_id, bool enabled)
{
    return TaskQueue::enqueueAsync([this, rule_id, enabled]()
    {
        RuleView view;
        std::string error;
        ChangedCallback callback;
        {
            std::unique_lock lock(m_mutex);
            if (!applySetEnabled(rule_id, enabled, view, error))
                throw std::runtime_error(error.empty() ? "rule not found" : error);
            callback = m_onChanged;
        }
        if (callback)
            callback();
        return view;
    });
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
