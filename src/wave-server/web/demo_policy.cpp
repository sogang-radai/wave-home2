#include "demo_policy.h"

#include <string_view>

#include <drogon/drogon.h>

#include "../app/app_state.h"
#include "../demo/demo_device_backend.h"
#include "http/v1/session_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN

namespace
{
    constexpr const char* kDemoWriteBlockedMessage =
        "시연 모드입니다. 여러 분이 함께 사용하는 환경이므로 기기 조작, 알림·일정 설정 등 "
        "변경 작업은 지원하지 않습니다. 수면·전력 등 현재 데이터 조회와 질문 답변만 가능합니다.";

    bool isReadMethod(drogon::HttpMethod method)
    {
        return method == drogon::Get || method == drogon::Head || method == drogon::Options;
    }

    bool isApiPath(std::string_view path)
    {
        return path.starts_with("/api/v1/") || path.starts_with("/internal/v1/");
    }

    bool isDemoReadPost(std::string_view path)
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

    bool isDemoVirtualWritePath(std::string_view path, drogon::HttpMethod method)
    {
        if (!demoVirtualDevicesEnabled())
            return false;

        if (isReadMethod(method))
            return true;

        if (path.starts_with("/api/v1/chat"))
            return true;

        if (isDemoReadPost(path))
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

    bool isDemoMutationAllowed(std::string_view path, drogon::HttpMethod method)
    {
        if (isReadMethod(method))
            return true;

        if (path.starts_with("/api/v1/chat"))
            return true;

        if (isDemoReadPost(path))
            return true;

        if (isDemoVirtualWritePath(path, method))
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

        if (!isApiPath(req->path()))
            return nullptr;

        if (isDemoMutationAllowed(req->path(), req->method()))
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
