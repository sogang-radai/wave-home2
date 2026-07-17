#pragma once

#include "alarms_facade.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class RealAlarmsFacade :
    public IAlarmsFacade
{
public:
    Json::Value list(
        int64_t user_id,
        const std::optional<bool>& enabled,
        const std::string& runtime_id,
        const db::DbClientPtr& client) override;

    Json::Value create(
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    Json::Value update(
        int64_t user_id,
        int64_t alarm_id,
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) override;

    Json::Value remove(
        int64_t user_id,
        int64_t alarm_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error) override;
};

} // namespace facade
WAVE_NAMESPACE_END
