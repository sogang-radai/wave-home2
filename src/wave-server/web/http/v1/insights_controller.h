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
    ADD_METHOD_TO(InsightsController::generateInsights, "/api/v1/insights/generate", drogon::Post);
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

    /**
     * 특정 surface/date 인사이트를 에이전트에 즉시 생성 요청하고 저장한다
     * (평소엔 sleep_report 는 sleep_manager 가 리포트 생성 직후 자동 트리거하지만,
     * weekly_plan/dashboard_banner 등 다른 surface 는 아직 자동 트리거가 없어
     * 수동/운영 트리거로도 쓸 수 있게 열어둔다).
     */
    void generateInsights(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
