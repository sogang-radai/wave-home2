#include "insight_vec_store.h"

#include <cstring>
#include <unordered_map>

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    std::vector<char> pack_embedding_blob(const std::vector<float>& values)
    {
        std::vector<char> blob(values.size() * sizeof(float));
        if (!values.empty())
            std::memcpy(blob.data(), values.data(), blob.size());
        return blob;
    }
}

const char* vecTableForInsightSurface(const std::string& surface)
{
    static const std::unordered_map<std::string, const char*> k_map = {
        {"dashboard_banner", "vec_insight_dashboard"},
        {"weekly_plan", "vec_insight_weekly_plan"},
        {"sleep_report", "vec_insight_sleep"},
        {"posture_report", "vec_insight_posture"},
        {"power", "vec_insight_power"},
    };
    const auto it = k_map.find(surface);
    return it == k_map.end() ? nullptr : it->second;
}

InsightVecStore::InsightVecStore(const db::DbClientPtr& client) :
    m_client(client)
{
}

bool InsightVecStore::tableExists(const std::string& name) const
{
    if (!m_client || name.empty())
        return false;

    try
    {
        const auto rows = m_client->execSqlSync(
            "SELECT 1 FROM sqlite_master WHERE type IN ('table', 'virtual table') AND name = ? LIMIT 1",
            name);
        return !rows.empty();
    }
    catch (const std::exception&)
    {
        return false;
    }
}

void InsightVecStore::deleteEmbeddings(
    const std::string& surface,
    const std::vector<int64_t>& insight_ids)
{
    const char* vec_table = vecTableForInsightSurface(surface);
    if (!m_client || !vec_table || insight_ids.empty() || !tableExists(vec_table))
        return;

    try
    {
        for (const int64_t insight_id : insight_ids)
        {
            m_client->execSqlSync(
                std::string("DELETE FROM ") + vec_table + " WHERE insight_id = ?",
                insight_id);
        }
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("insight embedding delete failed ({}): {}", vec_table, e.what());
    }
}

void InsightVecStore::storeEmbedding(
    const std::string& surface,
    int64_t insight_id,
    const std::vector<float>& embedding)
{
    const char* vec_table = vecTableForInsightSurface(surface);
    if (!m_client || !vec_table || embedding.empty())
        return;

    if (!tableExists(vec_table))
    {
        WLOG_WARN("insight embedding store skipped — missing {}", vec_table);
        return;
    }

    try
    {
        const auto blob = pack_embedding_blob(embedding);
        m_client->execSqlSync(
            std::string("DELETE FROM ") + vec_table + " WHERE insight_id = ?",
            insight_id);
        m_client->execSqlSync(
            std::string("INSERT INTO ") + vec_table + " (insight_id, embedding) VALUES (?, ?)",
            insight_id,
            blob);
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("insight embedding store failed (id={}, {}): {}", insight_id, vec_table, e.what());
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
