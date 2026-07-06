#include "notifications_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

NotificationsStore::NotificationsStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string NotificationsStore::toCreatedAtIso(const std::string& db_time)
{
    if (db_time.size() >= 19)
        return db_time.substr(0, 10) + "T" + db_time.substr(11, 8) + "+09:00";
    return db_time;
}

Json::Value NotificationsStore::listForUser(int64_t user_id) const
{
    Json::Value body(Json::arrayValue);
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, type, message, read, created_at
FROM notification
WHERE user_id = ?
ORDER BY created_at DESC, id DESC
)SQL",
        user_id);

    for (const auto& row : rows)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["type"] = row["type"].as<std::string>();
        item["message"] = row["message"].as<std::string>();
        item["createdAt"] = toCreatedAtIso(row["created_at"].as<std::string>());
        item["read"] = row["read"].as<int>() != 0;
        body.append(item);
    }
    return body;
}

Json::Value NotificationsStore::markAllRead(int64_t user_id) const
{
    m_client->execSqlSync(
        "UPDATE notification SET read = 1 WHERE user_id = ? AND read = 0",
        user_id);
    return listForUser(user_id);
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
