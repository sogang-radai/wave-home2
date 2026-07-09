#include "rag_internal_store.h"

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

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {
namespace
{
    constexpr double kEmbedTimeoutSeconds = 60.0;

    std::string packEmbedding(const std::vector<float>& values)
    {
        std::string blob;
        blob.resize(values.size() * sizeof(float));
        if (!values.empty())
            std::memcpy(blob.data(), values.data(), blob.size());
        return blob;
    }

    Json::Value makeHit(int64_t ref_id, double score, const std::string& text)
    {
        Json::Value hit;
        hit["refId"] = static_cast<Json::Int64>(ref_id);
        hit["score"] = score;
        hit["text"] = text;
        return hit;
    }

    std::string joinInsightText(const drogon::orm::Row& row)
    {
        const std::string title = row["title"].as<std::string>();
        if (row["text"].isNull())
            return title;
        return title + "\n" + row["text"].as<std::string>();
    }

    std::string sqlEscape(std::string_view value)
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

RagInternalStore::RagInternalStore(drogon::orm::DbClientPtr client, RagSearchConfig config) :
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
        LOG_WARN("RAG query embedding failed (text fallback only): {}", embed_error);

    const std::vector<float>* embedding_ptr = has_embedding ? &query_embedding : nullptr;

    Json::Value results(Json::arrayValue);
    for (const auto& target : request["targets"])
    {
        Json::Value resolved = target;
        if (AppState::get().demo_mode && !targetInt(target, "userId"))
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
    Json::Value input(Json::arrayValue);
    input.append(query);
    payload["input"] = input;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const std::string body = Json::writeString(builder, payload);

    auto client = drogon::HttpClient::newHttpClient(m_config.agent_base_url, nullptr, false, false);

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/llm/v1/embeddings");
    req->setBody(body);
    req->addHeader("Content-Type", "application/json");

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

std::optional<int64_t> RagInternalStore::targetInt(const Json::Value& target, const char* key)
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

std::optional<std::string> RagInternalStore::targetString(const Json::Value& target, const char* key)
{
    if (!target.isMember(key) || !target[key].isString())
        return std::nullopt;
    return target[key].asString();
}

int RagInternalStore::targetTopK(const Json::Value& target)
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
    int top_k) const
{
    Json::Value hits(Json::arrayValue);
    if (!tableExists(vec_table))
        return hits;

    try
    {
        const std::string blob = packEmbedding(query_embedding);
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
    }
    catch (const std::exception& e)
    {
        LOG_WARN("vec search failed for {}: {}", vec_table, e.what());
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

    const auto user_id = targetInt(target, "userId");
    if (!user_id)
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = targetTopK(target);

    if (query_embedding && tableExists("vec_sleep_stat"))
    {
        hits = searchVecTable("vec_sleep_stat", "stat_id", *query_embedding, top_k);
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

    if (!tableExists("sleep_stat"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, summary_text FROM sleep_stat WHERE user_id = " << *user_id
        << " AND granularity = '30m' AND summary_text IS NOT NULL AND summary_text != ''";
    if (const auto from = targetString(target, "from"))
        sql << " AND time_start >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND time_start < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY time_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, row["summary_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_stat text search failed: {}", e.what());
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

    const auto user_id = targetInt(target, "userId");
    if (!user_id)
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = targetTopK(target);

    if (query_embedding && tableExists("vec_sleep_report"))
    {
        hits = searchVecTable("vec_sleep_report", "report_id", *query_embedding, top_k);
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

    if (!tableExists("sleep_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, report_text FROM sleep_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto period = targetString(target, "period"))
        sql << " AND period = '" << sqlEscape(*period) << "'";
    if (const auto from = targetString(target, "from"))
        sql << " AND period_start >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND period_start < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("sleep_report text search failed: {}", e.what());
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

    const int top_k = targetTopK(target);

    if (query_embedding && tableExists("vec_power_report"))
    {
        hits = searchVecTable("vec_power_report", "report_id", *query_embedding, top_k);
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

    if (!tableExists("power_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    std::ostringstream sql;
    sql << "SELECT id, report_text FROM power_report WHERE report_text IS NOT NULL AND report_text != ''";

    if (target.isMember("deviceId") && !target["deviceId"].isNull())
    {
        if (const auto device_id = targetInt(target, "deviceId"))
            sql << " AND device_id = " << *device_id;
        else
            sql << " AND device_id IS NULL";
    }
    else if (const auto user_id = targetInt(target, "userId"))
    {
        sql << " AND device_id IN (SELECT device_id FROM device_user_map WHERE user_id = " << *user_id << ")";
    }

    if (const auto period = targetString(target, "period"))
        sql << " AND period = '" << sqlEscape(*period) << "'";
    if (const auto from = targetString(target, "from"))
        sql << " AND period_start >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND period_start < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("power_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchInsightSurface(
    const Json::Value& target,
    const std::string& collection,
    const std::string& surface) const
{
    Json::Value entry;
    entry["collection"] = collection;
    Json::Value hits(Json::arrayValue);

    const auto user_id = targetInt(target, "userId");
    if (!user_id || !tableExists("insight"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = targetTopK(target);
    std::ostringstream sql;
    sql << "SELECT id, title, text FROM insight WHERE user_id = " << *user_id
        << " AND surface = '" << sqlEscape(surface) << "'";
    if (const auto date = targetString(target, "date"))
        sql << " AND date = '" << sqlEscape(*date) << "'";
    if (const auto from = targetString(target, "from"))
        sql << " AND date >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND date < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY date DESC, id DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, joinInsightText(row)));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("insight search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchPostureReport(const Json::Value& target) const
{
    Json::Value entry;
    entry["collection"] = "posture_report";
    Json::Value hits(Json::arrayValue);

    const auto user_id = targetInt(target, "userId");
    if (!user_id || !tableExists("posture_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = targetTopK(target);
    std::ostringstream sql;
    sql << "SELECT id, report_text FROM posture_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto period = targetString(target, "period"))
        sql << " AND period = '" << sqlEscape(*period) << "'";
    if (const auto from = targetString(target, "from"))
        sql << " AND period_start >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND period_start < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("posture_report text search failed: {}", e.what());
    }

    entry["hits"] = hits;
    return entry;
}

Json::Value RagInternalStore::searchWeeklyPlanReport(const Json::Value& target) const
{
    Json::Value entry;
    entry["collection"] = "weekly_plan_report";
    Json::Value hits(Json::arrayValue);

    const auto user_id = targetInt(target, "userId");
    if (!user_id || !tableExists("weekly_plan_report"))
    {
        entry["hits"] = hits;
        return entry;
    }

    const int top_k = targetTopK(target);
    std::ostringstream sql;
    sql << "SELECT id, report_text FROM weekly_plan_report WHERE user_id = " << *user_id
        << " AND report_text IS NOT NULL AND report_text != ''";
    if (const auto from = targetString(target, "from"))
        sql << " AND period_start >= '" << sqlEscape(*from) << "'";
    if (const auto to = targetString(target, "to"))
        sql << " AND period_start < '" << sqlEscape(*to) << "'";
    sql << " ORDER BY period_start DESC LIMIT " << top_k;

    try
    {
        for (const auto& row : m_client->execSqlSync(sql.str()))
            hits.append(makeHit(row["id"].as<int64_t>(), 0.5, row["report_text"].as<std::string>()));
    }
    catch (const std::exception& e)
    {
        LOG_WARN("weekly_plan_report text search failed: {}", e.what());
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
        return searchInsightSurface(target, collection, insight_it->second);

    Json::Value entry;
    entry["collection"] = collection;
    entry["hits"] = Json::Value(Json::arrayValue);
    return entry;
}

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
