#include "rag_internal_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include "../../../app/app_state.h"
#include "../../../core/logger.h"
#include "../../../service/agent/insight_vec_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    constexpr double kEmbedTimeoutSeconds = 60.0;

    std::string pack_embedding(const std::vector<float>& values)
    {
        // sqlite-vec MATCH accepts JSON arrays; Drogon binds std::string as
        // TEXT, so a float32 blob would be mis-parsed as JSON.
        std::ostringstream stream;
        stream << '[';
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0)
                stream << ',';
            stream << values[i];
        }
        stream << ']';
        return stream.str();
    }

    Json::Value make_hit(int64_t ref_id, double score, const std::string& text)
    {
        Json::Value hit;
        hit["refId"] = static_cast<Json::Int64>(ref_id);
        hit["score"] = score;
        hit["text"] = text;
        return hit;
    }

    std::string join_insight_text(const drogon::orm::Row& row)
    {
        const std::string title = row["title"].as<std::string>();
        if (row["text"].isNull())
            return title;
        return title + "\n" + row["text"].as<std::string>();
    }

    std::string sql_escape(std::string_view value)
    {
        std::string out;
        out.reserve(value.size());
        for (char ch : value)
        {
            if (ch == '\'')
                out += "''";
            else
                out += ch;
        }
        return out;
    }
}

RagInternalStore::RagInternalStore(db::DbClientPtr client, RagSearchConfig config) :
    m_client(std::move(client)),
    m_config(std::move(config))
{
    while (!m_config.agent_base_url.empty() && m_config.agent_base_url.back() == '/')
        m_config.agent_base_url.pop_back();
}

Json::Value RagInternalStore::search(const Json::Value& request, std::string& error, std::string& field) const
{
    if (!request.isObject() || !request.isMember("query") || !request["query"].isString() ||
        request["query"].asString().empty())
    {
        error = "query 가 필요합니다.";
        field = "query";
        return Json::Value();
    }

    if (!request.isMember("targets") || !request["targets"].isArray() || request["targets"].empty())
    {
        error = "targets 배열이 필요합니다.";
        field = "targets";
        return Json::Value();
    }

    if (!m_client)
    {
        error = "데이터베이스를 사용할 수 없습니다.";
        field = "targets";
        return Json::Value();
    }

    const std::string query = request["query"].asString();
    std::vector<float> query_embedding;
    std::string embed_error;
    const bool has_embedding = embedQuery(query, query_embedding, embed_error);
    if (!has_embedding)
        WLOG_WARN("RAG query embedding failed (text fallback only): {}", embed_error);

    const std::vector<float>* embedding_ptr = has_embedding ? &query_embedding : nullptr;

    Json::Value results(Json::arrayValue);
    for (const auto& target : request["targets"])
    {
        Json::Value resolved = target;
        if (AppState::get().demo_mode && !target_int(target, "userId"))
        {
            const bool has_device = target.isMember("deviceId") && !target["deviceId"].isNull();
            if (!has_device && target.isMember("collection") && target["collection"].isString())
            {
                static const std::unordered_set<std::string> kUserCollections = {
                    "sleep_stat",
                    "sleep_report",
                    "posture_report",
                    "weekly_plan_report",
                    "insight_dashboard",
                    "insight_weekly_plan",
                    "insight_sleep",
                    "insight_posture",
                    "insight_power",
                    "power_report",
                };
                if (kUserCollections.count(target["collection"].asString()) > 0)
                    resolved["userId"] = static_cast<Json::Int64>(1);
            }
        }
        results.append(searchTarget(resolved, embedding_ptr));
    }

    Json::Value body;
    body["results"] = results;
    return body;
}

bool RagInternalStore::embedQuery(
    const std::string& query,
    std::vector<float>& out_embedding,
    std::string& out_error) const
{
    if (m_config.agent_base_url.empty())
    {
        out_error = "agent base_url is not configured";
        return false;
    }

    Json::Value payload;
    payload["model"] = m_config.embedding_model;
    payload["input"] = query;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string body = Json::writeString(builder, payload);

    auto client = drogon::HttpClient::newHttpClient(m_config.agent_base_url, nullptr, false, false);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/llm/v1/embeddings");
    req->setBody(body);
    req->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    std::promise<std::pair<drogon::ReqResult, drogon::HttpResponsePtr>> done;
    auto future = done.get_future();
    client->sendRequest(
        req,
        [&done](drogon::ReqResult result, const drogon::HttpResponsePtr& response)
        {
            done.set_value({result, response});
        });

    const auto wait = future.wait_for(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(kEmbedTimeoutSeconds)));
    if (wait != std::future_status::ready)
    {
        out_error = "embedding request timed out";
        return false;
    }

    const auto [result, response] = future.get();
    if (result != drogon::ReqResult::Ok || !response)
    {
        out_error = "embedding request failed";
        return false;
    }
    if (response->getStatusCode() >= 400)
    {
        out_error = "embedding HTTP " + std::to_string(response->getStatusCode()) + ": " +
            std::string(response->body());
        return false;
    }

    Json::Value parsed;
    Json::CharReaderBuilder reader;
    std::string parse_error;
    const std::string response_body = std::string(response->body());
    std::istringstream stream(response_body);
    if (!Json::parseFromStream(reader, stream, &parsed, &parse_error))
    {
        out_error = "embedding response parse error: " + parse_error;
        return false;
    }

    const Json::Value* row = nullptr;
    if (parsed.isMember("data") && parsed["data"].isArray() && !parsed["data"].empty() &&
        parsed["data"][0].isMember("embedding"))
    {
        row = &parsed["data"][0]["embedding"];
    }
    else if (parsed.isMember("embeddings") && parsed["embeddings"].isArray() && !parsed["embeddings"].empty())
    {
        row = &parsed["embeddings"][0];
    }

    if (!row || !row->isArray() || row->empty())
    {
        out_error = "embedding response missing vector";
        return false;
    }

    out_embedding.clear();
    out_embedding.reserve(row->size());
    for (const auto& value : *row)
        out_embedding.push_back(static_cast<float>(value.asDouble()));
    return !out_embedding.empty();
}

bool RagInternalStore::tableExists(const std::string& name) const
{
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

std::optional<int64_t> RagInternalStore::target_int(const Json::Value& target, const char* key)
{
    if (!target.isMember(key))
        return std::nullopt;
    const auto& value = target[key];
    if (value.isInt64())
        return value.asInt64();
    if (value.isInt())
        return value.asInt();
    if (value.isUInt())
        return static_cast<int64_t>(value.asUInt());
    return std::nullopt;
}

std::optional<std::string> RagInternalStore::target_string(const Json::Value& target, const char* key)
{
    if (!target.isMember(key) || !target[key].isString())
        return std::nullopt;
    return target[key].asString();
}

int RagInternalStore::target_top_k(const Json::Value& target)
{
    int top_k = 3;
    if (target.isMember("topK") && target["topK"].isInt())
        top_k = target["topK"].asInt();
    return std::max(1, std::min(top_k, 20));
}

Json::Value RagInternalStore::searchVecTable(
    const std::string& vec_table,
    const std::string& id_column,
    const std::vector<float>& query_embedding,
    int top_k,
    bool& ok) const
{
    ok = false;
    Json::Value hits(Json::arrayValue);
    if (!tableExists(vec_table))
        return hits;

    try
    {
        const std::string blob = pack_embedding(query_embedding);
        std::ostringstream sql;
        sql << "SELECT " << id_column << ", distance "
            << "FROM " << vec_table << " "
            << "WHERE embedding MATCH ? AND k = " << top_k;

        const auto rows = m_client->execSqlSync(sql.str(), blob);
        for (const auto& row : rows)
        {
            const int64_t ref_id = row[id_column].as<int64_t>();
            const double distance = row["distance"].as<double>();
            const double score = std::max(0.0, std::min(1.0, 1.0 - distance));
            Json::Value hit;
            hit["refId"] = static_cast<Json::Int64>(ref_id);
            hit["score"] = score;
            hit["text"] = "";
            hits.append(hit);
        }
        ok = true;
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("vec search failed for {} (falling back to text): {}", vec_table, e.what());
    }
    return hits;
}

Json::Value RagInternalStore::searchSleepStat(
    const Json::Value& target,
    const std::vector<float>* query_embedding) const
{
    Json::Value entry;
    entry["collection"] = "sleep_stat";
    Json::Value hits(Json::arrayValue);

    const auto user_id = target_int(target, "userId");
    if (!user_id)
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = target_top_k(target);

    if (query_embedding && tableExists("vec_sleep_stat"))
    {
        bool vec_ok = false;
        hits = searchVecTable("vec_sleep_stat", "stat_id", *query_embedding, top_k, vec_ok);
        if (vec_ok && !hits.empty())
        {
            for (auto& hit : hits)
            {
                if (!hit.isMember("refId"))
                    continue;
                const int64_t ref_id = hit["refId"].asInt64();
                try
                {
                    const auto rows = m_client->execSqlSync(
                        "SELECT summary_text FROM sleep_stat WHERE id = ? AND summary_text IS NOT NULL LIMIT 1",
                        ref_id);
                    if (!rows.empty() && !rows[0]["summary_text"].isNull())
                        hit["text"] = rows[0]["summary_text"].as<std::string>();
                }
                catch (const std::exception&)
                {
                }
            }
            entry["hits"] = hits;
            return entry;
        }
        hits = Json::Value(Json::arrayValue);
    }

    if (!tableExists("sleep_stat"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, summary_text FROM sleep_stat WHERE user_id = " << *user_id
        << " AND granularity = '30m' AND summary_text IS NOT NULL AND summary_text != ''";
    if (const auto from = target_string(target, "from"))
        sql << " AND time_start >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND time_start < '" << sql_escape(*to) << "'";
    sql << " ORDER BY time_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, row["summary_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("sleep_stat text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchSleepReport(
    const Json::Value& target,
    const std::vector<float>* query_embedding) const
{
    Json::Value entry;
    entry["collection"] = "sleep_report";
    Json::Value hits(Json::arrayValue);

    const auto user_id = target_int(target, "userId");
    if (!user_id)
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = target_top_k(target);

    if (query_embedding && tableExists("vec_sleep_report"))
    {
        bool vec_ok = false;
        hits = searchVecTable("vec_sleep_report", "report_id", *query_embedding, top_k, vec_ok);
        if (vec_ok && !hits.empty())
        {
            for (auto& hit : hits)
            {
                const int64_t ref_id = hit["refId"].asInt64();
                try
                {
                    const auto rows = m_client->execSqlSync(
                        "SELECT report_text FROM sleep_report WHERE id = ? AND report_text IS NOT NULL LIMIT 1",
                        ref_id);
                    if (!rows.empty() && !rows[0]["report_text"].isNull())
                        hit["text"] = rows[0]["report_text"].as<std::string>();
                }
                catch (const std::exception&)
                {
                }
            }
            entry["hits"] = hits;
            return entry;
        }
        hits = Json::Value(Json::arrayValue);
    }

    if (!tableExists("sleep_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, report_text FROM sleep_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto period = target_string(target, "period"))
        sql << " AND period = '" << sql_escape(*period) << "'";
    if (const auto from = target_string(target, "from"))
        sql << " AND period_start >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND period_start < '" << sql_escape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("sleep_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchPowerReport(
    const Json::Value& target,
    const std::vector<float>* query_embedding) const
{
    Json::Value entry;
    entry["collection"] = "power_report";
    Json::Value hits(Json::arrayValue);

    const int top_k = target_top_k(target);

    if (query_embedding && tableExists("vec_power_report"))
    {
        bool vec_ok = false;
        hits = searchVecTable("vec_power_report", "report_id", *query_embedding, top_k, vec_ok);
        if (vec_ok && !hits.empty())
        {
            for (auto& hit : hits)
            {
                const int64_t ref_id = hit["refId"].asInt64();
                try
                {
                    const auto rows = m_client->execSqlSync(
                        "SELECT report_text FROM power_report WHERE id = ? AND report_text IS NOT NULL LIMIT 1",
                        ref_id);
                    if (!rows.empty() && !rows[0]["report_text"].isNull())
                        hit["text"] = rows[0]["report_text"].as<std::string>();
                }
                catch (const std::exception&)
                {
                }
            }
            entry["hits"] = hits;
            return entry;
        }
        hits = Json::Value(Json::arrayValue);
    }

    if (!tableExists("power_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, report_text FROM power_report WHERE report_text IS NOT NULL AND report_text != ''";

    if (target.isMember("deviceId") && !target["deviceId"].isNull())
    {
        if (const auto device_id = target_int(target, "deviceId"))
            sql << " AND device_id = " << *device_id;
        else
            sql << " AND device_id IS NULL";
    }
    else if (const auto user_id = target_int(target, "userId"))
    {
        sql << " AND device_id IN (SELECT device_id FROM device_user_map WHERE user_id = " << *user_id << ")";
    }

    if (const auto period = target_string(target, "period"))
        sql << " AND period = '" << sql_escape(*period) << "'";
    if (const auto from = target_string(target, "from"))
        sql << " AND period_start >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND period_start < '" << sql_escape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("power_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchInsightSurface(
    const Json::Value& target,
    const std::string& collection,
    const std::string& surface,
    const std::vector<float>* query_embedding) const
{
    Json::Value entry;
    entry["collection"] = collection;
    Json::Value hits(Json::arrayValue);

    const auto user_id = target_int(target, "userId");
    if (!user_id || !tableExists("insight"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = target_top_k(target);
    const char* vec_table = service::vecTableForInsightSurface(surface);
    if (query_embedding && vec_table && tableExists(vec_table))
    {
        bool vec_ok = false;
        const int candidate_k = std::min(std::max(top_k * 5, top_k), 50);
        auto candidates = searchVecTable(vec_table, "insight_id", *query_embedding, candidate_k, vec_ok);
        if (vec_ok && !candidates.empty())
        {
            for (auto& hit : candidates)
            {
                if (!hit.isMember("refId"))
                    continue;
                const int64_t ref_id = hit["refId"].asInt64();
                try
                {
                    const auto rows = m_client->execSqlSync(
                        "SELECT id, user_id, surface, date, title, text FROM insight WHERE id = ? LIMIT 1",
                        ref_id);
                    if (rows.empty())
                        continue;
                    const auto& row = rows[0];
                    if (row["user_id"].as<int64_t>() != *user_id)
                        continue;
                    if (row["surface"].as<std::string>() != surface)
                        continue;
                    if (const auto date = target_string(target, "date"))
                    {
                        if (row["date"].as<std::string>() != *date)
                            continue;
                    }
                    if (const auto from = target_string(target, "from"))
                    {
                        if (row["date"].as<std::string>() < *from)
                            continue;
                    }
                    if (const auto to = target_string(target, "to"))
                    {
                        if (!(row["date"].as<std::string>() < *to))
                            continue;
                    }
                    hit["text"] = join_insight_text(row);
                    hits.append(hit);
                    if (static_cast<int>(hits.size()) >= top_k)
                        break;
                }
                catch (const std::exception&)
                {
                }
            }

            if (!hits.empty())
            {
                entry["hits"] = hits;
                return entry;
            }
            hits = Json::Value(Json::arrayValue);
        }
    }

    std::ostringstream sql;
    sql << "SELECT id, title, text FROM insight WHERE user_id = " << *user_id
        << " AND surface = '" << sql_escape(surface) << "'";
    if (const auto date = target_string(target, "date"))
        sql << " AND date = '" << sql_escape(*date) << "'";
    if (const auto from = target_string(target, "from"))
        sql << " AND date >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND date < '" << sql_escape(*to) << "'";
    sql << " ORDER BY date DESC, id DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, join_insight_text(row)));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("insight search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchPostureReport(const Json::Value& target) const
{
    Json::Value entry;
    entry["collection"] = "posture_report";
    Json::Value hits(Json::arrayValue);

    const auto user_id = target_int(target, "userId");
    if (!user_id || !tableExists("posture_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = target_top_k(target);
    std::ostringstream sql;
    sql << "SELECT id, report_text FROM posture_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto period = target_string(target, "period"))
        sql << " AND period = '" << sql_escape(*period) << "'";
    if (const auto from = target_string(target, "from"))
        sql << " AND period_start >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND period_start < '" << sql_escape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("posture_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchWeeklyPlanReport(const Json::Value& target) const
{
    Json::Value entry;
    entry["collection"] = "weekly_plan_report";
    Json::Value hits(Json::arrayValue);

    const auto user_id = target_int(target, "userId");
    if (!user_id || !tableExists("weekly_plan_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = target_top_k(target);
    std::ostringstream sql;
    sql << "SELECT id, report_text FROM weekly_plan_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto from = target_string(target, "from"))
        sql << " AND period_start >= '" << sql_escape(*from) << "'";
    if (const auto to = target_string(target, "to"))
        sql << " AND period_start < '" << sql_escape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(make_hit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        WLOG_WARN("weekly_plan_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchTarget(
    const Json::Value& target,
    const std::vector<float>* query_embedding) const
{
    if (!target.isObject() || !target.isMember("collection") || !target["collection"].isString())
    {
        Json::Value entry;
        entry["collection"] = "unknown";
        entry["hits"] = Json::arrayValue;
        return entry;
    }

    const std::string collection = target["collection"].asString();
    static const std::unordered_map<std::string, std::string> kInsightSurfaces = {
        {"insight_dashboard", "dashboard_banner"},
        {"insight_weekly_plan", "weekly_plan"},
        {"insight_sleep", "sleep_report"},
        {"insight_posture", "posture_report"},
        {"insight_power", "power"},
    };

    if (collection == "sleep_stat")
        return searchSleepStat(target, query_embedding);
    if (collection == "sleep_report")
        return searchSleepReport(target, query_embedding);
    if (collection == "power_report")
        return searchPowerReport(target, query_embedding);
    if (collection == "posture_report")
        return searchPostureReport(target);
    if (collection == "weekly_plan_report")
        return searchWeeklyPlanReport(target);

    const auto insight_it = kInsightSurfaces.find(collection);
    if (insight_it != kInsightSurfaces.end())
        return searchInsightSurface(target, collection, insight_it->second, query_embedding);

    Json::Value entry;
    entry["collection"] = collection;
    entry["hits"] = Json::Value(Json::arrayValue);
    return entry;
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
