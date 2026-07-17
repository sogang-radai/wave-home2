#include "demo_session_writes.h"
#include "../db/database.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

#include "../core/json.h"
#include "../device/device_wire_id.hpp"
#include "../service/trigger_types.h"
#include "../web/http/v1/chat_store.h"
#include "../web/http/v1/settings_store.h"
#include "demo_device_backend.h"
#include "demo_session_registry.h"

WAVE_NAMESPACE_BEGIN

namespace
{
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

    Json::Value json_cpp_from_nlohmann(const json& value)
    {
        Json::CharReaderBuilder reader;
        Json::Value out;
        std::string errors;
        std::istringstream stream(value.dump());
        Json::parseFromStream(reader, stream, &out, &errors);
        return out;
    }

    json nlohmann_from_json_cpp(const Json::Value& value)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        return json::parse(Json::writeString(builder, value));
    }

    std::string wire_id_for_internal_device(const db::DbClientPtr& client, int64_t internal_id)
    {
        if (!client || internal_id <= 0)
            return {};
        auto rows = client->execSqlSync("SELECT name FROM device WHERE id = ?", internal_id);
        if (rows.empty())
            return {};
        return dev::wireIdForDbRow(internal_id, rows[0]["name"].as<std::string>());
    }

    Json::Value alarm_row_to_json(const db::DbClientPtr& client, const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["name"] = row["name"].as<std::string>();
        item["timeMinute"] = row["time_minute"].as<int>();
        item["daysOfWeek"] = parseJsonText(row["days_of_week"].as<std::string>(), Json::Value(Json::arrayValue));
        item["repeatWeekly"] = item["daysOfWeek"].isArray() && !item["daysOfWeek"].empty();
        item["smartWake"] = row["smart_wake"].as<int>() != 0;
        if (row["radar_device_id"].isNull())
            item["radarDeviceId"] = Json::nullValue;
        else
        {
            const auto wire = wire_id_for_internal_device(client, row["radar_device_id"].as<int64_t>());
            item["radarDeviceId"] = wire.empty() ? Json::nullValue : Json::Value(wire);
        }
        if (row["device_id"].isNull())
            item["deviceId"] = Json::nullValue;
        else
        {
            const auto wire = wire_id_for_internal_device(client, row["device_id"].as<int64_t>());
            item["deviceId"] = wire.empty() ? Json::nullValue : Json::Value(wire);
        }
        item["method"] = parseJsonText(row["method"].as<std::string>(), Json::Value(Json::objectValue));
        item["enabled"] = row["enabled"].as<int>() != 0;
        item["createdAt"] = row["created_at"].as<std::string>();
        item["updatedAt"] = row["updated_at"].as<std::string>();
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value schedule_row_to_json(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["title"] = row["title"].as<std::string>();
        if (row["created_at"].isNull())
            item["createdAt"] = Json::nullValue;
        else
            item["createdAt"] = web::v1::ChatStore::to_created_at_iso(row["created_at"].as<std::string>());
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

    Json::Value rule_row_to_json(const drogon::orm::Row& row)
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
            : demoNormalizeRuleSchedule(parseJsonText(row["schedule_json"].as<std::string>()));

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

    Json::Value chat_row_to_json(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["title"] = row["title"].as<std::string>();
        const auto created = row["created_at"].as<std::string>();
        item["createdAt"] = web::v1::ChatStore::to_created_at_iso(created);
        item["updatedAt"] = web::v1::ChatStore::to_created_at_iso(row["updated_at"].as<std::string>());
        item["messages"] = web::v1::ChatStore::normalize_messages_json(
            parseJsonText(row["message"].as<std::string>(), Json::Value(Json::arrayValue)),
            created);
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value chat_to_summary(const Json::Value& conversation)
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
        message["createdAt"] = web::v1::ChatStore::to_created_at_iso(created_at);
        return message;
    }

    Json::Value* find_chat_conversation(DemoSessionData& session, int64_t user_id, int64_t conversation_id)
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

    Json::Value notification_row_to_json(const drogon::orm::Row& row)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["userId"] = static_cast<Json::Int64>(row["user_id"].as<int64_t>());
        item["type"] = row["type"].as<std::string>();
        item["message"] = row["message"].as<std::string>();
        item["read"] = row["read"].as<int>() != 0;
        const auto created = row["created_at"].as<std::string>();
        if (created.size() >= 19)
            item["createdAt"] = created.substr(0, 10) + "T" + created.substr(11, 8) + "+09:00";
        else
            item["createdAt"] = created;
        item["sessionScoped"] = true;
        return item;
    }

    Json::Value notification_public_view(const Json::Value& item)
    {
        Json::Value view;
        view["id"] = item["id"];
        view["type"] = item.get("type", "");
        view["message"] = item.get("message", "");
        view["createdAt"] = item.get("createdAt", "");
        view["read"] = item.get("read", false).asBool();
        return view;
    }

    int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string voice_label_for_method(const Json::Value& method)
    {
        if (method.isMember("voiceLabel") && method["voiceLabel"].isString()
            && !method["voiceLabel"].asString().empty())
        {
            return method["voiceLabel"].asString();
        }
        if (method.isMember("speakerId") && !method["speakerId"].isNull())
        {
            if (method["speakerId"].isString() && !method["speakerId"].asString().empty())
                return method["speakerId"].asString();
            if (method["speakerId"].isIntegral())
                return "음성";
        }
        return "TTS";
    }

    void invoke_demo_device_action(
        const std::string& runtime_id,
        const std::string& device_id,
        const std::string& action_name,
        const Json::Value& params,
        const db::DbClientPtr& client)
    {
        if (!client || device_id.empty() || action_name.empty())
            return;

        DemoDeviceBackend backend(client);
        Json::Value body(Json::objectValue);
        body["params"] = params.isObject() ? params : Json::Value(Json::objectValue);
        std::string code;
        backend.invokeAction(runtime_id, device_id, action_name, body, code);
    }

    void execute_alarm_method(
        const std::string& runtime_id,
        const Json::Value& alarm,
        const db::DbClientPtr& client)
    {
        const auto method = alarm.get("method", Json::Value(Json::objectValue));
        if (!method.isObject())
            return;

        const std::string type = method.get("type", "").asString();
        const std::string device_id = alarm.isMember("deviceId") && !alarm["deviceId"].isNull()
            ? alarm["deviceId"].asString()
            : std::string();

        if (type.empty() || type == "notification" || device_id.empty())
            return;

        if (type == "light_on")
        {
            const int brightness = std::clamp(method.get("brightness", 70).asInt(), 10, 100);
            invoke_demo_device_action(runtime_id, device_id, "on", Json::Value(Json::objectValue), client);
            Json::Value params(Json::objectValue);
            params["value"] = brightness;
            invoke_demo_device_action(runtime_id, device_id, "brightness", params, client);
            return;
        }

        if (type == "light_blink")
        {
            const int brightness = std::clamp(method.get("brightness", 70).asInt(), 10, 100);
            const int interval_sec = std::clamp(method.get("intervalSec", 2).asInt(), 1, 10);
            invoke_demo_device_action(runtime_id, device_id, "on", Json::Value(Json::objectValue), client);
            Json::Value params(Json::objectValue);
            params["value"] = brightness;
            invoke_demo_device_action(runtime_id, device_id, "brightness", params, client);
            std::thread([runtime_id, device_id, interval_sec, client]()
            {
                std::this_thread::sleep_for(std::chrono::seconds(interval_sec));
                invoke_demo_device_action(runtime_id, device_id, "off", Json::Value(Json::objectValue), client);
            }).detach();
            return;
        }

        if (type == "plug_toggle")
        {
            invoke_demo_device_action(runtime_id, device_id, "toggle", Json::Value(Json::objectValue), client);
            return;
        }
        if (type == "plug_on")
        {
            invoke_demo_device_action(runtime_id, device_id, "on", Json::Value(Json::objectValue), client);
            return;
        }
        if (type == "plug_off")
        {
            invoke_demo_device_action(runtime_id, device_id, "off", Json::Value(Json::objectValue), client);
            return;
        }

        if (type == "tts" || type == "sound")
        {
            std::string text = method.get("text", "").asString();
            if (text.empty() && type == "sound")
                text = method.get("soundId", "알람").asString();
            if (text.empty())
                text = alarm.get("name", "알람").asString();

            const int interval_sec = std::clamp(method.get("intervalSec", 5).asInt(), 1, 60);
            const int repeat_count = std::clamp(method.get("repeatCount", 1).asInt(), 1, 20);
            const int64_t now = nowMs();

            Json::Value overlay(Json::objectValue);
            overlay["voiceLabel"] = voice_label_for_method(method);
            overlay["text"] = text;
            overlay["expiresAtMs"] = static_cast<Json::Int64>(now + 8000);
            overlay["intervalSec"] = interval_sec;
            overlay["nextShowAtMs"] = Json::Value();
            overlay["alarmId"] = alarm.get("id", Json::Int64(0));
            overlay["alarmName"] = alarm.get("name", "알람");
            overlay["showsLeft"] = repeat_count - 1;
            demoSetSpeechOverlay(runtime_id, device_id, overlay);
        }
    }
}

void ensureDemoSessionSeeded(const std::string& runtime_id, const db::DbClientPtr& client)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (session.data_seeded || !client)
        return;

    session.alarms = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, name, time_minute, days_of_week, smart_wake, radar_device_id,"
             " device_id, method, enabled, created_at, updated_at FROM alarm ORDER BY id"))
    {
        session.alarms.append(alarm_row_to_json(client, row));
    }

    session.schedule_tasks = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, title, created_at, created_by, category, schedule_kind, day_of_week,"
             " event_date, start_minute, end_minute, done, source_insight_id"
             " FROM schedule_task ORDER BY id"))
    {
        session.schedule_tasks.append(schedule_row_to_json(row));
    }

    session.rules = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, external_id, name, enabled, cooldown_ms, trigger_json, schedule_json, actions_json"
             " FROM automation_rule ORDER BY id"))
    {
        session.rules.append(rule_row_to_json(row));
    }

    session.chat_histories = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, title, created_at, updated_at, message FROM chat_history ORDER BY id"))
    {
        session.chat_histories.append(chat_row_to_json(row));
    }

    session.notifications = Json::Value(Json::arrayValue);
    for (const auto& row : client->execSqlSync(
             "SELECT id, user_id, type, message, read, created_at FROM notification ORDER BY id"))
    {
        session.notifications.append(notification_row_to_json(row));
    }

    if (!session.speech_overlays.isObject())
        session.speech_overlays = Json::Value(Json::objectValue);

    session.ai_agent_settings = Json::Value(Json::objectValue);
    for (const auto& row : client->execSqlSync(
             "SELECT user_id, personal_prompt, selected_model_id, ctrl_enter_send, wave_ai_sound"
             " FROM user_ai_agent_settings"))
    {
        Json::Value item(Json::objectValue);
        item["personalPrompt"] = row["personal_prompt"].as<std::string>();
        item["selectedModelId"] = row["selected_model_id"].as<std::string>();
        item["ctrlEnterSend"] = row["ctrl_enter_send"].as<int>() != 0;
        item["waveAiSound"] = row["wave_ai_sound"].as<int>() != 0;
        session.ai_agent_settings[std::to_string(row["user_id"].as<int64_t>())] = item;
    }

    session.data_seeded = true;
}

Json::Value demoListAlarms(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client,
    const std::optional<bool>& enabled_filter)
{
    ensureDemoSessionSeeded(runtime_id, client);
    Json::Value items(Json::arrayValue);
    const auto session = demoSessionRegistry().get(runtime_id);
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
    const db::DbClientPtr& client,
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    Json::Value alarm;
    alarm["id"] = static_cast<Json::Int64>(next_numeric_id(session.alarms));
    alarm["userId"] = body["userId"];
    alarm["name"] = body.get("name", "알람").asString();
    alarm["timeMinute"] = body["timeMinute"].asInt();
    alarm["daysOfWeek"] = body.isMember("daysOfWeek") ? body["daysOfWeek"] : Json::Value(Json::arrayValue);
    if (body.isMember("repeatWeekly") && !body["repeatWeekly"].isNull())
        alarm["repeatWeekly"] = body["repeatWeekly"].asBool();
    else
        alarm["repeatWeekly"] = alarm["daysOfWeek"].isArray() && !alarm["daysOfWeek"].empty();
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
        if (body.isMember("enabled") && body["enabled"].asBool())
        {
            session.alarm_once_fired.erase(alarm_id);
            session.alarm_last_fired_date.erase(alarm_id);
        }
        return item;
    }
    error = "세션 알람을 찾을 수 없습니다.";
    return Json::Value();
}

bool demoDeleteAlarm(const std::string& runtime_id, const int64_t alarm_id)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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

Json::Value demoListNotifications(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client,
    const int limit,
    const int64_t before_id)
{
    ensureDemoSessionSeeded(runtime_id, client);
    Json::Value body(Json::objectValue);
    body["items"] = Json::Value(Json::arrayValue);
    body["unreadCount"] = 0;
    body["hasMore"] = false;

    const auto session = demoSessionRegistry().get(runtime_id);
    if (!session)
        return body;

    std::vector<Json::Value> matched;
    int unread = 0;
    for (const auto& item : session->notifications)
    {
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        if (!item.get("read", false).asBool())
            ++unread;
        matched.push_back(notification_public_view(item));
    }
    std::sort(matched.begin(), matched.end(), [](const Json::Value& a, const Json::Value& b)
    {
        const auto ca = a.get("createdAt", "").asString();
        const auto cb = b.get("createdAt", "").asString();
        if (ca != cb)
            return ca > cb;
        return a.get("id", Json::Int64(0)).asInt64() > b.get("id", Json::Int64(0)).asInt64();
    });

    Json::Value items(Json::arrayValue);
    const int page_size = limit > 0 ? limit : static_cast<int>(matched.size());
    bool started = before_id <= 0;
    int taken = 0;
    for (const auto& item : matched)
    {
        const int64_t id = item.get("id", Json::Int64(0)).asInt64();
        if (!started)
        {
            if (id == before_id)
                started = true;
            continue;
        }
        if (taken >= page_size)
        {
            body["hasMore"] = true;
            break;
        }
        items.append(item);
        ++taken;
    }

    body["items"] = items;
    body["unreadCount"] = unread;
    return body;
}

Json::Value demoMarkAllNotificationsRead(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    for (Json::ArrayIndex i = 0; i < session.notifications.size(); ++i)
    {
        auto& item = session.notifications[i];
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        item["read"] = true;
    }
    return demoListNotifications(runtime_id, user_id, nullptr, 0, 0);
}

Json::Value demoMarkNotificationRead(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t notification_id,
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    for (Json::ArrayIndex i = 0; i < session.notifications.size(); ++i)
    {
        auto& item = session.notifications[i];
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        if (item.get("id", Json::Int64(0)).asInt64() != notification_id)
            continue;
        item["read"] = true;
        return notification_public_view(item);
    }
    return Json::Value();
}

Json::Value demoAppendNotification(
    const std::string& runtime_id,
    const int64_t user_id,
    const std::string& type,
    const std::string& message)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (!session.notifications.isArray())
        session.notifications = Json::Value(Json::arrayValue);

    Json::Value item;
    item["id"] = static_cast<Json::Int64>(next_numeric_id(session.notifications));
    item["userId"] = static_cast<Json::Int64>(user_id);
    item["type"] = type;
    item["message"] = message;
    item["read"] = false;
    const auto stamp = nowStamp();
    if (stamp.size() >= 19)
        item["createdAt"] = stamp.substr(0, 10) + "T" + stamp.substr(11, 8) + "+09:00";
    else
        item["createdAt"] = stamp;
    item["sessionScoped"] = true;
    session.notifications.append(item);
    return notification_public_view(item);
}

Json::Value demoListSpeechOverlays(const std::string& runtime_id)
{
    const auto session = demoSessionRegistry().get(runtime_id);
    if (!session || !session->speech_overlays.isObject())
        return Json::Value(Json::objectValue);
    return session->speech_overlays;
}

void demoSetSpeechOverlay(
    const std::string& runtime_id,
    const std::string& device_id,
    const Json::Value& overlay)
{
    if (device_id.empty() || !overlay.isObject())
        return;
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (!session.speech_overlays.isObject())
        session.speech_overlays = Json::Value(Json::objectValue);
    session.speech_overlays[device_id] = overlay;
}

void demoClearSpeechOverlay(const std::string& runtime_id, const std::string& device_id)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (!session.speech_overlays.isObject())
        return;
    session.speech_overlays.removeMember(device_id);
}

void demoRefreshSpeechOverlays(const std::string& runtime_id, const int64_t now_ms)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (!session.speech_overlays.isObject())
        return;

    Json::Value next_map(Json::objectValue);
    for (const auto& device_id : session.speech_overlays.getMemberNames())
    {
        Json::Value overlay = session.speech_overlays[device_id];
        if (!overlay.isObject())
            continue;

        const int64_t expires_at = overlay.get("expiresAtMs", Json::Int64(0)).asInt64();
        const int interval_sec = overlay.get("intervalSec", 0).asInt();
        const int shows_left = overlay.get("showsLeft", 0).asInt();

        if (now_ms < expires_at)
        {
            next_map[device_id] = overlay;
            continue;
        }

        // Currently hidden — waiting for next show or finished.
        if (overlay.isMember("nextShowAtMs") && !overlay["nextShowAtMs"].isNull())
        {
            const int64_t next_show = overlay["nextShowAtMs"].asInt64();
            if (now_ms >= next_show)
            {
                overlay["expiresAtMs"] = static_cast<Json::Int64>(now_ms + 8000);
                overlay["nextShowAtMs"] = Json::Value();
                overlay["showsLeft"] = std::max(0, shows_left - 1);
                next_map[device_id] = overlay;
            }
            else
            {
                next_map[device_id] = overlay;
            }
            continue;
        }

        if (interval_sec > 0 && shows_left > 0)
        {
            overlay["nextShowAtMs"] = static_cast<Json::Int64>(now_ms + static_cast<int64_t>(interval_sec) * 1000);
            next_map[device_id] = overlay;
            continue;
        }

        // Expired with no more repeats — drop.
    }
    session.speech_overlays = next_map;
}

void demoDisableAlarm(const std::string& runtime_id, const int64_t alarm_id)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    for (Json::ArrayIndex i = 0; i < session.alarms.size(); ++i)
    {
        auto& item = session.alarms[i];
        if (!item.isObject() || item.get("id", Json::Int64(0)).asInt64() != alarm_id)
            continue;
        item["enabled"] = false;
        item["updatedAt"] = nowStamp();
        return;
    }
}

void demoFireAlarm(
    const std::string& runtime_id,
    const Json::Value& alarm,
    const db::DbClientPtr& client)
{
    if (!alarm.isObject())
        return;

    const int64_t user_id = alarm.get("userId", Json::Int64(0)).asInt64();
    const std::string name = alarm.get("name", "알람").asString();
    demoAppendNotification(
        runtime_id,
        user_id,
        "alarm",
        "\"" + name + "\" 알람이 울렸습니다.");
    execute_alarm_method(runtime_id, alarm, client);
}

Json::Value demoListScheduleTasks(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client)
{
    if (client)
        ensureDemoSessionSeeded(runtime_id, client);

    Json::Value items(Json::arrayValue);
    if (const auto session = demoSessionRegistry().get(runtime_id))
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

    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    Json::Value task;
    task["id"] = static_cast<Json::Int64>(next_numeric_id(session.schedule_tasks));
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
    task["createdAt"] = web::v1::ChatStore::to_created_at_iso(nowStamp());
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    Json::Value rule = body;
    if (!rule.isMember("id") || !rule["id"].isString() || rule["id"].asString().empty())
        rule["id"] = std::string("demo_rule_") + std::to_string(next_numeric_id(session.rules));
    rule["enabled"] = body.get("enabled", true);
    rule["sessionScoped"] = true;
    session.rules.append(rule);
    return rule;
}

Json::Value demoListRules(const std::string& runtime_id, const int64_t user_id)
{
    Json::Value items(Json::arrayValue);
    if (const auto session = demoSessionRegistry().get(runtime_id))
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
            Json::Value out = item;
            if (out.isMember("schedule") && out["schedule"].isObject())
                out["schedule"] = demoNormalizeRuleSchedule(out["schedule"]);
            items.append(out);
        }
    }
    return items;
}

Json::Value demoNormalizeRuleSchedule(const Json::Value& schedule)
{
    if (schedule.isNull() || !schedule.isObject() || schedule.empty())
        return Json::nullValue;

    service::RuleSchedule parsed;
    std::string error;
    if (!service::parseRuleScheduleFromJson(nlohmann_from_json_cpp(schedule), parsed, error))
        return Json::nullValue;

    // wave-home-agent RuleSchedule 과 맞춤: once 는 delayMinutes, daily/weekly 는 time 필수.
    if (parsed.repeat == "once" && !parsed.delayMinutes)
        return Json::nullValue;
    if ((parsed.repeat == "daily" || parsed.repeat == "weekly") && !parsed.time)
        return Json::nullValue;
    if (parsed.repeat == "weekly" && parsed.daysOfWeek.empty())
        return Json::nullValue;

    return json_cpp_from_nlohmann(service::ruleScheduleToJson(parsed));
}

Json::Value demoUpdateRule(
    const std::string& runtime_id,
    const std::string& rule_id,
    const Json::Value& body,
    std::string& code)
{
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    Json::Value items(Json::arrayValue);
    const auto session = demoSessionRegistry().get(runtime_id);
    if (!session)
        return items;

    std::vector<Json::Value> matched;
    for (const auto& item : session->chat_histories)
    {
        if (!item.isObject())
            continue;
        if (item.get("userId", Json::Int64(0)).asInt64() != user_id)
            continue;
        matched.push_back(chat_to_summary(item));
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
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (const auto* found = find_chat_conversation(session, user_id, conversation_id))
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
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    const auto stamp = nowStamp();
    Json::Value conversation;
    conversation["id"] = static_cast<Json::Int64>(next_numeric_id(session.chat_histories));
    conversation["userId"] = static_cast<Json::Int64>(user_id);
    conversation["title"] = title;
    conversation["createdAt"] = web::v1::ChatStore::to_created_at_iso(stamp);
    conversation["updatedAt"] = web::v1::ChatStore::to_created_at_iso(stamp);
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
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    auto* found = find_chat_conversation(session, user_id, conversation_id);
    if (!found)
    {
        error = "대화를 찾을 수 없습니다.";
        return std::nullopt;
    }
    (*found)["title"] = title;
    (*found)["updatedAt"] = web::v1::ChatStore::to_created_at_iso(nowStamp());
    return chat_to_summary(*found);
}

bool demoDeleteChatConversation(
    const std::string& runtime_id,
    const int64_t user_id,
    const int64_t conversation_id,
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
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
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    auto* found = find_chat_conversation(session, user_id, conversation_id);
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
    (*found)["updatedAt"] = web::v1::ChatStore::to_created_at_iso(stamp);

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
    const db::DbClientPtr& client)
{
    if (text.empty())
    {
        error = "메시지를 입력해주세요.";
        return std::nullopt;
    }
    const auto title = web::v1::ChatStore::title_from_text(text);
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
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    auto* found = find_chat_conversation(session, user_id, conversation_id);
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
    message["createdAt"] = web::v1::ChatStore::to_created_at_iso(stamp);
    messages.append(message);
    (*found)["updatedAt"] = web::v1::ChatStore::to_created_at_iso(stamp);
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

namespace
{
    Json::Value default_ai_agent_settings()
    {
        Json::Value value(Json::objectValue);
        value["personalPrompt"] = "";
        value["selectedModelId"] = "gemini-flash2.5";
        value["ctrlEnterSend"] = false;
        value["waveAiSound"] = true;
        return value;
    }

    std::string trim_copy(std::string value)
    {
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\r\n");
        return value.substr(start, end - start + 1);
    }

    size_t utf8_sequence_length(unsigned char lead)
    {
        if (lead < 0x80)
            return 1;
        if ((lead & 0xE0) == 0xC0)
            return 2;
        if ((lead & 0xF0) == 0xE0)
            return 3;
        if ((lead & 0xF8) == 0xF0)
            return 4;
        return 0;
    }

    size_t utf8_code_point_count(const std::string& text)
    {
        size_t count = 0;
        for (size_t i = 0; i < text.size();)
        {
            const auto len = utf8_sequence_length(static_cast<unsigned char>(text[i]));
            if (len == 0 || i + len > text.size())
                break;
            i += len;
            ++count;
        }
        return count;
    }
}

Json::Value demoGetAiAgentSettings(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client)
{
    ensureDemoSessionSeeded(runtime_id, client);
    const auto session = demoSessionRegistry().get(runtime_id);
    Json::Value value = default_ai_agent_settings();
    if (!session || !session->ai_agent_settings.isObject())
        return value;

    const auto key = std::to_string(user_id);
    if (!session->ai_agent_settings.isMember(key) || !session->ai_agent_settings[key].isObject())
        return value;

    const auto& stored = session->ai_agent_settings[key];
    for (const auto& member : stored.getMemberNames())
        value[member] = stored[member];
    return value;
}

Json::Value demoPutAiAgentSettings(
    const std::string& runtime_id,
    const int64_t user_id,
    const Json::Value& body,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    ensureDemoSessionSeeded(runtime_id, client);
    auto locked_session = demoSessionRegistry().lockSession(runtime_id);
    auto& session = *locked_session;
    if (!session.ai_agent_settings.isObject())
        session.ai_agent_settings = Json::Value(Json::objectValue);

    Json::Value out = demoGetAiAgentSettings(runtime_id, user_id, nullptr);
    for (const auto& key : body.getMemberNames())
    {
        if (key == "demoRuntimeId")
            continue;
        out[key] = body[key];
    }

    if (body.isMember("personalPrompt"))
    {
        if (!body["personalPrompt"].isString())
        {
            error = "personalPrompt는 문자열이어야 합니다.";
            field = "personalPrompt";
            return Json::Value();
        }
        // Always replace from the request body (never concatenate with the previous value).
        const auto prompt = body["personalPrompt"].asString();
        if (utf8_code_point_count(prompt) > web::v1::kPersonalPromptMaxChars)
        {
            error = "개인 프롬프트는 " + std::to_string(web::v1::kPersonalPromptMaxChars)
                + "자 이하여야 합니다.";
            field = "personalPrompt";
            return Json::Value();
        }
        out["personalPrompt"] = trim_copy(prompt);
    }

    session.ai_agent_settings[std::to_string(user_id)] = out;
    return out;
}

std::string demoResolvePersonalPrompt(
    const std::string& runtime_id,
    const int64_t user_id,
    const db::DbClientPtr& client)
{
    const auto settings = demoGetAiAgentSettings(runtime_id, user_id, client);
    if (!settings.isMember("personalPrompt") || !settings["personalPrompt"].isString())
        return {};
    return trim_copy(settings["personalPrompt"].asString());
}

WAVE_NAMESPACE_END
