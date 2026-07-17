#include "vec_tables.h"

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN
DB_NAMESPACE_BEGIN

namespace
{
    struct VecTableSpec
    {
        const char* name;
        const char* create_sql;
    };

    const VecTableSpec k_vec_tables[] = {
        {
            "vec_sleep_stat",
            "CREATE VIRTUAL TABLE vec_sleep_stat USING vec0 ("
            "stat_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_sleep_report",
            "CREATE VIRTUAL TABLE vec_sleep_report USING vec0 ("
            "report_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_power_report",
            "CREATE VIRTUAL TABLE vec_power_report USING vec0 ("
            "report_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_posture_report",
            "CREATE VIRTUAL TABLE vec_posture_report USING vec0 ("
            "report_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_weekly_plan_report",
            "CREATE VIRTUAL TABLE vec_weekly_plan_report USING vec0 ("
            "report_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_insight_dashboard",
            "CREATE VIRTUAL TABLE vec_insight_dashboard USING vec0 ("
            "insight_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_insight_weekly_plan",
            "CREATE VIRTUAL TABLE vec_insight_weekly_plan USING vec0 ("
            "insight_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_insight_sleep",
            "CREATE VIRTUAL TABLE vec_insight_sleep USING vec0 ("
            "insight_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_insight_posture",
            "CREATE VIRTUAL TABLE vec_insight_posture USING vec0 ("
            "insight_id INTEGER PRIMARY KEY, embedding float[768])",
        },
        {
            "vec_insight_power",
            "CREATE VIRTUAL TABLE vec_insight_power USING vec0 ("
            "insight_id INTEGER PRIMARY KEY, embedding float[768])",
        },
    };

    bool table_exists(const DbClientPtr& client, const char* name)
    {
        try
        {
            const auto rows = client->execSqlSync(
                "SELECT 1 FROM sqlite_master WHERE type IN ('table', 'virtual table') AND name = ? LIMIT 1",
                name);
            return !rows.empty();
        }
        catch (const std::exception&)
        {
            return false;
        }
    }
}

bool ensureVecTables(const DbClientPtr& client)
{
    if (!client)
        return false;

    try
    {
        client->execSqlSync("SELECT vec_version()");
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("ensureVecTables skipped (sqlite-vec unavailable): {}", e.what());
        return false;
    }

    int created = 0;
    for (const auto& spec : k_vec_tables)
    {
        if (table_exists(client, spec.name))
            continue;

        try
        {
            client->execSqlSync(spec.create_sql);
            ++created;
            WLOG_INFO("Created vec table {}", spec.name);
        }
        catch (const std::exception& e)
        {
            WLOG_WARN("Failed to create {}: {}", spec.name, e.what());
        }
    }

    if (created > 0)
        WLOG_INFO("ensureVecTables created {} table(s)", created);

    return true;
}

DB_NAMESPACE_END
WAVE_NAMESPACE_END
