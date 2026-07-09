#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class RulesController :
    public drogon::HttpController<RulesController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(RulesController::listRules, "/api/v1/iot/rules", drogon::Get);
    ADD_METHOD_TO(RulesController::createRule, "/api/v1/iot/rules", drogon::Post);
    ADD_METHOD_TO(RulesController::updateRule, "/api/v1/iot/rules/{ruleId}", drogon::Put);
    ADD_METHOD_TO(RulesController::deleteRule, "/api/v1/iot/rules/{ruleId}", drogon::Delete);
    ADD_METHOD_TO(RulesController::setRuleEnabled, "/api/v1/iot/rules/{ruleId}/enabled", drogon::Put);
    ADD_METHOD_TO(RulesController::executeRule, "/api/v1/iot/rules/{ruleId}/execute", drogon::Post);
    METHOD_LIST_END

    void listRules(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void deleteRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void setRuleEnabled(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);

    void executeRule(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        std::string ruleId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
