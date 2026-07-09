#pragma once

#include <string>

#include <drogon/orm/DbClient.h>
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

class DbQueryStore
{
public:
    explicit DbQueryStore(drogon::orm::DbClientPtr client);

    Json::Value execute(const Json::Value& request, std::string& error, std::string& field) const;

private:
    drogon::orm::DbClientPtr m_client;

    Json::Value executeOne(const Json::Value& query) const;
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
