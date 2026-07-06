#pragma once

#include <drogon/orm/DbClient.h>
#include <vector>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
namespace db {

struct Migration
{
    int version;
    const char* description;
    std::vector<const char*> statements;
};

bool runMigrations(const drogon::orm::DbClientPtr& client);

} // namespace db

WAVE_NAMESPACE_END
