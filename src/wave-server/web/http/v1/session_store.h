#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

Json::Value makeError(const std::string& code, const std::string& message, int status = 400);
void respondError(
    const std::function<void(const drogon::HttpResponsePtr&)>& callback,
    int status,
    const std::string& code,
    const std::string& message);

struct AccountView
{
    int64_t id = 0;
    std::string name;
};

struct SessionView
{
    int64_t session_id = 0;
    std::optional<AccountView> active_account;
};

class SessionStore
{
public:
    explicit SessionStore(drogon::orm::DbClientPtr client);

    SessionView resolveSession(const drogon::HttpRequestPtr& req) const;
    Json::Value sessionJson(const SessionView& session) const;

    bool setActiveAccount(int64_t session_id, int64_t account_id, std::string& error);
    std::vector<AccountView> listAccounts() const;

private:
    drogon::orm::DbClientPtr m_client;
    int64_t resolveSessionId(const drogon::HttpRequestPtr& req) const;
    std::optional<AccountView> findAccount(int64_t id) const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
