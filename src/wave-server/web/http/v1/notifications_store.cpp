#include "notifications_store.h"
#include "../../../db/database.h"

#include <vector>

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

NotificationsStore::NotificationsStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string NotificationsStore::to_created_at_iso(const std::string& db_time)
{
    if (db_time.size() >= 19)
        return db_time.substr(0, 10) + "T" + db_time.substr(11, 8) + "+09:00";
    return db_time;
}

Json::Value NotificationsStore::listForUser(
    int64_t user_id,
    int limit,
    int64_t before_id) const
{
    Json::Value body(Json::objectValue);
    body["items"] = Json::Value(Json::arrayValue);
    body["unreadCount"] = 0;
    body["hasMore"] = false;

    auto unread_rows = m_client->execSqlSync(
        "SELECT COUNT(*) AS cnt FROM notification WHERE user_id = ? AND read = 0",
        user_id);
    if (!unread_rows.empty())
        body["unreadCount"] = unread_rows[0]["cnt"].as<int>();

    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, type, message, read, created_at
FROM notification
WHERE user_id = ?
ORDER BY created_at DESC, id DESC
)SQL",
        user_id);

    std::vector<Json::Value> matched;
    matched.reserve(rows.size());
    for (const auto& row : rows)
    {
        Json::Value item;
        item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
        item["type"] = row["type"].as<std::string>();
        item["message"] = row["message"].as<std::string>();
        item["createdAt"] = to_created_at_iso(row["created_at"].as<std::string>());
        item["read"] = row["read"].as<int>() != 0;
        matched.push_back(item);
    }

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
    return body;
}

Json::Value NotificationsStore::markAllRead(int64_t user_id) const
{
    m_client->execSqlSync(
        "UPDATE notification SET read = 1 WHERE user_id = ? AND read = 0",
        user_id);
    return listForUser(user_id, 0, 0);
}

Json::Value NotificationsStore::markRead(int64_t user_id, int64_t notification_id) const
{
    m_client->execSqlSync(
        "UPDATE notification SET read = 1 WHERE user_id = ? AND id = ?",
        user_id,
        notification_id);
    auto rows = m_client->execSqlSync(
        R"SQL(
SELECT id, type, message, read, created_at
FROM notification
WHERE user_id = ? AND id = ?
)SQL",
        user_id,
        notification_id);
    if (rows.empty())
        return Json::Value();

    const auto& row = rows[0];
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["type"] = row["type"].as<std::string>();
    item["message"] = row["message"].as<std::string>();
    item["createdAt"] = to_created_at_iso(row["created_at"].as<std::string>());
    item["read"] = row["read"].as<int>() != 0;
    return item;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
