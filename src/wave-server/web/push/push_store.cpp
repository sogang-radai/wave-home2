#include "push_store.h"
#include "../../db/database.h"

#include "../../core/logger.h"
#include "util/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace push {

bool upsertSubscription(
    const db::DbClientPtr& client,
    int64_t session_id,
    const Subscription& subscription)
{
    if (!client || subscription.endpoint.empty())
        return false;

    const auto now = formatTimestamp();
    try
    {
        client->execSqlSync(
            "DELETE FROM push_subscription WHERE session_id = ?",
            session_id);

        client->execSqlSync(
            "INSERT INTO push_subscription (session_id, endpoint, p256dh, auth_key, created_at, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?)",
            session_id,
            subscription.endpoint,
            subscription.p256dh,
            subscription.auth,
            now,
            now);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to save push subscription: {}", e.what());
        return false;
    }
}

bool deleteSubscriptions(const db::DbClientPtr& client, int64_t session_id)
{
    if (!client)
        return false;

    try
    {
        client->execSqlSync("DELETE FROM push_subscription WHERE session_id = ?", session_id);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to delete push subscription: {}", e.what());
        return false;
    }
}

std::vector<Subscription> listSubscriptions(
    const db::DbClientPtr& client,
    int64_t session_id)
{
    std::vector<Subscription> subscriptions;
    if (!client)
        return subscriptions;

    try
    {
        auto rows = client->execSqlSync(
            "SELECT endpoint, p256dh, auth_key FROM push_subscription WHERE session_id = ?",
            session_id);
        subscriptions.reserve(rows.size());
        for (const auto& row : rows)
        {
            Subscription sub;
            sub.endpoint = row["endpoint"].as<std::string>();
            sub.p256dh = row["p256dh"].as<std::string>();
            sub.auth = row["auth_key"].as<std::string>();
            subscriptions.push_back(std::move(sub));
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to list push subscriptions: {}", e.what());
    }

    return subscriptions;
}

} // namespace push
} // namespace web
WAVE_NAMESPACE_END
