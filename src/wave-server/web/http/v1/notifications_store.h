#pragma once

#include <cstdint>
#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class NotificationsStore
{
public:
    explicit NotificationsStore(drogon::orm::DbClientPtr client);

    Json::Value listForUser(int64_t user_id) const;
    Json::Value markAllRead(int64_t user_id) const;

private:
    drogon::orm::DbClientPtr m_client;

    static std::string toCreatedAtIso(const std::string& db_time);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
