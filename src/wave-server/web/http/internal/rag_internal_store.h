#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

struct RagSearchConfig
{
    std::string agent_base_url = "http://127.0.0.1:8502";
    std::string embedding_model = "nomic-embed-text";
};

class RagInternalStore
{
public:
    explicit RagInternalStore(db::DbClientPtr client, RagSearchConfig config = {});

    Json::Value search(const Json::Value& request, std::string& error, std::string& field) const;

private:
    db::DbClientPtr m_client;
    RagSearchConfig m_config;

    bool embedQuery(const std::string& query, std::vector<float>& out_embedding, std::string& out_error) const;
    bool tableExists(const std::string& name) const;

    Json::Value searchTarget(
        const Json::Value& target,
        const std::vector<float>* query_embedding) const;

    /** Returns hits; ok=true only when the vec query executed without error. */
    Json::Value searchVecTable(
        const std::string& vec_table,
        const std::string& id_column,
        const std::vector<float>& query_embedding,
        int top_k,
        bool& ok) const;

    Json::Value searchSleepStat(const Json::Value& target, const std::vector<float>* query_embedding) const;
    Json::Value searchSleepReport(const Json::Value& target, const std::vector<float>* query_embedding) const;
    Json::Value searchPowerReport(const Json::Value& target, const std::vector<float>* query_embedding) const;
    Json::Value searchInsightSurface(
        const Json::Value& target,
        const std::string& collection,
        const std::string& surface,
        const std::vector<float>* query_embedding) const;
    Json::Value searchPostureReport(const Json::Value& target) const;
    Json::Value searchWeeklyPlanReport(const Json::Value& target) const;

    static std::optional<int64_t> target_int(const Json::Value& target, const char* key);
    static std::optional<std::string> target_string(const Json::Value& target, const char* key);
    static int target_top_k(const Json::Value& target);
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
