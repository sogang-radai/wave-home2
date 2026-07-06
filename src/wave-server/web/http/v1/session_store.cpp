#include "session_store.h"

#include <string_view>

#include "../../../core/logger.h"
#include "../../../core/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

namespace
{
    std::string extractBearerToken(const drogon::HttpRequestPtr& req)
    {
        const auto auth = req->getHeader("Authorization");
        constexpr std::string_view prefix = "Bearer ";
        if (auth.size() <= prefix.size())
            return {};
        if (auth.compare(0, prefix.size(), prefix) != 0)
            return {};
        return auth.substr(prefix.size());
    }
}

Json::Value makeError(const std::string& code, const std::string& message, int /*status*/)
{
    Json::Value root;
    Json::Value err;
    err["code"] = code;
    err["message"] = message;
    root["error"] = err;
    return root;
}

void respondError(
    const std::function<void(const drogon::HttpResponsePtr&)>& callback,
    int status,
    const std::string& code,
    const std::string& message)
{
    auto resp = drogon::HttpResponse::newHttpJsonResponse(makeError(code, message, status));
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    callback(resp);
}

SessionStore::SessionStore(drogon::orm::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t SessionStore::resolveSessionId(const drogon::HttpRequestPtr& req) const
{
    const auto token = extractBearerToken(req);
    if (!token.empty())
    {
        auto rows = m_client->execSqlSync(
            "SELECT id FROM user_session WHERE access_token_hash = ? LIMIT 1",
            token);
        if (!rows.empty())
            return rows[0]["id"].as<int64_t>();
    }

    // Single-household dev fallback: session row 1 when no Bearer token is sent.
    return 1;
}

std::optional<AccountView> SessionStore::findAccount(int64_t id) const
{
    auto rows = m_client->execSqlSync(
        "SELECT id, name FROM user WHERE id = ? LIMIT 1",
        id);
    if (rows.empty())
        return std::nullopt;

    AccountView view;
    view.id = rows[0]["id"].as<int64_t>();
    view.name = rows[0]["name"].as<std::string>();
    return view;
}

SessionView SessionStore::resolveSession(const drogon::HttpRequestPtr& req) const
{
    SessionView view;
    view.session_id = resolveSessionId(req);

    auto rows = m_client->execSqlSync(
        "SELECT active_user_id FROM user_session WHERE id = ? LIMIT 1",
        view.session_id);
    if (rows.empty())
        return view;

    if (rows[0]["active_user_id"].isNull())
        return view;

    view.active_account = findAccount(rows[0]["active_user_id"].as<int64_t>());
    return view;
}

Json::Value SessionStore::sessionJson(const SessionView& session) const
{
    Json::Value body;
    if (session.active_account)
    {
        Json::Value account;
        account["id"] = static_cast<Json::Int64>(session.active_account->id);
        account["name"] = session.active_account->name;
        body["activeAccount"] = account;
    }
    else
    {
        body["activeAccount"] = Json::Value(Json::nullValue);
    }
    return body;
}

bool SessionStore::setActiveAccount(int64_t session_id, int64_t account_id, std::string& error)
{
    if (!findAccount(account_id))
    {
        error = "구성원을 찾을 수 없습니다.";
        return false;
    }

    const auto now = formatTimestamp();
    auto result = m_client->execSqlSync(
        "UPDATE user_session SET active_user_id = ?, updated_at = ? WHERE id = ?",
        account_id,
        now,
        session_id);
    if (result.affectedRows() == 0)
    {
        error = "세션을 찾을 수 없습니다.";
        return false;
    }
    return true;
}

std::vector<AccountView> SessionStore::listAccounts() const
{
    std::vector<AccountView> accounts;
    auto rows = m_client->execSqlSync("SELECT id, name FROM user ORDER BY id");
    accounts.reserve(rows.size());
    for (const auto& row : rows)
    {
        AccountView view;
        view.id = row["id"].as<int64_t>();
        view.name = row["name"].as<std::string>();
        accounts.push_back(std::move(view));
    }
    return accounts;
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
