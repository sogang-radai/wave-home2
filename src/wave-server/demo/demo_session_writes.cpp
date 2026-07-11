#include "demo_session_writes.h"

#include <algorithm>
#include <chrono>
#include <sstream>

#include "../device/device_wire_id.hpp"
#include "../web/http/v1/chat_store.h"
#include "demo_session_registry.h"

WAVE_NAMESPACE_BEGIN

namespace
{
    std::string nowIso()
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

    std::string nowStamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm {};
#if defined(_WIN32)
        localtime_s(&local_tm, &time);
#else
        localtime_r(&time, &local_tm);
#endif
        char buffer[32];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
        return buffer;
    }

    int64_t nextNumericId(const Json::Value& items)
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

    Json::Value parseJsonText(const std::string& raw, Json::Value fallback = Json::nullValue)
    {
        if (raw.empty())
            return fallback;
        Json::CharReaderBuilder reader;
        Json::Value value;
        std::string errors;
        std::istringstream stream(raw);
        if (!Json::parseFromStream(reader, stream, &value, &errors))
            return fallback;
        return value;
    }

    std::string wireIdForInternalDevice(const drogon::orm::DbClientPtr& client, int64_t internal_id)
    {
        if (!client || internal_id <= 0)
            return {};
        auto rows = client->execSqlSync("SELECT name FROM device WHERE id = ?", internal_id);
        if (rows.empty())
            return {};
        return dev::wireIdForDbRow(internal_id, rows[0]["name"].as<std::string>());
    }

    Json::Value alarmRowToJson(const drogon::orm::DbClientPtr& client, const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["name"] = row["name"].as<std::string>();
        item["timeMinute"] = row["time_minute"].as<int>();
        item["daysOfWeek"] = parseJsonText(row["days_of_week"].as<std::string>(), Json::Value(Json::arrayValue));
        item["smartWake"] = row["smart_wake"].as<int>() != 0;
        if (row["radar_device_id"].isNull())
            item["radarDeviceId"] = Json::nullValue;
        else
        {
            const auto wire = wireIdForInternalDevice(client, row["radar_device_id"].as<int64_t>());
            item["radarDeviceId"] = wire.empty() ? Json::nullValue : Json::Value(wire);
        }
        if (row["device_id"].isNull())
            item["deviceId"] = Json::nullValue;
        else
        {
            const auto wire = wireIdForInternalDevice(client, row["device_id"].as<int64_t>());
            item["deviceId"] = wire.empty() ? Json::nullValue : Json::Value(wire);
        }
        item["method"] = parseJsonText(row["method"].as<std::string>(), Json::Value(Json::objectValue));
        item["enabled"] = row["enabled"].as<int>() != 0;
        item["createdAt"] = row["created_at"].as<std::string>();
        item["updatedAt"] = row["updated_at"].as<std::string>();
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value scheduleRowToJson(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["title"] = row["title"].as<std::string>();
        if (row["created_at"].isNull())
            item["createdAt"] = Json::nullValue;
        else
            item["createdAt"] = web::v1::ChatStore::toCreatedAtIso(row["created_at"].as<std::string>());
        item["createdBy"] = row["created_by"].as<std::string>();
        item["category"] = row["category"].as<std::string>();
        item["scheduleKind"] = row["schedule_kind"].as<std::string>();
        item["dayOfWeek"] = row["day_of_week"].as<std::string>();
        item["eventDate"] = row["event_date"].isNull() ? Json::Value() : Json::Value(row["event_date"].as<std::string>());
        item["startMinute"] = row["start_minute"].isNull() ? Json::Value() : Json::Value(row["start_minute"].as<int>());
        item["endMinute"] = row["end_minute"].isNull() ? Json::Value() : Json::Value(row["end_minute"].as<int>());
        item["done"] = row["done"].as<int>() != 0;
        item["sourceInsightId"] = row["source_insight_id"].isNull()
            ? Json::Value()
            : Json::Value(static_cast<Json::Int64>(row["source_insight_id"].as<int64_t>()));
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value ruleRowToJson(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = row["external_id"].as<std::string>();
        item["dbId"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["name"] = row["name"].as<std::string>();
        item["enabled"] = row["enabled"].as<int>() != 0;
        item["cooldownMs"] = row["cooldown_ms"].as<int>();
        item["trigger"] = row["trigger_json"].isNull()
            ? Json::Value()
            : parseJsonText(row["trigger_json"].as<std::string>());
        item["schedule"] = row["schedule_json"].isNull()
            ? Json::Value()
            : parseJsonText(row["schedule_json"].as<std::string>());

        const auto actions = parseJsonText(row["actions_json"].as<std::string>(), Json::Value(Json::objectValue));
        if (actions.isObject())
        {
            item["action"] = actions;
            item["execMode"] = actions.get("execMode", "once");
            item["repeatIntervalMs"] = actions.get("repeatIntervalMs", 0);
        }
        else if (actions.isArray() && !actions.empty())
        {
            item["action"] = actions[0];
            item["execMode"] = actions[0].get("execMode", "once");
            item["repeatIntervalMs"] = actions[0].get("repeatIntervalMs", 0);
        }
        else
        {
            item["action"] = Json::Value(Json::objectValue);
            item["execMode"] = "once";
            item["repeatIntervalMs"] = 0;
        }
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value chatRowToJson(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["title"] = row["title"].as<std::string>();
        item["createdAt"] = web::v1::ChatStore::toCreatedAtIso(row["created_at"].as<std::string>());
        item["updatedAt"] = web::v1::ChatStore::toCreatedAtIso(row["updated_at"].as<std::string>());
        item["messages"] = parseJsonText(row["message"].as<std::string>(), Json::Value(Json::arrayValue));
        if (!item["messages"].isArray())
            item["messages"] = Json::Value(Json::arrayValue);
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value chatToSummary(const Json::Value& conversation)
    {
        Json::Value summary;
        summary["id"] = conversation["id"];
        summary["title"] = conversation.get("title", "새 대화");
        const auto& messages = conversation["messages"];
        summary["messageCount"] = messages.isArray() ? static_cast<Json::UInt>(messages.size()) : 0;
        std::string preview;
        if (messages.isArray() && !messages.empty())
        {
            const auto& last = messages[static_cast<Json::ArrayIndex>(messages.size() - 1u)];
            if (last.isMember("text") && last["text"].isString())
                preview = last["text"].asString();
        }
        summary["lastMessagePreview"] = preview.empty() ? Json::Value() : Json::Value(preview);
        summary["createdAt"] = conversation.get("createdAt", "");
        summary["updatedAt"] = conversation.get("updatedAt", "");
        return summary;
    }

    Json::Value makeUserMessage(int64_t message_id, const std::string& text, const std::string& created_at)
    {
        Json::Value message;
        message["id"] = static_cast<Json::Int64>(message_id);
        message["role"] = "user";
        message["text"] = text;
        message["createdAt"] = web::v1::ChatStore::toCreatedAtIso(created_at);
        return message;
    }

    Json::Value* findChatConversation(DemoSessionData& session, int64_t user_id, int64_t conversation_id)
    {
        for (Json::ArrayIndex i = 0; i < session.chat_histories.size(); ++i)
        {
            auto& item = session.chat_histories[i];
            if (!item.isObject())
                continue;
            if (item.get("id", Json::Int64(0)).asInt64() != conversation_id)
                continue;
            if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
                continue;
            return &item;
        }
        return nullptr;
    }
}

void ensureDemoSessionSeeded(const std::string& runtime_id, const drogon::orm::DbClientPtr& client)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    if (session.data_seeded || !client)
        return;

    session.alarms = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, name, time_minute, days_of_week, smart_wake, radar_device_id,"
             " device_id, method, enabled, created_at, updated_at FROM alarm ORDER BY id"))
    {
        session.alarms.append(alarmRowToJson(client, row));
    }

    session.schedule_tasks = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,"
             " event_date, start_minute, end_minute, done, source_insight_id"
             " FROM schedule_task ORDER BY id"))
    {
        session.schedule_tasks.append(scheduleRowToJson(row));
    }

    session.rules = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json"
             " FROM automation_rule ORDER BY id"))
    {
        session.rules.append(ruleRowToJson(row));
    }

    session.chat_histories = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, title, created_at, updated_at, message FROM chat_history ORDER BY id"))
    {
        session.chat_histories.append(chatRowToJson(row));
    }

    session.data_seeded = true;
}

Json::Value demoListAlarms(
    const std::string& runtime_id,
    const int64_t user_id,
    const drogon::orm::DbClientPtr& client,
    const std::optional<bool>& enabled_filter)
{
    ensureDemoSessionSeeded(runtime_id, client);
    Json::Value items(Json::arrayValue);
    const auto session = DemoSessionRegistry::instance().get(runtime_id);
    if (!session)
        return items;

    for (const auto& item : session->alarms)
    {
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        if (enabled_filter && item.get("enabled", true).asBool() != *enabled_filter)
            continue;
        items.append(item);
    }
    return items;
}

Json::Value demoCreateAlarm(
    const std::string& runtime_id,
    const Json::Value& body,
    const drogon::orm::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    if (!body.isMember("userId"))
    {
        error = "userId가 필요합니다.";
        field = "userId";
        return Json::Value();
    }
    if (!body.isMember("timeMinute"))
    {
        error = "timeMinute가 필요합니다.";
        field = "timeMinute";
        return Json::Value();
    }

    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value alarm;
    alarm["id"] = static_cast<Json::Int64>(nextNumericId(session.alarms));
    alarm["userId"] = body["userId"];
    alarm["name"] = body.get("name", "알람").asString();
    alarm["timeMinute"] = body["timeMinute"].asInt();
    alarm["daysOfWeek"] = body.isMember("daysOfWeek") ? body["daysOfWeek"] : Json::Value(Json::arrayValue);
    alarm["smartWake"] = body.get("smartWake", false).asBool();
    alarm["enabled"] = body.get("enabled", true).asBool();
    alarm["method"] = body.isMember("method") ? body["method"] : Json::Value(Json::objectValue);
    alarm["radarDeviceId"] = body.isMember("radarDeviceId") ? body["radarDeviceId"] : Json::Value();
    alarm["deviceId"] = body.isMember("deviceId") ? body["deviceId"] : Json::Value();
    alarm["sessionScoped"] = true;
    alarm["createdAt"] = nowStamp();
    alarm["updatedAt"] = nowStamp();
    session.alarms.append(alarm);
    return alarm;
}

Json::Value demoUpdateAlarm(
    const std::string& runtime_id,
    const int64_t alarm_id,
    const Json::Value& body,
    std::string& error,
    std::string& /*field*/)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    for (Json::ArrayIndex i = 0; i < session.alarms.size(); ++i)
    {
        auto& item = session.alarms[i];
        if (!item.isObject() || item.get("id", Json::Int64(0)).asInt64() != alarm_id)
            continue;
        for (const auto& key : body.getMemberNames())
        {
            if (key == "id" || key == "userId" || key == "demoRuntimeId")
                continue;
            item[key] = body[key];
        }
        item["updatedAt"] = nowStamp();
        return item;
    }
    error = "세션 알람을 찾을 수 없습니다.";
    return Json::Value();
}

bool demoDeleteAlarm(const std::string& runtime_id, const int64_t alarm_id)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value kept(Json::arrayValue);
    bool removed = false;
    for (const auto& item : session.alarms)
    {
        if (item.isObject() && item.get("id", Json::Int64(0)).asInt64() == alarm_id)
        {
            removed = true;
            continue;
        }
        kept.append(item);
    }
    session.alarms = kept;
    return removed;
}

Json::Value demoListScheduleTasks(
    const std::string& runtime_id,
    const int64_t user_id,
    const drogon::orm::DbClientPtr& client)
{
    if (client)
        ensureDemoSessionSeeded(runtime_id, client);

    Json::Value items(Json::arrayValue);
    if (const auto session = DemoSessionRegistry::instance().get(runtime_id))
    {
        for (const auto& item : session->schedule_tasks)
        {
            if (!item.isObject())
                continue;
            if (item.get("userId", Json::Int64(0)).asInt64() == user_id)
                items.append(item);
        }
    }
    return items;
}

Json::Value demoCreateScheduleTask(
    const std::string& runtime_id,
    const Json::Value& body,
    std::string& error,
    std::string& field)
{
    if (!body.isMember("userId"))
    {
        error = "userId가 필요합니다.";
        field = "userId";
        return Json::Value();
    }
    if (!body.isMember("title"))
    {
        error = "title이 필요합니다.";
        field = "title";
        return Json::Value();
    }

    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value task;
    task["id"] = static_cast<Json::Int64>(nextNumericId(session.schedule_tasks));
    task["userId"] = body["userId"];
    task["title"] = body["title"].asString();
    task["category"] = body.get("category", "general").asString();
    task["scheduleKind"] = body.get("scheduleKind", "weekly").asString();
    task["dayOfWeek"] = body.get("dayOfWeek", "mon").asString();
    task["eventDate"] = body.isMember("eventDate") ? body["eventDate"] : Json::Value();
    task["startMinute"] = body.get("startMinute", 540).asInt();
    task["endMinute"] = body.isMember("endMinute") ? body["endMinute"] : Json::Value();
    task["done"] = body.get("done", false).asBool();
    task["createdBy"] = body.get("createdBy", "user").asString();
    task["sourceInsightId"] = body.isMember("sourceInsightId") ? body["sourceInsightId"] : Json::Value();
    task["sessionScoped"] = true;
    task["createdAt"] = web::v1::ChatStore::toCreatedAtIso(nowStamp());
    session.schedule_tasks.append(task);
    return task;
}

Json::Value demoUpdateScheduleTask(
    const std::string& runtime_id,
    const int64_t task_id,
    const Json::Value& body,
    std::string& error,
    std::string& /*field*/)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    for (Json::ArrayIndex i = 0; i < session.schedule_tasks.size(); ++i)
    {
        auto& item = session.schedule_tasks[i];
        if (!item.isObject() || item.get("id", Json::Int64(0)).asInt64() != task_id)
            continue;
        for (const auto& key : body.getMemberNames())
        {
            if (key == "id" || key == "userId" || key == "demoRuntimeId")
                continue;
            item[key] = body[key];
        }
        return item;
    }
    error = "세션 일정을 찾을 수 없습니다.";
    return Json::Value();
}

bool demoDeleteScheduleTask(const std::string& runtime_id, const int64_t task_id)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value kept(Json::arrayValue);
    bool removed = false;
    for (const auto& item : session.schedule_tasks)
    {
        if (item.isObject() && item.get("id", Json::Int64(0)).asInt64() == task_id)
        {
            removed = true;
            continue;
        }
        kept.append(item);
    }
    session.schedule_tasks = kept;
    return removed;
}

Json::Value demoCreateRule(const std::string& runtime_id, const Json::Value& body, std::string& code)
{
    if (!body.isObject())
    {
        code = "INVALID_REQUEST";
        return Json::Value();
    }
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value rule = body;
    if (!rule.isMember("id") || !rule["id"].isString() || rule["id"].asString().empty())
        rule["id"] = std::string("demo_rule_") + std::to_string(nextNumericId(session.rules));
    rule["enabled"] = body.get("enabled", true);
    rule["sessionScoped"] = true;
    session.rules.append(rule);
    return rule;
}

Json::Value demoListRules(const std::string& runtime_id, const int64_t user_id)
{
    Json::Value items(Json::arrayValue);
    if (const auto session = DemoSessionRegistry::instance().get(runtime_id))
    {
        for (const auto& item : session->rules)
        {
            if (!item.isObject())
                continue;
            if (user_id > 0 && item.isMember("userId") &&
                item.get("userId", Json::Int64(0)).asInt64() != user_id)
            {
                continue;
            }
            items.append(item);
        }
    }
    return items;
}

Json::Value demoUpdateRule(
    const std::string& runtime_id,
    const std::string& rule_id,
    const Json::Value& body,
    std::string& code)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    for (Json::ArrayIndex i = 0; i < session.rules.size(); ++i)
    {
        auto& item = session.rules[i];
        if (!item.isObject() || item.get("id", "").asString() != rule_id)
            continue;
        for (const auto& key : body.getMemberNames())
        {
            if (key == "id" || key == "demoRuntimeId")
                continue;
            item[key] = body[key];
        }
        return item;
    }
    code = "NOT_FOUND";
    return Json::Value();
}

bool demoDeleteRule(const std::string& runtime_id, const std::string& rule_id)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value kept(Json::arrayValue);
    bool removed = false;
    for (const auto& item : session.rules)
    {
        if (item.isObject() && item.get("id", "").asString() == rule_id)
        {
            removed = true;
            continue;
        }
        kept.append(item);
    }
    session.rules = kept;
    return removed;
}

Json::Value demoListChatSummaries(
    const std::string& runtime_id,
    const int64_t user_id,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    Json::Value items(Json::arrayValue);
    const auto session = DemoSessionRegistry::instance().get(runtime_id);
    if (!session)
        return items;

    std::vector<Json::Value> matched;
    for (const auto& item : session->chat_histories)
    {
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        matched.push_back(chatToSummary(item));
    }
    std::sort(matched.begin(), matched.end(), [](const Json::Value& a, const Json::Value& b) {
        return a.get("updatedAt", "").asString() > b.get("updatedAt", "").asString();
    });
    for (const auto& item : matched)
        items.append(item);
    return items;
}

std::optional<Json::Value> demoGetChatConversation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    if (const auto* found = findChatConversation(session, user_id, conversation_id))
    {
        Json::Value out = *found;
        out.removeMember("userId");
        out.removeMember("sessionScoped");
        return out;
    }
    return std::nullopt;
}

std::optional<Json::Value> demoCreateChatConversation(
    const std::string& runtime_id,
    const int64_t user_id,
    const std::string& title,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    const auto stamp = nowStamp();
    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(nextNumericId(session.chat_histories));
    conversation["userId"] = static_cast<Json::Int64>(user_id);
    conversation["title"] = title;
    conversation["createdAt"] = web::v1::ChatStore::toCreatedAtIso(stamp);
    conversation["updatedAt"] = web::v1::ChatStore::toCreatedAtIso(stamp);
    conversation["messages"] = Json::Value(Json::arrayValue);
    conversation["sessionScoped"] = true;
    session.chat_histories.append(conversation);

    Json::Value out = conversation;
    out.removeMember("userId");
    out.removeMember("sessionScoped");
    return out;
}

std::optional<Json::Value> demoRenameChatConversation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const std::string& title,
    std::string& error,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    auto* found = findChatConversation(session, user_id, conversation_id);
    if (!found)
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }
    (*found)["title"] = title;
    (*found)["updatedAt"] = web::v1::ChatStore::toCreatedAtIso(nowStamp());
    return chatToSummary(*found);
}

bool demoDeleteChatConversation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    Json::Value kept(Json::arrayValue);
    bool removed = false;
    for (const auto& item : session.chat_histories)
    {
        if (item.isObject() &&
            item.get("id", Json::Int64(0)).asInt64() == conversation_id &&
            item.get("userId", Json::Int64(0)).asInt64() == user_id)
        {
            removed = true;
            continue;
        }
        kept.append(item);
    }
    session.chat_histories = kept;
    return removed;
}

std::optional<Json::Value> demoAppendChatUserMessage(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const std::string& text,
    std::string& error,
    const drogon::orm::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    auto* found = findChatConversation(session, user_id, conversation_id);
    if (!found)
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }

    auto& messages = (*found)["messages"];
    if (!messages.isArray())
        messages = Json::Value(Json::arrayValue);
    const auto stamp = nowStamp();
    const auto message_id = demoNextChatMessageId(messages);
    const auto user_message = makeUserMessage(message_id, text, stamp);
    messages.append(user_message);
    (*found)["updatedAt"] = web::v1::ChatStore::toCreatedAtIso(stamp);

    Json::Value out = *found;
    out.removeMember("userId");
    out.removeMember("sessionScoped");
    out["userMessage"] = user_message;
    return out;
}

std::optional<Json::Value> demoCreateChatWithUserMessage(
    const std::string& runtime_id,
    const int64_t user_id,
    const std::string& text,
    std::string& error,
    const drogon::orm::DbClientPtr& client)
{
    if (text.empty())
    {
        error = "메시지를 입력해주세요.";
        return std::nullopt;
    }
    const auto title = web::v1::ChatStore::titleFromText(text);
    auto created = demoCreateChatConversation(runtime_id, user_id, title, client);
    if (!created)
        return std::nullopt;
    return demoAppendChatUserMessage(runtime_id, user_id, (*created)["id"].asInt64(), text, error, client);
}

bool demoAppendChatAssistantMessage(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const int64_t message_id,
    const std::string& text,
    const Json::Value& tool_events,
    const std::string& reasoning)
{
    auto& session = DemoSessionRegistry::instance().touch(runtime_id);
    auto* found = findChatConversation(session, user_id, conversation_id);
    if (!found)
        return false;

    auto& messages = (*found)["messages"];
    if (!messages.isArray())
        messages = Json::Value(Json::arrayValue);

    const auto stamp = nowStamp();
    Json::Value message;
    message["id"] = static_cast<Json::Int64>(message_id);
    message["role"] = "assistant";
    message["text"] = text;
    message["status"] = "done";
    message["reasoning"] = reasoning.empty() ? Json::Value() : Json::Value(reasoning);
    message["toolEvents"] = tool_events.isArray() ? tool_events : Json::Value(Json::arrayValue);
    message["createdAt"] = web::v1::ChatStore::toCreatedAtIso(stamp);
    messages.append(message);
    (*found)["updatedAt"] = web::v1::ChatStore::toCreatedAtIso(stamp);
    return true;
}

int64_t demoNextChatMessageId(const Json::Value& messages)
{
    int64_t max_id = 0;
    if (messages.isArray())
    {
        for (const auto& message : messages)
        {
            if (message.isObject() && message.isMember("id") && message["id"].isIntegral())
                max_id = std::max(max_id, message["id"].asInt64());
        }
    }
    return max_id + 1;
}

WAVE_NAMESPACE_END
