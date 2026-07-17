#include "real_rules_facade.h"

#include "../web/http/internal/rules_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

Json::Value RealRulesFacade::list(
    const RuleListFilter& filter,
    const std::string& /*runtime_id*/,
    int64_t /*user_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().listRules(filter, code);
}

Json::Value RealRulesFacade::get(
    const std::string& rule_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().getRule(rule_id, code);
}

Json::Value RealRulesFacade::create(
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().createRule(body, code);
}

Json::Value RealRulesFacade::update(
    const std::string& rule_id,
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().updateRule(rule_id, body, code);
}

Json::Value RealRulesFacade::remove(
    const std::string& rule_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().deleteRule(rule_id, code);
}

Json::Value RealRulesFacade::setEnabled(
    const std::string& rule_id,
    bool enabled,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().setRuleEnabled(rule_id, enabled, code);
}

Json::Value RealRulesFacade::execute(
    const std::string& rule_id,
    const std::string& /*runtime_id*/,
    std::string& code)
{
    return web::internal::RulesInternalStore().executeRule(rule_id, code);
}

} // namespace facade
WAVE_NAMESPACE_END
