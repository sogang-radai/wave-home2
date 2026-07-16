#pragma once

#include <string>

#include "../../../db/database.h"
#include <json/json.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace web {
namespace internal {

class DbQueryStore
{
public:
    explicit DbQueryStore(db::DbClientPtr client);

    Json::Value execute(const Json::Value& request, std::string& error, std::string& field) const;

private:
    db::DbClientPtr m_client;

    Json::Value executeOne(const Json::Value& query) const;
    Json::Value executeOneUnchecked(const Json::Value& query, const std::string& table) const;
};

} // namespace internal
} // namespace web
WAVE_NAMESPACE_END
