#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

#include "../http_controller.h"

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
    ADD_METHOD_TO(InsightsController::generateInsights, "/api/v1/insights/generate", drogon::Post);
    METHOD_LIST_END

    void listInsights(const HttpRequestPtr& req, HttpResponseCallback&& callback);
    void getInsight(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t insightId);
    void applyInsight(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t insightId);
    void updateInsight(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t insightId);
    /** 특정 surface/date 인사이트를 에이전트에 즉시 생성 요청하고 저장한다. */
    void generateInsights(const HttpRequestPtr& req, HttpResponseCallback&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
