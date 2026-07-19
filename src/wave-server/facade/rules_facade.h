#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "../core/coredefs.h"
#include "../web/http/internal/rules_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

using RuleListFilter = web::internal::RuleListFilter;

class IRulesFacade
{
public:
    virtual ~IRulesFacade() = default;

    virtual Json::Value list(
        const RuleListFilter& filter,
        const std::string& runtime_id,
        int64_t user_id,
        std::string& code) = 0;

    virtual Json::Value get(const std::string& rule_id, const std::string& runtime_id, std::string& code) = 0;

    virtual Json::Value create(
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) = 0;

    virtual Json::Value update(
        const std::string& rule_id,
        const Json::Value& body,
        const std::string& runtime_id,
        std::string& code) = 0;

    virtual Json::Value remove(const std::string& rule_id, const std::string& runtime_id, std::string& code) = 0;

    virtual Json::Value setEnabled(
        const std::string& rule_id,
        bool enabled,
        const std::string& runtime_id,
        std::string& code) = 0;

    virtual Json::Value execute(const std::string& rule_id, const std::string& runtime_id, std::string& code) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
