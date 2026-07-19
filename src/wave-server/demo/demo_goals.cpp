#include "demo_goals.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <sstream>

#include "../core/json.h"
#include "../core/logger.h"
#include "../service/agent_client.h"
#include "demo_session_registry.h"
#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    std::string today_date()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &t);
#else
        localtime_r(&t, &local_tm);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
            local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday);
        return buf;
    }

    std::string now_iso()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &t);
#else
        localtime_r(&t, &local_tm);
#endif
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
            local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
            local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
        return buf;
    }

    int64_t next_numeric_id(const Json::Value& items)
    {
        int64_t max_id = 0;
        if (items.isArray())
        {
            for (const auto& item : items)
            {
                if (item.isObject() && item.isMember("id") && item["id"].isIntegral())
                    max_id = std::max(max_id, item["id"].asInt64());
            }
        }
        return max_id + 1;
    }

    int64_t next_recommendation_id(const DemoSessionData& session)
    {
        int64_t max_id = 0;
        if (!session.goal_coachings.isObject())
            return 1;
        for (const auto& key : session.goal_coachings.getMemberNames())
        {
            const auto& coaching = session.goal_coachings[key];
            if (!coaching.isObject() || !coaching.isMember("recommendations"))
                continue;
            for (const auto& item : coaching["recommendations"])
            {
                if (item.isObject() && item.isMember("id") && item["id"].isIntegral())
                    max_id = std::max(max_id, item["id"].asInt64());
            }
        }
        return max_id + 1;
    }

    bool valid_category(const std::string& category)
    {
        static const char* kCategories[] = {"sleep", "posture", "mental", "life", "diet"};
        for (const char* c : kCategories)
        {
            if (category == c)
                return true;
        }
        return false;
    }

    Json::Value json_cpp_from_nlohmann(const json& value)
    {
        Json::CharReaderBuilder reader;
        Json::Value out;
        std::string errors;
        const auto text = value.dump();
        std::istringstream stream(text);
        if (!Json::parseFromStream(reader, stream, &out, &errors))
            return Json::nullValue;
        return out;
    }

    Json::Value coaching_from_agent(
        DemoSessionData& session,
        const Json::Value& goal,
        const json& content)
    {
        const int64_t goal_id = goal["id"].asInt64();
        int64_t rec_id = next_recommendation_id(session);

        Json::Value coaching;
        coaching["periodStart"] = content.value("periodStart", today_date());
        coaching["pastSummary"] = content.value("pastSummary", std::string());
        coaching["projection"] = content.value("projection", std::string());
        coaching["projectedMetrics"] = json_cpp_from_nlohmann(content.value("projectedMetrics", json::object()));
        coaching["source"] = "agent";

        Json::Value recommendations(Json::arrayValue);
        const json items = content.value("items", json::array());
        for (const auto& item : items)
        {
            Json::Value rec;
            rec["id"] = static_cast<Json::Int64>(rec_id++);
            rec["goalId"] = static_cast<Json::Int64>(goal_id);
            rec["kind"] = item.value("kind", std::string("tip"));
            rec["title"] = item.value("title", std::string());
            rec["text"] = item.value("text", std::string());
            rec["actionable"] = item.value("actionable", false);
            if (item.contains("actionType") && item["actionType"].is_string())
                rec["actionType"] = item["actionType"].get<std::string>();
            else
                rec["actionType"] = Json::nullValue;
            rec["approved"] = false;
            if (item.contains("ruleJson") && !item["ruleJson"].is_null())
                rec["ruleJson"] = json_cpp_from_nlohmann(item["ruleJson"]);
            else
                rec["ruleJson"] = Json::nullValue;
            if (item.contains("scheduleTaskJson") && !item["scheduleTaskJson"].is_null())
                rec["scheduleTaskJson"] = json_cpp_from_nlohmann(item["scheduleTaskJson"]);
            else
                rec["scheduleTaskJson"] = Json::nullValue;
            recommendations.append(rec);
        }
        coaching["recommendations"] = recommendations;
        return coaching;
    }

    Json::Value* find_goal(DemoSessionData& session, int64_t user_id, int64_t goal_id)
    {
        for (Json::ArrayIndex i = 0; i < session.goals.size(); ++i)
        {
            auto& item = session.goals[i];
            if (!item.isObject())
                continue;
            if (item.get("id", Json::Int64(0)).asInt64() != goal_id)
                continue;
            if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
                continue;
            return &item;
        }
        return nullptr;
    }

    Json::Value* find_recommendation(Json::Value& coaching, int64_t recommendation_id)
    {
        if (!coaching.isObject() || !coaching.isMember("recommendations"))
            return nullptr;
        auto& items = coaching["recommendations"];
        for (Json::ArrayIndex i = 0; i < items.size(); ++i)
        {
            auto& item = items[i];
            if (item.isObject() && item.get("id", Json::Int64(0)).asInt64() == recommendation_id)
                return &item;
        }
        return nullptr;
    }

    std::string coaching_key(int64_t goal_id)
    {
        return std::to_string(goal_id);
    }
}

Json::Value demoListGoals(
    const std::string& runtime_id,
    const int64_t user_id,
    const std::optional<std::string>& status)
{
    auto locked = demoSessionRegistry().lockSession(runtime_id);
    Json::Value items(Json::arrayValue);
    for (const auto& item : locked->goals)
    {
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        if (status && item.get("status", "").asString() != *status)
            continue;
        Json::Value view = item;
        view.removeMember("userId");
        items.append(view);
    }
    return items;
}

std::optional<Json::Value> demoGetGoal(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t goal_id)
{
    auto locked = demoSessionRegistry().lockSession(runtime_id);
    const auto* goal = find_goal(*locked, user_id, goal_id);
    if (!goal)
        return std::nullopt;
    Json::Value view = *goal;
    view.removeMember("userId");
    return view;
}

std::optional<Json::Value> demoCreateGoal(
    const std::string& runtime_id,
    const int64_t user_id,
    const std::string& title,
    const std::string& category,
    std::string& error,
    std::string& field)
{
    if (title.empty())
    {
        error = "title 이 필요합니다.";
        field = "title";
        return std::nullopt;
    }
    if (!valid_category(category))
    {
        error = "category 가 올바르지 않습니다.";
        field = "category";
        return std::nullopt;
    }

    auto locked = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked;
    const std::string now = now_iso();
    for (Json::ArrayIndex i = 0; i < session.goals.size(); ++i)
    {
        auto& item = session.goals[i];
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        if (item.get("status", "").asString() == "active")
        {
            item["status"] = "archived";
            item["updatedAt"] = now;
        }
    }

    Json::Value goal;
    goal["id"] = static_cast<Json::Int64>(next_numeric_id(session.goals));
    goal["userId"] = static_cast<Json::Int64>(user_id);
    goal["title"] = title;
    goal["category"] = category;
    goal["status"] = "active";
    goal["createdAt"] = now;
    goal["updatedAt"] = now;
    session.goals.append(goal);

    // Coaching is generated on first GET /coaching (agent first, then fallback).
    if (!session.goal_coachings.isObject())
        session.goal_coachings = Json::Value(Json::objectValue);

    Json::Value view = goal;
    view.removeMember("userId");
    return view;
}

std::optional<Json::Value> demoUpdateGoalStatus(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t goal_id,
    const std::string& status,
    std::string& error)
{
    if (status != "active" && status != "archived" && status != "completed")
    {
        error = "status 가 올바르지 않습니다.";
        return std::nullopt;
    }

    auto locked = demoSessionRegistry().lockSession(runtime_id);
    auto* goal = find_goal(*locked, user_id, goal_id);
    if (!goal)
    {
        error = "목표를 찾을 수 없습니다.";
        return std::nullopt;
    }

    (*goal)["status"] = status;
    (*goal)["updatedAt"] = now_iso();
    Json::Value view = *goal;
    view.removeMember("userId");
    return view;
}

std::optional<Json::Value> demoGetGoalCoaching(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t goal_id,
    const std::string& agent_base_url,
    std::string& error)
{
    Json::Value goal_copy;
    {
        auto locked = demoSessionRegistry().lockSession(runtime_id);
        auto& session = *locked;
        const auto* goal = find_goal(session, user_id, goal_id);
        if (!goal)
        {
            error = "목표를 찾을 수 없습니다.";
            return std::nullopt;
        }
        goal_copy = *goal;

        if (!session.goal_coachings.isObject())
            session.goal_coachings = Json::Value(Json::objectValue);

        const auto key = coaching_key(goal_id);
        if (session.goal_coachings.isMember(key) && session.goal_coachings[key].isObject())
            return session.goal_coachings[key];
    }

    if (agent_base_url.empty())
    {
        error = "AGENT_UNAVAILABLE: 목표 코칭 에이전트가 구성되지 않았습니다.";
        return std::nullopt;
    }

    json body;
    body["userId"] = user_id;
    body["goalId"] = goal_id;
    body["goalTitle"] = goal_copy["title"].asString();
    body["category"] = goal_copy["category"].asString();
    body["periodStart"] = today_date();
    body["embed"] = false;

    service::AgentGoalCoachingJobResult agent_result;
    std::string agent_error;
    if (service::runGoalCoachingJobSync(agent_base_url, body, agent_result, agent_error)
        == service::AgentClientResult::success)
    {
        auto locked = demoSessionRegistry().lockSession(runtime_id);
        const Json::Value coaching = coaching_from_agent(*locked, goal_copy, agent_result.content);
        locked->goal_coachings[coaching_key(goal_id)] = coaching;
        WLOG_INFO("demo goal coaching from agent (goal {})", goal_id);
        return coaching;
    }

    // 의미 없는 목표·생성 실패 시 카테고리 프리셋 fallback 을 쓰지 않는다.
    // (fallback 은 실제 코칭처럼 보여 사용자를 오해시킨다.)
    WLOG_WARN("demo goal coaching agent unavailable (goal {}): {}", goal_id, agent_error);
    error = agent_error.empty() ? "AGENT_UNAVAILABLE: 목표 코칭 생성에 실패했습니다." : agent_error;
    return std::nullopt;
}

std::optional<Json::Value> demoApplyGoalRecommendation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t goal_id,
    const int64_t recommendation_id,
    std::string& error,
    std::string& field)
{
    std::string category;
    std::string action_type;
    Json::Value schedule_body;
    Json::Value rule_body;
    {
        auto locked = demoSessionRegistry().lockSession(runtime_id);
        auto& session = *locked;
        const auto* goal = find_goal(session, user_id, goal_id);
        if (!goal)
        {
            error = "목표를 찾을 수 없습니다.";
            return std::nullopt;
        }
        category = (*goal)["category"].asString();

        const auto key = coaching_key(goal_id);
        if (!session.goal_coachings.isObject() || !session.goal_coachings.isMember(key))
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }

        auto* recommendation = find_recommendation(session.goal_coachings[key], recommendation_id);
        if (!recommendation)
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }
        if (!(*recommendation)["actionable"].asBool())
        {
            error = "적용 가능한 추천이 아닙니다.";
            return std::nullopt;
        }
        if ((*recommendation)["approved"].asBool())
        {
            error = "이미 적용된 추천입니다.";
            return std::nullopt;
        }

        action_type = (*recommendation).get("actionType", "").asString();
        if (action_type == "schedule_task")
        {
            schedule_body = (*recommendation)["scheduleTaskJson"];
            if (!schedule_body.isObject())
            {
                error = "scheduleTaskJson 이 비어 있습니다.";
                return std::nullopt;
            }
        }
        else if (action_type == "automation_rule")
        {
            rule_body = (*recommendation)["ruleJson"];
            if (!rule_body.isObject())
            {
                error = "ruleJson 이 비어 있습니다.";
                return std::nullopt;
            }
        }
        else
        {
            error = "지원하지 않는 actionType 입니다.";
            return std::nullopt;
        }
    }

    Json::Value response;
    response["id"] = static_cast<Json::Int64>(recommendation_id);
    response["ruleJson"] = Json::nullValue;
    response["approved"] = true;

    if (action_type == "schedule_task")
    {
        schedule_body["userId"] = static_cast<Json::Int64>(user_id);
        schedule_body["createdBy"] = "agent";
        if (!schedule_body.isMember("category"))
            schedule_body["category"] = category;

        const auto created = demoCreateScheduleTask(runtime_id, schedule_body, error, field);
        if (created.isNull() || !created.isObject())
            return std::nullopt;

        auto locked = demoSessionRegistry().lockSession(runtime_id);
        auto* recommendation =
            find_recommendation(locked->goal_coachings[coaching_key(goal_id)], recommendation_id);
        if (!recommendation)
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }

        Json::Value updated_schedule = (*recommendation)["scheduleTaskJson"];
        updated_schedule["id"] = created["id"];
        (*recommendation)["scheduleTaskJson"] = updated_schedule;
        (*recommendation)["approved"] = true;

        response["derivedScheduleTaskId"] = created["id"];
        response["scheduleTaskJson"] = updated_schedule;
        return response;
    }

    std::string code;
    const auto created_rule = demoCreateRule(runtime_id, rule_body, code);
    if (created_rule.isNull() || !created_rule.isObject())
    {
        error = code.empty() ? "룰 생성에 실패했습니다." : code;
        return std::nullopt;
    }

    auto locked = demoSessionRegistry().lockSession(runtime_id);
    auto* recommendation =
        find_recommendation(locked->goal_coachings[coaching_key(goal_id)], recommendation_id);
    if (!recommendation)
    {
        error = "추천을 찾을 수 없습니다.";
        return std::nullopt;
    }

    Json::Value updated_rule = rule_body;
    updated_rule["id"] = created_rule["id"];
    (*recommendation)["ruleJson"] = updated_rule;
    (*recommendation)["approved"] = true;

    response["ruleJson"] = updated_rule;
    response["derivedRuleId"] = created_rule["id"];
    return response;
}

std::optional<Json::Value> demoUpdateGoalRecommendation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t goal_id,
    const int64_t recommendation_id,
    const bool approved,
    std::string& error)
{
    int64_t schedule_task_id = 0;
    std::string rule_id;
    std::string action_type;
    bool currently_approved = false;

    {
        auto locked = demoSessionRegistry().lockSession(runtime_id);
        if (!find_goal(*locked, user_id, goal_id))
        {
            error = "목표를 찾을 수 없습니다.";
            return std::nullopt;
        }

        const auto key = coaching_key(goal_id);
        if (!locked->goal_coachings.isObject() || !locked->goal_coachings.isMember(key))
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }

        auto* recommendation = find_recommendation(locked->goal_coachings[key], recommendation_id);
        if (!recommendation)
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }

        currently_approved = (*recommendation)["approved"].asBool();
        action_type = (*recommendation).get("actionType", "").asString();
        if (!approved && currently_approved)
        {
            if (action_type == "schedule_task")
            {
                const auto& schedule = (*recommendation)["scheduleTaskJson"];
                if (schedule.isObject() && schedule.isMember("id"))
                    schedule_task_id = schedule["id"].asInt64();
            }
            else if (action_type == "automation_rule")
            {
                const auto& rule = (*recommendation)["ruleJson"];
                if (rule.isObject() && rule.isMember("id") && rule["id"].isString())
                    rule_id = rule["id"].asString();
            }
        }
        else if (approved && !currently_approved)
        {
            (*recommendation)["approved"] = true;
            return *recommendation;
        }
        else
        {
            return *recommendation;
        }
    }

    if (!approved && currently_approved)
    {
        if (schedule_task_id > 0)
            demoDeleteScheduleTask(runtime_id, schedule_task_id);
        if (!rule_id.empty())
            demoDeleteRule(runtime_id, rule_id);

        auto locked = demoSessionRegistry().lockSession(runtime_id);
        auto* recommendation =
            find_recommendation(locked->goal_coachings[coaching_key(goal_id)], recommendation_id);
        if (!recommendation)
        {
            error = "추천을 찾을 수 없습니다.";
            return std::nullopt;
        }
        (*recommendation)["approved"] = false;
        return *recommendation;
    }

    error = "추천을 찾을 수 없습니다.";
    return std::nullopt;
}

WAVE_NAMESPACE_END
