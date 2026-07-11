#include "insights_controller.h"

#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../service/rule_store.h"
#include "../internal/schedule_tasks_internal_store.h"
#include "action_log_store.h"
#include "insights_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::optional<int64_t> resolveUserId(const drogon::HttpRequestPtr& req, drogon::orm::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }

    std::optional<bool> parseOptionalBool(const std::string& value)
    {
        if (value.empty())
            return std::nullopt;
        if (value == "true" || value == "1")
            return true;
        if (value == "false" || value == "0")
            return false;
        return std::nullopt;
    }

    ws::json jsonFromRequest(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::istringstream stream(Json::writeString(builder, value));
        return ws::json::parse(stream);
    }
}

void InsightsController::listInsights(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto surface = req->getParameter("surface");
    const auto date = req->getParameter("date");
    const auto kind = req->getParameter("kind");
    const auto approved = parseOptionalBool(req->getParameter("approved"));
    const auto actionable = parseOptionalBool(req->getParameter("actionable"));

    InsightsStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(store.list(
        *user_id,
        surface.empty() ? std::nullopt : std::optional<std::string>(surface),
        date.empty() ? std::nullopt : std::optional<std::string>(date),
        kind.empty() ? std::nullopt : std::optional<std::string>(kind),
        approved,
        actionable)));
}

void InsightsController::getInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t insightId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    InsightsStore store(client);
    const auto body = store.getById(*user_id, insightId);
    if (body.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

void InsightsController::applyInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t insightId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    InsightsStore store(client);
    const auto insight = store.getById(*user_id, insightId);
    if (insight.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
        return;
    }

    if (!insight["actionable"].asBool())
    {
        respondError(callback, 409, "NOT_ACTIONABLE", "적용 가능한 인사이트가 아닙니다.");
        return;
    }
    if (insight["approved"].asBool())
    {
        respondError(callback, 409, "ALREADY_APPLIED", "이미 적용된 인사이트입니다.");
        return;
    }

    const auto action_type = insight["actionType"].asString();
    Json::Value response;
    response["id"] = static_cast<Json::Int64>(insightId);
    response["ruleJson"] = Json::nullValue;

    if (action_type == "schedule_task")
    {
        Json::Value body = insight["scheduleTaskJson"];
        if (!body.isObject())
        {
            respondError(callback, 400, "INVALID_INSIGHT", "scheduleTaskJson 이 비어 있습니다.");
            return;
        }
        body["userId"] = static_cast<Json::Int64>(*user_id);
        body["createdBy"] = "agent";
        body["sourceInsightId"] = static_cast<Json::Int64>(insightId);

        std::string error;
        std::string field;
        const auto created = internal::ScheduleTasksInternalStore(client).create(body, error, field);
        if (!created)
        {
            respondError(callback, 400, "INVALID_REQUEST", error, field);
            return;
        }

        if (!store.markApplied(*user_id, insightId, std::nullopt))
        {
            respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
            return;
        }

        response["derivedScheduleTaskId"] = (*created)["id"];
    }
    else if (action_type == "automation_rule" || action_type == "reservation")
    {
        auto& state = AppState::get();
        if (!state.hasRuleStore())
        {
            respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "룰 저장소를 사용할 수 없습니다.");
            return;
        }

        Json::Value rule_json = insight["ruleJson"];
        if (!rule_json.isObject())
        {
            respondError(callback, 400, "INVALID_INSIGHT", "ruleJson 이 비어 있습니다.");
            return;
        }

        ws::json payload;
        try
        {
            payload = jsonFromRequest(rule_json);
        }
        catch (...)
        {
            respondError(callback, 400, "INVALID_INSIGHT", "ruleJson 형식이 올바르지 않습니다.");
            return;
        }

        std::string validate_error;
        if (!service::RuleStore::validatePayload(payload, validate_error))
        {
            respondError(callback, 400, "INVALID_INSIGHT", validate_error);
            return;
        }

        try
        {
            auto future = state.ruleStore().createAsync(payload);
            const auto view = future.get();

            Json::Value updated_rule_json = rule_json;
            updated_rule_json["id"] = view.rule.id;

            if (!store.markApplied(*user_id, insightId, updated_rule_json))
            {
                respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
                return;
            }

            response["ruleJson"] = updated_rule_json;
            response["derivedRuleId"] = view.rule.id;
        }
        catch (const std::exception& e)
        {
            respondError(callback, 409, "RULE_ID_CONFLICT", e.what());
            return;
        }
    }
    else
    {
        respondError(callback, 400, "INVALID_INSIGHT", "지원하지 않는 actionType 입니다.");
        return;
    }

    response["approved"] = true;
    ActionLogStore(client).record(*user_id, "insight_applied", "insight", insightId);
    callback(drogon::HttpResponse::newHttpJsonResponse(response));
}

void InsightsController::updateInsight(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    int64_t insightId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolveUserId(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject() || !json->isMember("approved") || !(*json)["approved"].isBool())
    {
        respondError(callback, 400, "INVALID_BODY", "approved 값이 필요합니다.", "approved");
        return;
    }

    InsightsStore store(client);
    const auto insight = store.getById(*user_id, insightId);
    if (insight.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "인사이트를 찾을 수 없습니다.");
        return;
    }

    const bool wants_approved = (*json)["approved"].asBool();
    const bool currently_approved = insight["approved"].asBool();

    if (!wants_approved && currently_approved)
    {
        const auto action_type = insight["actionType"].asString();
        if (action_type == "automation_rule" || action_type == "reservation")
        {
            const auto& rule_json = insight["ruleJson"];
            if (rule_json.isObject() && rule_json.isMember("id") && rule_json["id"].isString())
            {
                auto& state = AppState::get();
                if (state.hasRuleStore())
                {
                    try
                    {
                        state.ruleStore().deleteAsync(rule_json["id"].asString()).get();
                    }
                    catch (const std::exception&)
                    {
                        // 이미 삭제됐거나 존재하지 않으면 무시하고 취소를 계속 진행한다.
                    }
                }
            }
        }
        else if (action_type == "schedule_task")
        {
            internal::ScheduleTasksInternalStore(client).removeBySourceInsight(*user_id, insightId);
        }

        store.markCanceled(*user_id, insightId);
        ActionLogStore(client).record(*user_id, "insight_canceled", "insight", insightId);
    }
    else if (wants_approved && !currently_approved)
    {
        // 승인 + 파생 리소스 생성은 POST .../apply 로만 수행한다. PATCH 는 플래그만 반영한다.
        store.markApplied(*user_id, insightId, std::nullopt);
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(store.getById(*user_id, insightId)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
