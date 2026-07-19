#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class GoalsController :
    public drogon::HttpController<GoalsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(GoalsController::createGoal, "/api/v1/goals", drogon::Post);
    ADD_METHOD_TO(GoalsController::listGoals, "/api/v1/goals", drogon::Get);
    ADD_METHOD_TO(GoalsController::updateGoal, "/api/v1/goals/{goalId}", drogon::Patch);
    ADD_METHOD_TO(GoalsController::getCoaching, "/api/v1/goals/{goalId}/coaching", drogon::Get);
    ADD_METHOD_TO(GoalsController::applyRecommendation,
        "/api/v1/goals/{goalId}/recommendations/{recommendationId}/apply", drogon::Post);
    ADD_METHOD_TO(GoalsController::updateRecommendation, "/api/v1/goals/{goalId}/recommendations/{recommendationId}", drogon::Patch);
    METHOD_LIST_END

    void createGoal(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void listGoals(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void updateGoal(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId);
    void getCoaching(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId);

    void applyRecommendation(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId, int64_t recommendationId);
    void updateRecommendation(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId, int64_t recommendationId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
