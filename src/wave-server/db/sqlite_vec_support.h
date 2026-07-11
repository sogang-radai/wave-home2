#pragma once

#include "core/coredefs.h"

WAVE_NAMESPACE_BEGIN

/** Register sqlite-vec via sqlite3_auto_extension (call before opening DB). */
bool registerSqliteVecExtension();

WAVE_NAMESPACE_END
