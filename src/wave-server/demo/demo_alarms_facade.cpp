#include "demo_alarms_facade.h"

#include "demo_session_writes.h"

WAVE_NAMESPACE_BEGIN

Json::Value DemoAlarmsFacade::list(
    int64_t user_id,
    const std::optional<bool>& enabled,
    const std::string& runtime_id,
    const db::DbClientPtr& client)
{
    return demoListAlarms(runtime_id, user_id, client, enabled);
}

Json::Value DemoAlarmsFacade::create(
    const Json::Value& body,
    const std::string& runtime_id,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    return demoCreateAlarm(runtime_id, body, client, error, field);
}

Json::Value DemoAlarmsFacade::update(
    int64_t /*user_id*/,
    int64_t alarm_id,
    const Json::Value& body,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/,
    std::string& error,
    std::string& field)
{
    return demoUpdateAlarm(runtime_id, alarm_id, body, error, field);
}

Json::Value DemoAlarmsFacade::remove(
    int64_t /*user_id*/,
    int64_t alarm_id,
    const std::string& runtime_id,
    const db::DbClientPtr& /*client*/,
    std::string& error)
{
    if (demoDeleteAlarm(runtime_id, alarm_id))
    {
        Json::Value removed;
        removed["id"] = static_cast<Json::Int64>(alarm_id);
        return removed;
    }
    error = "세션 알람을 찾을 수 없습니다.";
    return Json::Value();
}

WAVE_NAMESPACE_END
