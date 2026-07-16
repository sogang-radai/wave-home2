#pragma once

#include <cstdint>
#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class NotificationsStore
{
public:
    explicit NotificationsStore(db::DbClientPtr client);

    Json::Value listForUser(int64_t user_id, int limit = 0, int64_t before_id = 0) const;
    Json::Value markAllRead(int64_t user_id) const;
    Json::Value markRead(int64_t user_id, int64_t notification_id) const;

private:
    db::DbClientPtr m_client;

    static std::string to_created_at_iso(const std::string& db_time);
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
