#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class SleepController :
    public drogon::HttpController<SleepController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SleepController::todaySummary, "/api/v1/sleep/today/summary", drogon::Get);
    ADD_METHOD_TO(SleepController::todayPlan, "/api/v1/sleep/today/plan", drogon::Get);
    ADD_METHOD_TO(SleepController::todayPhoneUsage, "/api/v1/sleep/today/phone-usage", drogon::Get);
    ADD_METHOD_TO(SleepController::todayAutomationSummary, "/api/v1/sleep/today/automation-summary", drogon::Get);
    ADD_METHOD_TO(SleepController::dailySessions, "/api/v1/sleep/reports/daily/sessions", drogon::Get);
    ADD_METHOD_TO(SleepController::dailyReport, "/api/v1/sleep/reports/daily", drogon::Get);
    ADD_METHOD_TO(SleepController::weeklyReport, "/api/v1/sleep/reports/weekly", drogon::Get);
    METHOD_LIST_END

    void todaySummary(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void todayPlan(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void todayPhoneUsage(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void todayAutomationSummary(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void dailySessions(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void dailyReport(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void weeklyReport(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
