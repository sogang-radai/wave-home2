#include "accounts_store.h"
#include "../../../db/database.h"

#include <algorithm>
#include <cctype>

#include "session_store.h"
#include "../../../core/logger.h"
#include "util/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {
namespace
{
    std::string trim_copy(const std::string& value)
    {
        const auto start = value.find_first_not_of(" \t\n\r");
        if (start == std::string::npos)
            return {};
        const auto end = value.find_last_not_of(" \t\n\r");
        return value.substr(start, end - start + 1);
    }
}

AccountsStore::AccountsStore(db::DbClientPtr client) :
    m_client(std::move(client))
{
}

int64_t AccountsStore::nextAccountId() const
{
    auto rows = m_client->execSqlSync("SELECT COALESCE(MAX(id), 0) + 1 AS next_id FROM user");
    return rows.empty() ? 1 : rows[0]["next_id"].as<int64_t>();
}

std::optional<AccountView> AccountsStore::createAccount(const std::string& name, std::string& error, std::string& field)
{
    const auto trimmed = trim_copy(name);
    if (trimmed.empty())
    {
        error = "이름을 입력해주세요.";
        field = "name";
        return std::nullopt;
    }

    const auto id = nextAccountId();
    const auto now = formatTimestamp();
    try
    {
        m_client->execSqlSync(
            "INSERT INTO user (id, name, created_at) VALUES (?, ?, ?)",
            id,
            trimmed,
            now);
    }
    catch (const std::exception& e)
    {
        WLOG_ERROR("Failed to create account: {}", e.what());
        error = "구성원 생성에 실패했습니다.";
        return std::nullopt;
    }

    AccountView view;
    view.id = id;
    view.name = trimmed;
    return view;
}

std::optional<AccountView> AccountsStore::updateAccount(
    int64_t account_id,
    const std::string& name,
    std::string& error,
    std::string& field)
{
    const auto trimmed = trim_copy(name);
    if (trimmed.empty())
    {
        error = "이름을 입력해주세요.";
        field = "name";
        return std::nullopt;
    }

    auto rows = m_client->execSqlSync(
        "UPDATE user SET name = ? WHERE id = ?",
        trimmed,
        account_id);
    if (rows.affectedRows() == 0)
    {
        error = "구성원을 찾을 수 없습니다.";
        return std::nullopt;
    }

    AccountView view;
    view.id = account_id;
    view.name = trimmed;
    return view;
}

bool AccountsStore::deleteAccount(int64_t account_id, int64_t session_id, std::string& error, std::string& code)
{
    SessionStore sessions(m_client);
    const auto account = sessions.findAccount(account_id);
    if (!account)
    {
        error = "구성원을 찾을 수 없습니다.";
        code = "NOT_FOUND";
        return false;
    }

    auto session_rows = m_client->execSqlSync(
        "SELECT active_user_id FROM user_session WHERE id = ? LIMIT 1",
        session_id);
    if (!session_rows.empty() && !session_rows[0]["active_user_id"].isNull()
        && session_rows[0]["active_user_id"].as<int64_t>() == account_id)
    {
        error = "현재 사용 중인 구성원은 삭제할 수 없습니다.";
        code = "CANNOT_DELETE_ACTIVE_ACCOUNT";
        return false;
    }

    try
    {
        m_client->execSqlSync("DELETE FROM room_user_map WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM device_user_map WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM user_sleep_config WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM user_general_settings WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM user_ai_agent_settings WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM notification WHERE user_id = ?", account_id);
        m_client->execSqlSync("DELETE FROM user WHERE id = ?", account_id);
        return true;
    }
    catch (const std::exception& e)
    {
        WLOG_ERROR("Failed to delete account: {}", e.what());
        error = "구성원 삭제에 실패했습니다.";
        code = "DELETE_FAILED";
        return false;
    }
}

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
