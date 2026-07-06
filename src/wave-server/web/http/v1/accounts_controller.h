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
    METHOD_LIST_END

    void listAccounts(
        const drogon::HttpRequestPtr& req,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback);
};

} // namespace v1
WEB_NAMESPACE_END
WAVE_NAMESPACE_END
