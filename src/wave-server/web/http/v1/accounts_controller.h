#pragma once

#include <drogon/HttpController.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
WEB_NAMESPACE_BEGIN
namespace v1 {

class AccountsController :
    public drogon::HttpController<AccountsController>
{
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AccountsController::listAccounts, "/api/v1/accounts", drogon::Get);
    ADD_METHOD_TO(AccountsController::createAccount, "/api/v1/accounts", drogon::Post);
    ADD_METHOD_TO(AccountsController::updateAccount, "/api/v1/accounts/{accountId}", drogon::Patch);
    ADD_METHOD_TO(AccountsController::deleteAccount, "/api/v1/accounts/{accountId}", drogon::Delete);
    METHOD_LIST_END

    void listAccounts(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void createAccount(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void updateAccount(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t accountId);

    void deleteAccount(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
        int64_t accountId);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
