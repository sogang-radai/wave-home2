#pragma once

#include <drogon/orm/DbClient.h>
#include <vector>

#include "core/coredefs.h"

#define DB_NAMESPACE_BEGIN namespace db {
#define DB_NAMESPACE_END }

WAVE_NAMESPACE_BEGIN
DB_NAMESPACE_BEGIN

struct Migration
{
    int version;
    const char* description;
    std::vector<const char*> statements;
};

using DbClientPtr = drogon::orm::DbClientPtr;

void configureConnectionSettings(const DbClientPtr& client);
bool runMigrations(const DbClientPtr& client);
bool validateDatabaseSchema(const DbClientPtr& client);

DB_NAMESPACE_END
WAVE_NAMESPACE_END
