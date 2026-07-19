#include "insights_store.h"
#include "../../../db/database.h"

#include <sstream>

#include "../../../app/app_state.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    bool tableExists(db::DbClientPtr client, const char* name)
    {
        const auto rows = client->execSqlSync(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
            name);
        return !rows.empty();
    }
}

InsightsStore::InsightsStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

std::string InsightsStore::reference_date(db::DbClientPtr client)
{
    const auto& state = AppState::get();
    if (state.demo_mode && !state.anchor_date.empty())
        return state.anchor_date;

    const auto rows = client->execSqlSync("SELECT date('now', 'localtime') AS d");
    if (rows.empty())
        return {};
    return rows[0]["d"].as<std::string>();
}

Json::Value InsightsStore::parse_json_column(const drogon::orm::Field& field)
{
    if (field.isNull())
        return Json::nullValue;

    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    const auto text = field.as<std::string>();
    std::istringstream stream(text);
    if (Json::parseFromStream(builder, stream, &parsed, &errors))
        return parsed;
    return Json::nullValue;
}

Json::Value InsightsStore::rowToJson(const drogon::orm::Row& row) const
{
    Json::Value item;
    item["id"] = static_cast<Json::Int64>(row["id"].as<int64_t>());
    item["surface"] = row["surface"].as<std::string>();
    item["kind"] = row["kind"].as<std::string>();
    item["date"] = row["date"].as<std::string>();
    if (row["label"].isNull())
        item["label"] = Json::nullValue;
    else
        item["label"] = row["label"].as<std::string>();
    item["title"] = row["title"].as<std::string>();
    item["text"] = row["text"].as<std::string>();
    item["actionable"] = row["actionable"].as<int>() != 0;
    if (row["action_type"].isNull())
        item["actionType"] = Json::nullValue;
    else
        item["actionType"] = row["action_type"].as<std::string>();
    item["approved"] = row["approved"].as<int>() != 0;
    item["ruleJson"] = parse_json_column(row["rule_json"]);
    item["scheduleTaskJson"] = parse_json_column(row["schedule_task_json"]);
    if (!row["created_at"].isNull())
    {
        const auto created = row["created_at"].as<std::string>();
        if (created.size() >= 19)
            item["createdAt"] = created.substr(0, 10) + "T" + created.substr(11, 8) + "+09:00";
        else
            item["createdAt"] = created;
    }
    return item;
}

Json::Value InsightsStore::list(
    int64_t user_id,
    const std::optional<std::string>& surface,
    const std::optional<std::string>& date,
    const std::optional<std::string>& kind,
    const std::optional<bool>& approved,
    const std::optional<bool>& actionable) const
{
    Json::Value items(Json::arrayValue);
    if (!tableExists(m_client, "insight"))
        return items;

    std::ostringstream sql;
    sql << "SELECT id, surface, kind, date, label, title, text, actionable, action_type,"
        << " approved, rule_json, schedule_task_json, created_at"
        << " FROM insight WHERE user_id = " << user_id;
    if (surface)
        sql << " AND surface = '" << *surface << "'";
    if (date)
        sql << " AND date = '" << *date << "'";
    if (kind)
        sql << " AND kind = '" << *kind << "'";
    if (approved)
        sql << " AND approved = " << (*approved ? 1 : 0);
    if (actionable)
        sql << " AND actionable = " << (*actionable ? 1 : 0);
    sql << " ORDER BY created_at ASC, id ASC";

    for (const auto& row : m_client->execSqlSync(sql.str()))
        items.append(rowToJson(row));
    return items;
}

Json::Value InsightsStore::getById(int64_t user_id, int64_t insight_id) const
{
    if (!tableExists(m_client, "insight"))
        return Json::nullValue;

    const auto rows = m_client->execSqlSync(
        "SELECT id, surface, kind, date, label, title, text, actionable, action_type,"
        " approved, rule_json, schedule_task_json, created_at"
        " FROM insight WHERE user_id = ? AND id = ? LIMIT 1",
        user_id,
        insight_id);
    if (rows.empty())
        return Json::nullValue;
    return rowToJson(rows[0]);
}

bool InsightsStore::markApplied(
    int64_t user_id,
    int64_t insight_id,
    const std::optional<Json::Value>& rule_json_override) const
{
    if (rule_json_override)
    {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        const auto serialized = Json::writeString(builder, *rule_json_override);
        const auto rows = m_client->execSqlSync(
            "UPDATE insight SET approved = 1, rule_json = ? WHERE id = ? AND user_id = ?",
            serialized,
            insight_id,
            user_id);
        return rows.affectedRows() > 0;
    }

    const auto rows = m_client->execSqlSync(
        "UPDATE insight SET approved = 1 WHERE id = ? AND user_id = ?",
        insight_id,
        user_id);
    return rows.affectedRows() > 0;
}

bool InsightsStore::markCanceled(int64_t user_id, int64_t insight_id) const
{
    const auto rows = m_client->execSqlSync(
        "UPDATE insight SET approved = 0 WHERE id = ? AND user_id = ?",
        insight_id,
        user_id);
    return rows.affectedRows() > 0;
}

Json::Value InsightsStore::dashboardDailyMessage(int64_t user_id) const
{
    if (!tableExists(m_client, "insight"))
        return Json::nullValue;

    const auto ref_date = reference_date(m_client);
    const auto rows = m_client->execSqlSync(
        "SELECT title, text FROM insight"
        " WHERE user_id = ? AND surface = 'dashboard_banner' AND date <= ?"
        " ORDER BY date DESC, id DESC LIMIT 1",
        user_id,
        ref_date);
    if (rows.empty())
        return Json::nullValue;

    Json::Value out;
    out["headline"] = rows[0]["title"].as<std::string>();
    out["body"] = rows[0]["text"].as<std::string>();
    return out;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
