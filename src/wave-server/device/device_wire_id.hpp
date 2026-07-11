#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <drogon/orm/DbClient.h>

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DEVICE_NAMESPACE_BEGIN

/** True when device_list.json contains this 16-char manifest wire id. */
bool manifestHasWireId(const std::string& wire_id);

/** API wire id for a DB row: zero-padded 16-char hex of the INTEGER PK. */
std::string wireIdForDbRow(int64_t db_id, const std::string& db_name = {});

/** Resolve 16-char hex (or small decimal string) to device.id (INTEGER PK). */
std::optional<int64_t> dbIdForWireId(
    const drogon::orm::DbClientPtr& client,
    const std::string& wire_id);

DEVICE_NAMESPACE_END
WAVE_NAMESPACE_END
