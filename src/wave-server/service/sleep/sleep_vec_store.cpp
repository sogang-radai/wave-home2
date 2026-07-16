#include "sleep_vec_store.h"
#include "../../db/database.h"

#include <cstring>

#include "../../core/logger.h"

WAVE_NAMESPACE_BEGIN
SERVICE_NAMESPACE_BEGIN

namespace
{
    std::string packEmbeddingBlob(const std::vector<float>& values)
    {
        std::string blob;
        blob.resize(values.size() * sizeof(float));
        if (!values.empty())
            std::memcpy(blob.data(), values.data(), blob.size());
        return blob;
    }
}

SleepVecStore::SleepVecStore(const db::DbClientPtr& client) :
    m_client(client)
{
}

bool SleepVecStore::tableExists(const std::string& name) const
{
    if (!m_client)
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

void SleepVecStore::storeSleepStatEmbedding(int64_t stat_id, const std::vector<float>& embedding)
{
    if (!m_client || embedding.empty())
        return;

    try
    {
        if (tableExists("vec_sleep_stat"))
        {
            const std::string blob = packEmbeddingBlob(embedding);
            m_client->execSqlSync("DELETE FROM vec_sleep_stat WHERE stat_id = ?", stat_id);
            m_client->execSqlSync(
                "INSERT INTO vec_sleep_stat (stat_id, embedding) VALUES (?, ?)",
                stat_id,
                blob);
            return;
        }

        if (tableExists("sleep_stat_embedding"))
        {
            const std::string blob = packEmbeddingBlob(embedding);
            m_client->execSqlSync(
                R"SQL(
INSERT INTO sleep_stat_embedding (stat_id, dim, embedding_blob, updated_at)
VALUES (?, ?, ?, datetime('now'))
ON CONFLICT(stat_id) DO UPDATE SET
    dim = excluded.dim,
    embedding_blob = excluded.embedding_blob,
    updated_at = excluded.updated_at
)SQL",
                stat_id,
                static_cast<int64_t>(embedding.size()),
                blob);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep stat embedding store failed: {}", e.what());
    }
}

void SleepVecStore::storeSleepReportEmbedding(int64_t report_id, const std::vector<float>& embedding)
{
    if (!m_client || embedding.empty())
        return;

    try
    {
        if (tableExists("vec_sleep_report"))
        {
            const std::string blob = packEmbeddingBlob(embedding);
            m_client->execSqlSync("DELETE FROM vec_sleep_report WHERE report_id = ?", report_id);
            m_client->execSqlSync(
                "INSERT INTO vec_sleep_report (report_id, embedding) VALUES (?, ?)",
                report_id,
                blob);
            return;
        }

        if (tableExists("sleep_report_embedding"))
        {
            const std::string blob = packEmbeddingBlob(embedding);
            m_client->execSqlSync(
                R"SQL(
INSERT INTO sleep_report_embedding (report_id, dim, embedding_blob, updated_at)
VALUES (?, ?, ?, datetime('now'))
ON CONFLICT(report_id) DO UPDATE SET
    dim = excluded.dim,
    embedding_blob = excluded.embedding_blob,
    updated_at = excluded.updated_at
)SQL",
                report_id,
                static_cast<int64_t>(embedding.size()),
                blob);
        }
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep report embedding store failed: {}", e.what());
    }
}

SERVICE_NAMESPACE_END
WAVE_NAMESPACE_END
