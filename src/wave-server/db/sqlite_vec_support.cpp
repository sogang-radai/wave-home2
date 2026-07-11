#include "sqlite_vec_support.h"

#define SQLITE_CORE
#include <sqlite3.h>
#include <sqlite-vec.h>

#include "../core/logger.h"

WAVE_NAMESPACE_BEGIN

bool registerSqliteVecExtension()
{
    // Drogon also requests multithread mode on first connection. Set it here
    // first so sqlite3_auto_extension does not initialize SQLite too early and
    // leave Drogon's sqlite3_config() call failing.
    const int cfg = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
    if (cfg != SQLITE_OK && cfg != SQLITE_MISUSE)
        LOG_WARN("sqlite3_config(MULTITHREAD) returned {}", cfg);

    const int rc = sqlite3_auto_extension(reinterpret_cast<void (*)(void)>(sqlite3_vec_init));
    if (rc != SQLITE_OK)
    {
        LOG_WARN("sqlite-vec auto_extension failed: {}", rc);
        return false;
    }
    LOG_INFO("sqlite-vec registered (static, {})", SQLITE_VEC_VERSION);
    return true;
}

WAVE_NAMESPACE_END
