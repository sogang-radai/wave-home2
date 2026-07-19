#include "goals_controller.h"
#include "../../../db/database.h"

#include <sstream>

#include "../../../app/app_state.h"
#include "../../../core/json.h"
#include "../../../demo/demo_device_backend.h"
#include "../../../demo/demo_goals.h"
#include "../../../demo/demo_runtime_id.h"
#include "../../../service/goal_coaching_generator.h"
#include "../../../service/rule_store.h"
#include "../internal/schedule_tasks_internal_store.h"
#include "action_log_store.h"
#include "goals_store.h"
#include "insights_store.h"
#include "session_store.h"
#include "settings_store.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

namespace
{
    std::optional<int64_t> resolve_user_id(const HttpRequestPtr& req, db::DbClientPtr client)
    {
        SessionStore sessions(client);
        SettingsStore settings(client);
        return settings.resolveActiveUserId(sessions, req);
    }

    ws::json json_from_request(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        std::istringstream stream(Json::writeString(builder, value));
        return ws::json::parse(stream);
    }

    drogon::HttpResponsePtr json_response(
        const HttpRequestPtr& req,
        const Json::Value& body,
        const std::string& runtime_id,
        drogon::HttpStatusCode status = drogon::k200OK)
    {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
        resp->setStatusCode(status);
        if (!runtime_id.empty())
            attachDemoRuntimeCookieIfNeeded(req, resp, runtime_id);
        return resp;
    }

    bool starts_with(const std::string& raw, const char* prefix)
    {
        const size_t n = std::char_traits<char>::length(prefix);
        return raw.size() >= n && raw.compare(0, n, prefix) == 0;
    }

    /**
     * agent_client 오류는 두 형태다.
     * 1) 알려진 코드 접두: "INVALID_GOAL: …", "GENERATION_FAILED: …"
     * 2) 자유 텍스트: "Failed to connect to agent at 127.0.0.1:8512"
     *    → 콜론으로 무조건 자르면 포트(8512)만 message 로 남는다.
     */
    void split_agent_error(const std::string& raw, std::string& code, std::string& message)
    {
        static const char* kKnownCodes[] = {
            "INVALID_GOAL",
            "GENERATION_FAILED",
            "AGENT_UNAVAILABLE",
            "NOT_FOUND",
            "JOB_ALREADY_RUNNING",
        };
        for (const char* known : kKnownCodes)
        {
            const std::string prefix = std::string(known) + ":";
            if (!starts_with(raw, prefix.c_str()))
                continue;
            code = known;
            message = raw.substr(prefix.size());
            while (!message.empty() && (message[0] == ' ' || message[0] == '\t'))
                message.erase(message.begin());
            if (message.empty())
                message = "목표 코칭 생성에 실패했습니다.";
            return;
        }

        if (raw.find("Failed to connect to agent") != std::string::npos
            || raw.find("DNS resolve failed") != std::string::npos
            || raw.find("Agent HTTP error") != std::string::npos)
        {
            code = "AGENT_UNAVAILABLE";
            message = "목표 코칭 에이전트에 연결하지 못했어요. 데모 에이전트(8512)가 실행 중인지 확인해 주세요.";
            return;
        }

        code = "AGENT_UNAVAILABLE";
        message = raw.empty() ? "목표 코칭 생성에 실패했습니다." : raw;
    }

    void respond_coaching_error(const HttpResponseCallback& callback, const std::string& raw_error)
    {
        std::string code;
        std::string message;
        split_agent_error(raw_error, code, message);
        if (code == "INVALID_GOAL")
        {
            respondError(callback, 422, "INVALID_GOAL", message);
            return;
        }
        if (code == "NOT_FOUND" || raw_error.find("찾을") != std::string::npos)
        {
            respondError(callback, 404, "NOT_FOUND", message.empty() ? raw_error : message);
            return;
        }
        respondError(callback, 502, code.empty() ? "AGENT_UNAVAILABLE" : code, message);
    }
}

void GoalsController::createGoal(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject() || !json->isMember("title") || !(*json)["title"].isString()
        || !json->isMember("category") || !(*json)["category"].isString())
    {
        respondError(callback, 400, "INVALID_BODY", "title, category 가 필요합니다.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, json.get());
        std::string error;
        std::string field;
        const auto created = demoCreateGoal(
            runtime_id,
            *user_id,
            (*json)["title"].asString(),
            (*json)["category"].asString(),
            error,
            field);
        if (!created)
        {
            respondError(callback, 400, "INVALID_REQUEST", error, field);
            return;
        }
        callback(json_response(req, *created, runtime_id, drogon::k201Created));
        return;
    }

    GoalsStore store(client);
    // 활성 목표는 사용자당 1개 — 새로 설정하면 기존 활성 목표는 확인 없이 archive.
    store.archiveActiveGoals(*user_id);

    std::string error;
    std::string field;
    const auto created = store.create(*user_id, (*json)["title"].asString(), (*json)["category"].asString(), error, field);
    if (!created)
    {
        respondError(callback, 400, "INVALID_REQUEST", error, field);
        return;
    }

    auto resp = drogon::HttpResponse::newHttpJsonResponse(*created);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void GoalsController::listGoals(const HttpRequestPtr& req, HttpResponseCallback&& callback)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto status = req->getParameter("status");
    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        callback(json_response(
            req,
            demoListGoals(runtime_id, *user_id, status.empty() ? std::nullopt : std::optional<std::string>(status)),
            runtime_id));
        return;
    }

    GoalsStore store(client);
    callback(drogon::HttpResponse::newHttpJsonResponse(
        store.list(*user_id, status.empty() ? std::nullopt : std::optional<std::string>(status))));
}

void GoalsController::updateGoal(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    const auto json = req->getJsonObject();
    if (!json || !json->isObject() || !json->isMember("status") || !(*json)["status"].isString())
    {
        respondError(callback, 400, "INVALID_BODY", "status 가 필요합니다.", "status");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, json.get());
        std::string error;
        const auto updated =
            demoUpdateGoalStatus(runtime_id, *user_id, goalId, (*json)["status"].asString(), error);
        if (!updated)
        {
            const int status_code = error.find("찾을") != std::string::npos ? 404 : 400;
            respondError(callback, status_code, status_code == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error);
            return;
        }
        callback(json_response(req, *updated, runtime_id));
        return;
    }

    std::string error;
    GoalsStore store(client);
    const auto updated = store.updateStatus(*user_id, goalId, (*json)["status"].asString(), error);
    if (!updated)
    {
        const int status_code = error.find("찾을") != std::string::npos ? 404 : 400;
        respondError(callback, status_code, status_code == 404 ? "NOT_FOUND" : "INVALID_REQUEST", error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*updated));
}

void GoalsController::getCoaching(const HttpRequestPtr& req, HttpResponseCallback&& callback, int64_t goalId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        std::string error;
        const auto coaching = demoGetGoalCoaching(
            runtime_id,
            *user_id,
            goalId,
            AppState::get().config.agent.base_url,
            error);
        if (!coaching)
        {
            respond_coaching_error(callback, error);
            return;
        }
        callback(json_response(req, *coaching, runtime_id));
        return;
    }

    GoalsStore store(client);
    const auto goal = store.getById(*user_id, goalId);
    if (goal.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "목표를 찾을 수 없습니다.");
        return;
    }

    const auto date = InsightsStore::reference_date(client);

    if (const auto cached = service::readCachedGoalCoaching(client, goalId, date))
    {
        callback(drogon::HttpResponse::newHttpJsonResponse(*cached));
        return;
    }

    std::string error;
    const auto generated = service::generateGoalCoaching(
        client,
        AppState::get().config.agent.base_url,
        *user_id,
        goalId,
        goal["title"].asString(),
        goal["category"].asString(),
        date,
        error);
    if (!generated)
    {
        respond_coaching_error(callback, error);
        return;
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(*generated));
}

void GoalsController::applyRecommendation(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    int64_t goalId,
    int64_t recommendationId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
    if (!user_id)
    {
        respondError(callback, 409, "ACTIVE_ACCOUNT_REQUIRED", "활성 구성원을 먼저 선택해주세요.");
        return;
    }

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, nullptr);
        std::string error;
        std::string field;
        const auto applied =
            demoApplyGoalRecommendation(runtime_id, *user_id, goalId, recommendationId, error, field);
        if (!applied)
        {
            int status = 400;
            const char* code = "INVALID_REQUEST";
            if (error.find("찾을") != std::string::npos)
            {
                status = 404;
                code = "NOT_FOUND";
            }
            else if (error.find("적용 가능") != std::string::npos)
            {
                status = 409;
                code = "NOT_ACTIONABLE";
            }
            else if (error.find("이미 적용") != std::string::npos)
            {
                status = 409;
                code = "ALREADY_APPLIED";
            }
            respondError(callback, status, code, error, field);
            return;
        }
        callback(json_response(req, *applied, runtime_id));
        return;
    }

    GoalsStore store(client);
    const auto goal = store.getById(*user_id, goalId);
    if (goal.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "목표를 찾을 수 없습니다.");
        return;
    }

    const auto recommendation = store.getRecommendation(*user_id, goalId, recommendationId);
    if (recommendation.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "추천을 찾을 수 없습니다.");
        return;
    }

    if (!recommendation["actionable"].asBool())
    {
        respondError(callback, 409, "NOT_ACTIONABLE", "적용 가능한 추천이 아닙니다.");
        return;
    }
    if (recommendation["approved"].asBool())
    {
        respondError(callback, 409, "ALREADY_APPLIED", "이미 적용된 추천입니다.");
        return;
    }

    const auto action_type = recommendation["actionType"].asString();
    const auto category = goal["category"].asString();
    Json::Value response;
    response["id"] = static_cast<Json::Int64>(recommendationId);
    response["ruleJson"] = Json::nullValue;

    if (action_type == "schedule_task")
    {
        Json::Value body = recommendation["scheduleTaskJson"];
        if (!body.isObject())
        {
            respondError(callback, 400, "INVALID_RECOMMENDATION", "scheduleTaskJson 이 비어 있습니다.");
            return;
        }
        body["userId"] = static_cast<Json::Int64>(*user_id);
        body["createdBy"] = "agent";
        if (!body.isMember("category"))
            body["category"] = category;

        std::string error;
        std::string field;
        const auto created = internal::ScheduleTasksInternalStore(client).create(body, error, field);
        if (!created)
        {
            respondError(callback, 400, "INVALID_REQUEST", error, field);
            return;
        }

        Json::Value updated_schedule_task_json = recommendation["scheduleTaskJson"];
        updated_schedule_task_json["id"] = (*created)["id"];

        if (!store.markRecommendationApplied(recommendationId, std::nullopt, updated_schedule_task_json))
        {
            respondError(callback, 404, "NOT_FOUND", "추천을 찾을 수 없습니다.");
            return;
        }

        ActionLogStore(client).record(*user_id, "schedule_task_created", "schedule_task", (*created)["id"].asInt64(), category);
        response["derivedScheduleTaskId"] = (*created)["id"];
    }
    else if (action_type == "automation_rule")
    {
        auto& state = AppState::get();
        if (!state.hasRuleStore())
        {
            respondError(callback, 503, "AUTOMATION_UNAVAILABLE", "룰 저장소를 사용할 수 없습니다.");
            return;
        }

        Json::Value rule_json = recommendation["ruleJson"];
        if (!rule_json.isObject())
        {
            respondError(callback, 400, "INVALID_RECOMMENDATION", "ruleJson 이 비어 있습니다.");
            return;
        }

        ws::json payload;
        try
        {
            payload = json_from_request(rule_json);
        }
        catch (...)
        {
            respondError(callback, 400, "INVALID_RECOMMENDATION", "ruleJson 형식이 올바르지 않습니다.");
            return;
        }

        std::string validate_error;
        if (!service::RuleStore::validate_payload(payload, validate_error))
        {
            respondError(callback, 400, "INVALID_RECOMMENDATION", validate_error);
            return;
        }

        try
        {
            auto future = state.ruleStore().createAsync(payload);
            const auto view = future.get();

            Json::Value updated_rule_json = rule_json;
            updated_rule_json["id"] = view.rule.id;

            if (!store.markRecommendationApplied(recommendationId, updated_rule_json, std::nullopt))
            {
                respondError(callback, 404, "NOT_FOUND", "추천을 찾을 수 없습니다.");
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
        respondError(callback, 400, "INVALID_RECOMMENDATION", "지원하지 않는 actionType 입니다.");
        return;
    }

    response["approved"] = true;
    callback(drogon::HttpResponse::newHttpJsonResponse(response));
}

void GoalsController::updateRecommendation(const HttpRequestPtr& req, HttpResponseCallback&& callback,
    int64_t goalId,
    int64_t recommendationId)
{
    auto client = AppState::get().db();
    if (!client)
    {
        respondError(callback, 503, "DB_UNAVAILABLE", "데이터베이스를 사용할 수 없습니다.");
        return;
    }

    const auto user_id = resolve_user_id(req, client);
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

    if (demoVirtualDevicesEnabled())
    {
        const auto runtime_id = resolveDemoRuntimeId(req, json.get());
        std::string error;
        const auto updated = demoUpdateGoalRecommendation(
            runtime_id, *user_id, goalId, recommendationId, (*json)["approved"].asBool(), error);
        if (!updated)
        {
            respondError(callback, 404, "NOT_FOUND", error);
            return;
        }
        callback(json_response(req, *updated, runtime_id));
        return;
    }

    GoalsStore store(client);
    const auto recommendation = store.getRecommendation(*user_id, goalId, recommendationId);
    if (recommendation.isNull())
    {
        respondError(callback, 404, "NOT_FOUND", "추천을 찾을 수 없습니다.");
        return;
    }

    const bool wants_approved = (*json)["approved"].asBool();
    const bool currently_approved = recommendation["approved"].asBool();

    if (!wants_approved && currently_approved)
    {
        const auto action_type = recommendation["actionType"].asString();
        if (action_type == "automation_rule")
        {
            const auto& rule_json = recommendation["ruleJson"];
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
            const auto& schedule_task_json = recommendation["scheduleTaskJson"];
            if (schedule_task_json.isObject() && schedule_task_json.isMember("id"))
            {
                std::string error;
                internal::ScheduleTasksInternalStore(client)
                    .remove(*user_id, schedule_task_json["id"].asInt64(), error);
            }
        }

        store.markRecommendationCanceled(recommendationId);
    }
    else if (wants_approved && !currently_approved)
    {
        // 승인 + 파생 리소스 생성은 POST .../apply 로만 수행한다. PATCH 는 플래그만 반영한다.
        store.markRecommendationApplied(recommendationId, std::nullopt, std::nullopt);
    }

    callback(drogon::HttpResponse::newHttpJsonResponse(store.getRecommendation(*user_id, goalId, recommendationId)));
}

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
