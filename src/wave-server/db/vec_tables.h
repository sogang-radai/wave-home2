#pragma once

#include "database.h"

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN
DB_NAMESPACE_BEGIN

/**
 * Create missing sqlite-vec virtual tables (vec_sleep_*, vec_power_*, vec_insight_*, …).
 * Safe to call repeatedly. Returns false only when vec0 is unavailable.
 */
bool ensureVecTables(const DbClientPtr& client);

DB_NAMESPACE_END
WAVE_NAMESPACE_END
