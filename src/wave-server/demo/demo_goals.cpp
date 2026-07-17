#include "demo_goals.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>

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

    struct CategoryPreset
    {
        const char* past_summary;
        const char* projection;
        double completion_rate;
        const char* trend;
        const char* rec_title;
        const char* rec_text;
        const char* day_of_week;
        const char* tip_title;
        const char* tip_text;
    };

    const CategoryPreset& preset_for(const std::string& category)
    {
        static const CategoryPreset kSleep = {
            "최근 30일간 취침 관련 습관을 약 68% 완료했어요. 최근 며칠은 흐름이 흔들리는 추세예요.",
            "지금 흐름이 이어지면 다음 달 완료율은 조금 더 낮아질 수 있어요. 취침 준비 알림을 다시 켜보는 걸 추천해요.",
            0.68,
            "declining",
            "22:30 취침 준비 알림 추가",
            "취침 30분 전 알림을 추가해 습관을 다시 잡아보세요.",
            "mon",
            "스마트폰은 침실 밖에 두기",
            "자기 전 스마트폰 사용을 줄이면 취침 시간을 지키기 더 쉬워져요.",
        };
        static const CategoryPreset kPosture = {
            "최근 30일간 자세 관련 습관을 약 74% 완료했어요. 꾸준한 편이에요.",
            "이 페이스를 유지하면 다음 달에도 비슷한 완료율을 기대할 수 있어요.",
            0.74,
            "steady",
            "오후 4시 목 스트레칭 알림",
            "오래 앉아있는 시간대에 스트레칭 알림을 추가해보세요.",
            "wed",
            "모니터 높이 맞추기",
            "눈높이에 모니터 상단을 맞추면 목·어깨 부담이 줄어요.",
        };
        static const CategoryPreset kMental = {
            "최근 30일간 멘탈 관리 습관을 약 61% 완료했어요.",
            "조금만 더 신경 쓰면 다음 달 완료율을 눈에 띄게 올릴 수 있어요.",
            0.61,
            "improving",
            "저녁 10분 명상 알림",
            "하루를 마무리하며 짧은 명상 시간을 가져보세요.",
            "tue",
            "취침 전 걱정 노트 작성해보기",
            "자기 전 걱정거리를 적어두면 마음이 한결 가벼워져요.",
        };
        static const CategoryPreset kLife = {
            "최근 30일간 생활 습관 목표를 약 70% 완료했어요.",
            "지금 페이스라면 다음 달에도 무난히 목표를 이어갈 수 있어요.",
            0.70,
            "steady",
            "아침 물 한 잔 마시기 알림",
            "기상 직후 물 한 잔으로 하루를 시작해보세요.",
            "thu",
            "주간 회고 5분",
            "주말에 한 주를 짧게 돌아보면 다음 주 계획이 쉬워져요.",
        };
        static const CategoryPreset kDiet = {
            "최근 30일간 식습관 목표를 약 58% 완료했어요. 최근 개선되는 흐름이에요.",
            "이 흐름을 이어가면 다음 달엔 완료율이 더 올라갈 수 있어요.",
            0.58,
            "improving",
            "점심 채소 위주 식단 알림",
            "점심시간 전 알림으로 식단 선택을 도와드려요.",
            "fri",
            "식사 전 물 한 잔 마시기",
            "식사 전 물을 마시면 과식을 줄이는 데 도움이 돼요.",
        };

        if (category == "sleep")
            return kSleep;
        if (category == "posture")
            return kPosture;
        if (category == "mental")
            return kMental;
        if (category == "diet")
            return kDiet;
        return kLife;
    }

    Json::Value build_coaching(DemoSessionData& session, const Json::Value& goal)
    {
        const auto& preset = preset_for(goal["category"].asString());
        const int64_t goal_id = goal["id"].asInt64();
        int64_t rec_id = next_recommendation_id(session);

        Json::Value action;
        action["id"] = static_cast<Json::Int64>(rec_id++);
        action["goalId"] = static_cast<Json::Int64>(goal_id);
        action["kind"] = "action";
        action["title"] = preset.rec_title;
        action["text"] = preset.rec_text;
        action["actionable"] = true;
        action["actionType"] = "schedule_task";
        action["approved"] = false;
        action["ruleJson"] = Json::nullValue;
        Json::Value schedule;
        schedule["title"] = preset.rec_title;
        schedule["dayOfWeek"] = preset.day_of_week;
        schedule["scheduleKind"] = "weekly";
        schedule["category"] = goal["category"].asString();
        action["scheduleTaskJson"] = schedule;

        Json::Value tip;
        tip["id"] = static_cast<Json::Int64>(rec_id++);
        tip["goalId"] = static_cast<Json::Int64>(goal_id);
        tip["kind"] = "tip";
        tip["title"] = preset.tip_title;
        tip["text"] = preset.tip_text;
        tip["actionable"] = false;
        tip["actionType"] = Json::nullValue;
        tip["approved"] = false;
        tip["ruleJson"] = Json::nullValue;
        tip["scheduleTaskJson"] = Json::nullValue;

        Json::Value metrics;
        metrics["completionRate"] = preset.completion_rate;
        metrics["trend"] = preset.trend;
        metrics["streakDays"] = 2;

        Json::Value coaching;
        coaching["periodStart"] = today_date();
        coaching["pastSummary"] = preset.past_summary;
        coaching["projection"] = preset.projection;
        coaching["projectedMetrics"] = metrics;
        Json::Value recommendations(Json::arrayValue);
        recommendations.append(action);
        recommendations.append(tip);
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

    if (!session.goal_coachings.isObject())
        session.goal_coachings = Json::Value(Json::objectValue);
    session.goal_coachings[coaching_key(goal["id"].asInt64())] = build_coaching(session, goal);

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
    std::string& error)
{
    auto locked = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked;
    if (!find_goal(session, user_id, goal_id))
    {
        error = "목표를 찾을 수 없습니다.";
        return std::nullopt;
    }

    if (!session.goal_coachings.isObject())
        session.goal_coachings = Json::Value(Json::objectValue);

    const auto key = coaching_key(goal_id);
    if (!session.goal_coachings.isMember(key) || !session.goal_coachings[key].isObject())
    {
        error = "목표 코칭을 찾을 수 없습니다.";
        return std::nullopt;
    }
    return session.goal_coachings[key];
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
