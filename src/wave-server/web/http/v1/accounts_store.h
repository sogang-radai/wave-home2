#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"
#include "session_store.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace v1 {

class AccountsStore
{
public:
    explicit AccountsStore(drogon::orm::DbClientPtr client);

    std::optional<AccountView> createAccount(const std::string& name, std::string& error, std::string& field);
    std::optional<AccountView> updateAccount(int64_t account_id, const std::string& name, std::string& error, std::string& field);
    bool deleteAccount(int64_t account_id, int64_t session_id, std::string& error, std::string& code);

private:
    drogon::orm::DbClientPtr m_client;

    int64_t nextAccountId() const;
};

} // namespace v1
} // namespace web
WAVE_NAMESPACE_END
