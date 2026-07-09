#pragma once

#include <optional>
#include <string>

#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

struct RuleListFilter
{
    std::optional<std::string> device_id;
    std::optional<bool> enabled;
    std::optional<bool> has_schedule;
    std::optional<bool> has_trigger;
    std::optional<int64_t> room_id;
};

class RulesInternalStore
{
public:
    Json::Value listRules(const RuleListFilter& filter, std::string& code) const;
    Json::Value getRule(const std::string& rule_id, std::string& code) const;
    Json::Value createRule(const Json::Value& body, std::string& code) const;
    Json::Value updateRule(const std::string& rule_id, const Json::Value& body, std::string& code) const;
    Json::Value deleteRule(const std::string& rule_id, std::string& code) const;
    Json::Value setRuleEnabled(const std::string& rule_id, bool enabled, std::string& code) const;
    Json::Value executeRule(const std::string& rule_id, std::string& code) const;

    Json::Value toolSchedule(const Json::Value& body, std::string& code) const;
    Json::Value toolScheduleList(const Json::Value& body, std::string& code) const;
    Json::Value toolScheduleCancel(const Json::Value& body, std::string& code) const;
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
