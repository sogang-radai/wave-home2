#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class InsightsController :
    public drogon::HttpController<InsightsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(InsightsController::listInsights, "/api/v1/insights", drogon::Get);
    ADD_METHOD_TO(InsightsController::getInsight, "/api/v1/insights/{insightId}", drogon::Get);
    ADD_METHOD_TO(InsightsController::applyInsight, "/api/v1/insights/{insightId}/apply", drogon::Post);
    ADD_METHOD_TO(InsightsController::updateInsight, "/api/v1/insights/{insightId}", drogon::Patch);
    METHOD_LIST_END

    void listInsights(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getInsight(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t insightId);

    void applyInsight(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t insightId);

    void updateInsight(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t insightId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
