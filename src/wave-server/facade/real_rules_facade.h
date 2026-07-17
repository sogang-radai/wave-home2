#pragma once

#include "rules_facade.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class RealRulesFacade :
    public IRulesFacade
{
public:
    Json::Value list(
        const RuleListFilter& filter,
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

} // namespace facade
WAVE_NAMESPACE_END
