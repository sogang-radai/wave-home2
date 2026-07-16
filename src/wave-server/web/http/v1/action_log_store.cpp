#include "action_log_store.h"
#include "../../../db/database.h"

#include "util/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

ActionLogStore::ActionLogStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t ActionLogStore::nextId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM user_action_log");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

void ActionLogStore::record(
    int64_t user_id,
    const std::string& action_type,
    const std::string& ref_type,
    int64_t ref_id,
    const std::optional<std::string>& category,
    const std::optional<Json::Value>& metadata) const
{
    std::optional<std::string> metadata_json;
    if (metadata)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        metadata_json = Json::writeString(builder, *metadata);
    }

    m_client->execSqlSync(
        R"SQL(
INSERT INTO user_action_log (id, user_id, action_type, ref_type, ref_id, occurred_at, category, metadata_json)
VALUES (?, ?, ?, ?, ?, ?, ?, ?)
)SQL",
        nextId(),
        user_id,
        action_type,
        ref_type,
        ref_id,
        formatTimestamp(),
        category,
        metadata_json);
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
