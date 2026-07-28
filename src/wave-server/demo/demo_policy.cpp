#include "demo_policy.h"

#include <string_view>

#include <drogon/drogon.h>

#include "../app/app_state.h"
#include "demo_device_backend.h"
#include "../web/http/v1/session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

namespace
{
    constexpr const char* kDemoWriteBlockedMessage =
        "시연 모드입니다. 여러 분이 함께 사용하는 환경이므로 기기 조작, 알림·일정 설정 등 "
        "변경 작업은 지원하지 않습니다. 수면·전력 등 현재 데이터 조회와 질문 답변만 가능합니다.";

    bool is_read_method(drogon::HttpMethod method)
    {
        return method == drogon::Get || method == drogon::Head || method == drogon::Options;
    }

    bool is_api_path(std::string_view path)
    {
        return path.starts_with("/api/v1/") || path.starts_with("/internal/v1/");
    }

    bool is_demo_read_post(std::string_view path)
    {
        static constexpr std::string_view kReadPosts[] = {
            "/internal/v1/db/query",
            "/internal/v1/rag/search",
            "/internal/v1/tools/device.list",
            "/internal/v1/tools/device.query",
            "/internal/v1/tools/device.schedule.list",
        };

        for (const auto& exact : kReadPosts)
        {
            if (path == exact)
                return true;
        }

        if (path.starts_with("/internal/v1/devices/") && path.find("/query/") != std::string_view::npos)
            return true;

        return false;
    }

    bool is_demo_virtual_write_path(std::string_view path, drogon::HttpMethod method)
    {
        if (!demoVirtualDevicesEnabled())
            return false;

        if (is_read_method(method))
            return true;

        if (path.starts_with("/api/v1/chat"))
            return true;

        if (is_demo_read_post(path))
            return true;

        if (path == "/internal/v1/tools/device.control")
            return true;

        if (path.starts_with("/internal/v1/devices/") &&
            (path.find("/actions/") != std::string_view::npos ||
             path.find("/query/") != std::string_view::npos))
            return true;

        if (path.starts_with("/api/v1/iot/devices/") &&
            (path.find("/actions/") != std::string_view::npos ||
             path.find("/query/") != std::string_view::npos ||
             path.ends_with("/gesture-set")))
            return true;

        if (path.starts_with("/api/v1/alarms"))
            return true;
        if (path.starts_with("/api/v1/schedule-tasks"))
            return true;
        if (path.starts_with("/api/v1/goals"))
            return true;
        if (path == "/api/v1/settings/ai-agent")
            return true;
        if (path.starts_with("/internal/v1/alarms"))
            return true;
        if (path.starts_with("/internal/v1/schedule-tasks"))
            return true;
        if (path.starts_with("/internal/v1/rules"))
            return true;

        return false;
    }

    bool is_demo_mutation_allowed(std::string_view path, drogon::HttpMethod method)
    {
        if (is_read_method(method))
            return true;

        if (path.starts_with("/api/v1/chat"))
            return true;

        if (is_demo_read_post(path))
            return true;

        if (is_demo_virtual_write_path(path, method))
            return true;

        // Loopback-only debug/admin endpoint (agent-api listener, not exposed to
        // demo visitors on the public client-api port) that forces the nightly
        // UserModelManager rollover for testing — this guard exists to protect
        // shared multi-user demo state from end-user actions, which doesn't apply
        // to an operator-triggered internal recompute.
        if (path == "/internal/v1/user-model/rollover")
            return true;

        // Same rationale as the rollover endpoint above — loopback-only debug/admin
        // path that forces a weekly power report to generate for testing.
        if (path == "/internal/v1/power/reports/weekly")
            return true;

        // Insight regeneration is admin/debug-triggered only — the frontend never
        // calls this (generation normally happens server-side via AgentJobQueue),
        // so allowing it doesn't expose any new capability through the demo UI
        // visitors actually use, just an operator/testing path for forcing a
        // regenerate against the shared demo dataset.
        if (path == "/api/v1/insights/generate")
            return true;

        // The insight card's "실행" button (SleepPage.js/PosturePage.js/
        // WeeklyPlanPage.js) PATCHes the per-user `approved` bookkeeping flag on
        // an existing insight row.
        if (path.starts_with("/api/v1/insights/") && method == drogon::Patch)
            return true;

        // "실행" on a weekly_plan recommendation additionally calls
        // POST .../apply, which does create a real schedule_task or
        // automation_rule (InsightsController::applyInsight) — a deliberate,
        // narrower exception to this guard's usual "no device/schedule
        // mutations in demo" rule, needed so applying an AI-recommended action
        // actually shows up in the routine planner calendar / rule settings
        // instead of only being simulated client-side.
        if (path.starts_with("/api/v1/insights/") && path.ends_with("/apply") && method == drogon::Post)
            return true;

        return false;
    }
}

void registerDemoPolicy()
{
    drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr
    {
        if (!AppState::get().demo_mode)
            return nullptr;

        if (!is_api_path(req->path()))
            return nullptr;

        if (is_demo_mutation_allowed(req->path(), req->method()))
            return nullptr;

        auto resp = drogon::HttpResponse::newHttpJsonResponse(v1::makeError(
            "DEMO_READ_ONLY",
            kDemoWriteBlockedMessage,
            403,
            ""));
        resp->setStatusCode(drogon::k403Forbidden);
        return resp;
    });
}

WEB_NAMESPACE_END
WAVE_NAMESPACE_END
