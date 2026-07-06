#include "database.h"

#include <stdexcept>

#include "../core/logger.h"
#include "../core/time_util.h"

WAVE_NAMESPACE_BEGIN
namespace db {

namespace
{
    const Migration kMigrations[] = {
        {
            1,
            "core accounts, session, rooms, devices, user settings",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER NOT NULL,
    applied_at  VARCHAR(50) NOT NULL,
    description TEXT,
    PRIMARY KEY (version)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user (
    id         INTEGER NOT NULL,
    name       TEXT    NOT NULL,
    created_at VARCHAR(50),
    PRIMARY KEY (id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_session (
    id                INTEGER PRIMARY KEY,
    active_user_id    INTEGER,
    access_token_hash TEXT    NOT NULL,
    created_at        VARCHAR(50) NOT NULL,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (active_user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS room (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    PRIMARY KEY (id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS room_user_map (
    room_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    class       TEXT    NOT NULL,
    archived    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device_user_map (
    device_id INTEGER NOT NULL,
    user_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, user_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device_room_map (
    device_id INTEGER NOT NULL,
    room_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, room_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (room_id) REFERENCES room(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_sleep_config (
    user_id    INTEGER PRIMARY KEY,
    config     TEXT    NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_general_settings (
    user_id    INTEGER PRIMARY KEY,
    settings   TEXT    NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_ai_agent_settings (
    user_id           INTEGER PRIMARY KEY,
    personal_prompt   TEXT    NOT NULL DEFAULT '',
    selected_model_id TEXT    NOT NULL,
    ctrl_enter_send   INTEGER NOT NULL DEFAULT 0,
    wave_ai_sound     INTEGER NOT NULL DEFAULT 1,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
            },
        },
        {
            2,
            "push subscription storage",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS push_subscription (
    session_id  INTEGER NOT NULL,
    endpoint    TEXT    NOT NULL,
    p256dh      TEXT    NOT NULL,
    auth_key    TEXT    NOT NULL,
    created_at  VARCHAR(50) NOT NULL,
    updated_at  VARCHAR(50) NOT NULL,
    PRIMARY KEY (session_id, endpoint),
    FOREIGN KEY (session_id) REFERENCES user_session(id)
))SQL",
            },
        },
    };

    int currentVersion(const drogon::orm::DbClientPtr& client)
    {
        try
        {
            auto rows = client->execSqlSync("SELECT MAX(version) FROM schema_version");
            if (rows.empty() || rows[0][0].isNull())
                return 0;
            return rows[0][0].as<int>();
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    void seedInitialData(const drogon::orm::DbClientPtr& client)
    {
        auto rows = client->execSqlSync("SELECT COUNT(*) FROM user");
        if (!rows.empty() && rows[0][0].as<int64_t>() > 0)
            return;

        const auto now = formatTimestamp();
        LOG_INFO("Seeding initial user/session data");
        LOG_INFO("Dev Bearer token (store as wavehome_access_token): wavehome-dev-token");

        client->execSqlSync(
            "INSERT INTO user (id, name, created_at) VALUES (?, ?, ?), (?, ?, ?)",
            1,
            "김건강",
            now,
            2,
            "박웰빙",
            now);

        client->execSqlSync(
            "INSERT INTO user_session (id, active_user_id, access_token_hash, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
            1,
            1,
            "wavehome-dev-token",
            now,
            now);
    }
}

bool runMigrations(const drogon::orm::DbClientPtr& client)
{
    if (!client)
    {
        LOG_ERROR("Database client is null");
        return false;
    }

    const int applied = currentVersion(client);

    try
    {
        for (const auto& migration : kMigrations)
        {
            if (migration.version <= applied)
                continue;

            LOG_INFO("Applying DB migration v{}: {}", migration.version, migration.description);
            for (const char* sql : migration.statements)
                client->execSqlSync(sql);

            client->execSqlSync(
                "INSERT INTO schema_version (version, applied_at, description) VALUES (?, ?, ?)",
                migration.version,
                formatTimestamp(),
                migration.description);
        }

        seedInitialData(client);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Database migration failed: {}", e.what());
        return false;
    }

    LOG_INFO("Database migrations complete (version {})", currentVersion(client));
    return true;
}

} // namespace db
WAVE_NAMESPACE_END
