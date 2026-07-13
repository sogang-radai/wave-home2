#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

/** Matches wave-home-front AiAgentSettings PROMPT_LIMIT (Unicode code points). */
inline constexpr size_t kPersonalPromptMaxChars = 10000;

class SessionStore;

class SettingsStore
{
public:
    explicit SettingsStore(drogon::orm::DbClientPtr client);

    std::optional<int64_t> resolveActiveUserId(const SessionStore& sessions, const drogon::HttpRequestPtr& req) const;

    Json::Value defaultGeneralSettings() const;
    Json::Value defaultSleepConfig() const;

    Json::Value getGeneralSettings(int64_t user_id) const;
    bool putGeneralSettings(int64_t user_id, const Json::Value& body, Json::Value& out, std::string& error, std::string& field);

    Json::Value getSleepConfig(int64_t user_id) const;
    bool putSleepConfig(int64_t user_id, const Json::Value& body, Json::Value& out, std::string& error, std::string& field);

    Json::Value listSounds() const;
    Json::Value listTtsSpeakers() const;

    Json::Value listAiModels() const;
    Json::Value getAiAgentSettings(int64_t user_id) const;
    bool putAiAgentSettings(int64_t user_id, const Json::Value& body, Json::Value& out, std::string& error, std::string& field);

private:
    drogon::orm::DbClientPtr m_client;

    Json::Value loadJsonColumn(const char* table, const char* column, int64_t user_id) const;
    bool upsertJsonColumn(
        const char* table,
        const char* column,
        int64_t user_id,
        const Json::Value& value) const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
