#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <json/json.h>

#include "../core/coredefs.h"
#include "../db/database.h"

WAVE_NAMESPACE_BEGIN
namespace facade {

class IAlarmsFacade
{
public:
    virtual ~IAlarmsFacade() = default;

    virtual Json::Value list(
        int64_t user_id,
        const std::optional<bool>& enabled,
        const std::string& runtime_id,
        const db::DbClientPtr& client) = 0;

    virtual Json::Value create(
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual Json::Value update(
        int64_t user_id,
        int64_t alarm_id,
        const Json::Value& body,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error,
        std::string& field) = 0;

    virtual Json::Value remove(
        int64_t user_id,
        int64_t alarm_id,
        const std::string& runtime_id,
        const db::DbClientPtr& client,
        std::string& error) = 0;
};

} // namespace facade
WAVE_NAMESPACE_END
