#pragma once

#include "../facade/rules_facade.h"

WAVE_NAMESPACE_BEGIN

class DemoRulesFacade :
    public facade::IRulesFacade
{
public:
    Json::Value list(
        const facade::RuleListFilter& filter,
        const std::string& runtime_id,
        int64_t user_id,
        std::string& code) override;

    Json::Value get(const std::string& rule_id, const std::string& runtime_id, std::string& code) override;

    Json::Value create(
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) override;

    Json::Value update(
        const std::string& rule_id,
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) override;

    Json::Value remove(const std::string& rule_id, const std::string& runtime_id, std::string& code) override;

    Json::Value setEnabled(
        const std::string& rule_id,
        bool enabled,
        const std::string& runtime_id,
        std::string& code) override;

    Json::Value execute(const std::string& rule_id, const std::string& runtime_id, std::string& code) override;
};

WAVE_NAMESPACE_END
