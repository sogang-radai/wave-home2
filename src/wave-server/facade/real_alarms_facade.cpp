#include "real_alarms_facade.h"

#include "../service/alarm_manager.h"
#include "../web/http/internal/alarms_internal_store.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

Json::Value RealAlarmsFacade::list(
    int64_t user_id,
    const std::optional<bool>& enabled,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client)
{
    web::internal::AlarmListFilter filter;
    filter.user_id = user_id;
    filter.enabled = enabled;
    return web::internal::AlarmsInternalStore(client).listAlarms(filter);
}

Json::Value RealAlarmsFacade::create(
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    const auto created = web::internal::AlarmsInternalStore(client).createAlarm(body, error, field);
    if (!created.isNull())
        service::AlarmManager::get().reconcile();
    return created;
}

Json::Value RealAlarmsFacade::update(
    int64_t user_id,
    int64_t alarm_id,
    const Json::Value& body,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error,
    std::string& field)
{
    const auto updated =
        web::internal::AlarmsInternalStore(client).updateAlarm(user_id, alarm_id, body, error, field);
    if (!updated.isNull())
        service::AlarmManager::get().reconcile();
    return updated;
}

Json::Value RealAlarmsFacade::remove(
    int64_t user_id,
    int64_t alarm_id,
    const std::string& /*runtime_id*/,
    const db::DbClientPtr& client,
    std::string& error)
{
    const auto removed = web::internal::AlarmsInternalStore(client).deleteAlarm(user_id, alarm_id, error);
    if (!removed.isNull())
        service::AlarmManager::get().reconcile();
    return removed;
}

} // namespace facade
WAVE_NAMESPACE_END
