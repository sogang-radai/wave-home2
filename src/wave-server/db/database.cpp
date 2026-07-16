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
            "schema v1 baseline",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS user (
    id         INTEGER     NOT NULL,
    name       TEXT        NOT NULL,
    created_at VARCHAR(50),
    PRIMARY KEY (id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_session (
    id                INTEGER     PRIMARY KEY,
    active_user_id    INTEGER,
    access_token_hash TEXT        NOT NULL,
    created_at        VARCHAR(50) NOT NULL,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (active_user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS room (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    PRIMARY KEY (id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS room_user_map (
    room_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device (
    id              INTEGER NOT NULL,
    name            TEXT    NOT NULL,
    description     TEXT    NOT NULL,
    class           TEXT    NOT NULL,
    archived        INTEGER NOT NULL,
    enabled         INTEGER NOT NULL DEFAULT 1,
    interface_json  TEXT    NOT NULL DEFAULT '{}',
    settings_json   TEXT,
    PRIMARY KEY (id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device_user_map (
    device_id INTEGER NOT NULL,
    user_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, user_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS device_room_map (
    device_id INTEGER NOT NULL,
    room_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, room_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (room_id) REFERENCES room(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_sleep_config (
    user_id    INTEGER     PRIMARY KEY,
    config     TEXT        NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_general_settings (
    user_id    INTEGER     PRIMARY KEY,
    settings   TEXT        NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS user_ai_agent_settings (
    user_id           INTEGER     PRIMARY KEY,
    personal_prompt   TEXT        NOT NULL DEFAULT '',
    selected_model_id TEXT        NOT NULL,
    ctrl_enter_send   INTEGER     NOT NULL DEFAULT 0,
    wave_ai_sound     INTEGER     NOT NULL DEFAULT 1,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS gesture_set (
    id       INTEGER NOT NULL,
    name     TEXT    NOT NULL,
    archived INTEGER NOT NULL,
    PRIMARY KEY (id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS gesture_log (
    id             INTEGER     PRIMARY KEY,
    gesture_set_id INTEGER     NOT NULL,
    class_id       INTEGER     NOT NULL,
    timestamp      VARCHAR(50) NOT NULL,
    gesture_name   VARCHAR(50) NOT NULL,
    radar_id       INTEGER     NOT NULL,
    device_id      INTEGER,
    action         VARCHAR(100),
    confidence     REAL,
    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_gesture_log_occurred ON gesture_log (timestamp)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS gesture_device_map (
    device_id       INTEGER NOT NULL,
    gesture_set_id  INTEGER NOT NULL,
    PRIMARY KEY (device_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS automation_rule (
    id              INTEGER      PRIMARY KEY,
    user_id         INTEGER      NOT NULL,
    external_id     TEXT         NOT NULL,
    name            VARCHAR(100) NOT NULL,
    enabled         INTEGER      NOT NULL DEFAULT 1,
    cooldown_ms     INTEGER      NOT NULL DEFAULT 0,
    trigger_json    TEXT,
    schedule_json   TEXT,
    actions_json    TEXT         NOT NULL,
    created_at      VARCHAR(50)  NOT NULL,
    updated_at      VARCHAR(50)  NOT NULL,
    UNIQUE (external_id),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_automation_rule_user ON automation_rule (user_id)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS home_event (
    id           INTEGER      PRIMARY KEY,
    user_id      INTEGER      NOT NULL,
    type         VARCHAR(20)  NOT NULL,
    occurred_at  VARCHAR(50)  NOT NULL,
    device_id    INTEGER,
    device_name  VARCHAR(100),
    message      VARCHAR(300) NOT NULL,
    triggered_by VARCHAR(100),
    detail_json  TEXT,
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_home_event_user_time ON home_event (user_id, occurred_at)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS insight (
    id                 INTEGER      PRIMARY KEY,
    user_id            INTEGER      NOT NULL,
    surface            VARCHAR(20)  NOT NULL,
    kind               VARCHAR(10)  NOT NULL,
    date               VARCHAR(10)  NOT NULL,
    label              VARCHAR(50),
    title              VARCHAR(100) NOT NULL,
    text               VARCHAR(500) NOT NULL,
    actionable         INTEGER      NOT NULL DEFAULT 0,
    action_type        VARCHAR(20),
    approved           INTEGER      NOT NULL DEFAULT 0,
    rule_json          TEXT,
    schedule_task_json TEXT,
    created_at         VARCHAR(50)  NOT NULL,
    CHECK (surface IN ('dashboard_banner', 'weekly_plan', 'sleep_report', 'posture_report', 'power')),
    CHECK (kind IN ('banner', 'action', 'goal', 'tip')),
    CHECK (actionable IN (0, 1)),
    CHECK (approved IN (0, 1)),
    CHECK (actionable = 0 OR action_type IS NOT NULL),
    CHECK (action_type NOT IN ('automation_rule', 'reservation') OR rule_json IS NOT NULL),
    CHECK (action_type != 'schedule_task' OR schedule_task_json IS NOT NULL),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_insight_user_surface_date ON insight (user_id, surface, date)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS schedule_task (
    id                INTEGER      PRIMARY KEY,
    user_id           INTEGER      NOT NULL,
    title             VARCHAR(100) NOT NULL,
    created_at        VARCHAR(50),
    created_by        VARCHAR(10)  NOT NULL,
    category          VARCHAR(10)  NOT NULL,
    schedule_kind     VARCHAR(10)  NOT NULL DEFAULT 'weekly',
    day_of_week       VARCHAR(3)   NOT NULL,
    event_date        VARCHAR(10),
    start_minute      INTEGER,
    end_minute        INTEGER,
    done              INTEGER      NOT NULL,
    source_insight_id INTEGER,
    CHECK (schedule_kind IN ('weekly', 'once')),
    CHECK (day_of_week IN ('mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun')),
    CHECK (created_by IN ('user', 'agent')),
    CHECK (
        (schedule_kind = 'weekly' AND event_date IS NULL)
        OR (schedule_kind = 'once' AND event_date IS NOT NULL)
    ),
    CHECK ((start_minute IS NULL AND end_minute IS NULL)
           OR (start_minute >= 0 AND start_minute < end_minute AND end_minute <= 1440)),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (source_insight_id) REFERENCES insight(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_schedule_task_user_day ON schedule_task (user_id, day_of_week)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_schedule_task_user_event ON schedule_task (user_id, event_date)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_schedule_task_insight ON schedule_task (source_insight_id)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS alarm (
    id              INTEGER      PRIMARY KEY,
    user_id         INTEGER      NOT NULL,
    name            VARCHAR(100) NOT NULL,
    time_minute     INTEGER      NOT NULL,
    days_of_week    TEXT         NOT NULL,
    smart_wake      INTEGER      NOT NULL,
    radar_device_id INTEGER,
    device_id       INTEGER,
    method          TEXT         NOT NULL,
    enabled         INTEGER      NOT NULL,
    created_at      VARCHAR(50)  NOT NULL,
    updated_at      VARCHAR(50)  NOT NULL,
    CHECK (time_minute >= 0 AND time_minute <= 1439),
    CHECK (smart_wake IN (0, 1)),
    CHECK (enabled IN (0, 1)),
    CHECK (smart_wake = 0 OR radar_device_id IS NOT NULL),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (radar_device_id) REFERENCES device(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_alarm_user_enabled ON alarm (user_id, enabled)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_alarm_user_time ON alarm (user_id, time_minute)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS notification (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    type       VARCHAR(20)  NOT NULL,
    message    VARCHAR(200) NOT NULL,
    read       INTEGER      NOT NULL,
    created_at VARCHAR(50)  NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_notification_user_created ON notification (user_id, created_at)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS chat_history (
    id         INTEGER      NOT NULL,
    user_id    INTEGER      NOT NULL,
    title      VARCHAR(100) NOT NULL DEFAULT '새 대화',
    created_at VARCHAR(50)  NOT NULL,
    updated_at VARCHAR(50)  NOT NULL,
    message    TEXT         NOT NULL,
    PRIMARY KEY (id),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_chat_history_user_updated ON chat_history (user_id, updated_at DESC)
)SQL",
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
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER NOT NULL,
    applied_at  VARCHAR(50) NOT NULL,
    description TEXT,
    PRIMARY KEY (version)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS sleep_session (
    id             INTEGER     PRIMARY KEY,
    user_id        INTEGER     NOT NULL,
    room_id        INTEGER     NOT NULL,
    radar_id       INTEGER     NOT NULL,
    station_id     INTEGER,
    night_date     VARCHAR(50) NOT NULL,
    onset          VARCHAR(50),
    final_wake     VARCHAR(50),
    time_in_bed_s  INTEGER,
    asleep_total_s INTEGER,
    efficiency     REAL,
    stage_totals   TEXT,
    toss_events    INTEGER,
    hr_mean        REAL,
    br_mean        REAL,
    snore_ratio    REAL,
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (station_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS sleep_stat (
    id               INTEGER     PRIMARY KEY,
    user_id          INTEGER     NOT NULL,
    room_id          INTEGER     NOT NULL,
    session_id       INTEGER,
    granularity      VARCHAR(3)  NOT NULL,
    time_start       VARCHAR(50) NOT NULL,
    time_end         VARCHAR(50),
    coverage         REAL        NOT NULL,
    stage_label      VARCHAR(10),
    stage_ratio      TEXT,
    stage_confidence REAL,
    status_ratio     TEXT,
    toss_mean        REAL,
    toss_max         REAL,
    toss_p90         REAL,
    toss_events      INTEGER,
    toss_ratio       TEXT,
    hr_mean          REAL,
    hr_min           REAL,
    hr_max           REAL,
    hr_std           REAL,
    hr_confidence    REAL,
    br_mean          REAL,
    br_min           REAL,
    br_max           REAL,
    br_std           REAL,
    snore_ratio      REAL,
    env_temp         REAL,
    env_lux          REAL,
    env_noise        REAL,
    summary_text     TEXT,
    CHECK (granularity IN ('1m', '30m')),
    UNIQUE (user_id, granularity, time_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_sleep_stat_user_granularity_time
    ON sleep_stat (user_id, granularity, time_start)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_sleep_session_user_night
    ON sleep_session (user_id, night_date)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS sleep_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    session_id   INTEGER,
    metrics      TEXT,
    report_text  TEXT,
    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS power_energy (
    id           INTEGER     PRIMARY KEY,
    device_id    INTEGER,
    granularity  VARCHAR(3)  NOT NULL,
    time_start   VARCHAR(50) NOT NULL,
    energy_wh    REAL        NOT NULL,
    coverage     REAL        NOT NULL,
    sample_count INTEGER     NOT NULL,
    CHECK (granularity IN ('5m', '1h', '24h', '1w', '1mo')),
    FOREIGN KEY (device_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE UNIQUE INDEX IF NOT EXISTS uq_power_energy ON power_energy (COALESCE(device_id, -1), granularity, time_start)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS power_report (
    id           INTEGER     PRIMARY KEY,
    energy_id    INTEGER     NOT NULL,
    device_id    INTEGER,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    metrics      TEXT,
    report_text  TEXT,
    created_at   VARCHAR(50),
    CHECK (period IN ('1h', '24h', '1w', '1mo', '1yr')),
    FOREIGN KEY (energy_id) REFERENCES power_energy(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
)
)SQL",
                R"SQL(
CREATE UNIQUE INDEX IF NOT EXISTS uq_power_report ON power_report (COALESCE(device_id, -1), period, period_start)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS posture_stat (
    id          INTEGER     PRIMARY KEY,
    user_id     INTEGER     NOT NULL,
    granularity VARCHAR(3)  NOT NULL,
    time_start  VARCHAR(50) NOT NULL,
    time_end    VARCHAR(50),
    score       INTEGER,
    metrics     TEXT,
    CHECK (granularity IN ('1h', '1d')),
    UNIQUE (user_id, granularity, time_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS posture_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    metrics      TEXT,
    report_text  TEXT,
    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS weekly_plan_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    headline     VARCHAR(100),
    report_text  TEXT NOT NULL,
    created_at   VARCHAR(50) NOT NULL,
    UNIQUE (user_id, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
)
)SQL",
            },
        },
        {
            2,
            "user action log (insight/schedule_task execution history)",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS user_action_log (
    id            INTEGER      PRIMARY KEY,
    user_id       INTEGER      NOT NULL,
    action_type   VARCHAR(30)  NOT NULL,
    ref_type      VARCHAR(20)  NOT NULL,
    ref_id        INTEGER      NOT NULL,
    occurred_at   VARCHAR(50)  NOT NULL,
    metadata_json TEXT,
    CHECK (action_type IN ('insight_applied', 'insight_canceled', 'schedule_task_completed', 'schedule_task_uncompleted', 'schedule_task_created')),
    CHECK (ref_type IN ('insight', 'schedule_task')),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_action_log_user_time ON user_action_log (user_id, occurred_at)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_action_log_ref ON user_action_log (ref_type, ref_id)
)SQL",
            },
        },
        {
            3,
            "sleep plan (tonight's recommended bedtime/wake time cache)",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS sleep_plan (
    id                      INTEGER      PRIMARY KEY,
    user_id                 INTEGER      NOT NULL,
    plan_date               VARCHAR(10)  NOT NULL,
    bedtime_minute          INTEGER      NOT NULL,
    wake_minute             INTEGER      NOT NULL,
    prep_minute             INTEGER,
    recommended_temp_c      REAL,
    target_duration_minutes INTEGER      NOT NULL,
    rationale_text          VARCHAR(300) NOT NULL,
    created_at              VARCHAR(50)  NOT NULL,
    CHECK (bedtime_minute >= 0 AND bedtime_minute <= 1439),
    CHECK (wake_minute >= 0 AND wake_minute <= 1439),
    UNIQUE (user_id, plan_date),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_sleep_plan_user_date ON sleep_plan (user_id, plan_date)
)SQL",
            },
        },
        {
            4,
            "power report embedding fallback storage",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS power_report_embedding (
    report_id      INTEGER     PRIMARY KEY,
    dim            INTEGER     NOT NULL,
    embedding_blob BLOB        NOT NULL,
    updated_at     VARCHAR(50) NOT NULL,
    FOREIGN KEY (report_id) REFERENCES power_report(id)
))SQL",
            },
        },
        {
            5,
            "index power_energy for granularity/time_start range scans",
            {
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_power_energy_gran_time ON power_energy (granularity, time_start, device_id)
)SQL",
            },
        },
        {
            6,
            "goal-based habit coaching tables (goal, goal_coaching_report, goal_recommendation)",
            {
                R"SQL(
CREATE TABLE IF NOT EXISTS goal (
    id          INTEGER      PRIMARY KEY,
    user_id     INTEGER      NOT NULL,
    title       VARCHAR(200) NOT NULL,
    category    VARCHAR(10)  NOT NULL,
    status      VARCHAR(10)  NOT NULL DEFAULT 'active',
    created_at  VARCHAR(50)  NOT NULL,
    updated_at  VARCHAR(50)  NOT NULL,
    CHECK (category IN ('sleep', 'posture', 'mental', 'life', 'diet')),
    CHECK (status IN ('active', 'archived', 'completed')),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_goal_user_status ON goal (user_id, status)
)SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS goal_coaching_report (
    id                      INTEGER      PRIMARY KEY,
    goal_id                 INTEGER      NOT NULL,
    user_id                 INTEGER      NOT NULL,
    period_start            VARCHAR(10)  NOT NULL,
    past_summary_text       VARCHAR(500) NOT NULL,
    projection_text         VARCHAR(500) NOT NULL,
    projected_metrics_json  TEXT,
    created_at              VARCHAR(50)  NOT NULL,
    UNIQUE (goal_id, period_start),
    FOREIGN KEY (goal_id) REFERENCES goal(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE TABLE IF NOT EXISTS goal_recommendation (
    id                 INTEGER      PRIMARY KEY,
    goal_id            INTEGER      NOT NULL,
    user_id            INTEGER      NOT NULL,
    date               VARCHAR(10)  NOT NULL,
    kind               VARCHAR(10)  NOT NULL,
    title              VARCHAR(100) NOT NULL,
    text               VARCHAR(500) NOT NULL,
    actionable         INTEGER      NOT NULL DEFAULT 0,
    action_type        VARCHAR(20),
    approved           INTEGER      NOT NULL DEFAULT 0,
    rule_json          TEXT,
    schedule_task_json TEXT,
    created_at         VARCHAR(50)  NOT NULL,
    CHECK (kind IN ('action', 'goal', 'tip')),
    CHECK (actionable IN (0, 1)),
    CHECK (approved IN (0, 1)),
    CHECK (actionable = 0 OR action_type IS NOT NULL),
    CHECK (action_type != 'automation_rule' OR rule_json IS NOT NULL),
    CHECK (action_type != 'schedule_task' OR schedule_task_json IS NOT NULL),
    FOREIGN KEY (goal_id) REFERENCES goal(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
))SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_goal_recommendation_goal_date ON goal_recommendation (goal_id, date)
)SQL",
            },
        },
        {
            7,
            "add category column to user_action_log for goal-coaching filtering",
            {
                R"SQL(
ALTER TABLE user_action_log ADD COLUMN category VARCHAR(10)
)SQL",
                R"SQL(
CREATE INDEX IF NOT EXISTS idx_action_log_user_category_time ON user_action_log (user_id, category, occurred_at)
)SQL",
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

    void seedNotificationsIfEmpty(const drogon::orm::DbClientPtr& client);
    void seedRoomsAndDevicesIfEmpty(const drogon::orm::DbClientPtr& client);

    void seedInitialData(const drogon::orm::DbClientPtr& client)
    {
        auto rows = client->execSqlSync("SELECT COUNT(*) FROM user");
        if (!rows.empty() && rows[0][0].as<int64_t>() > 0)
        {
            seedNotificationsIfEmpty(client);
            seedRoomsAndDevicesIfEmpty(client);
            return;
        }

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

        seedNotificationsIfEmpty(client);
        seedRoomsAndDevicesIfEmpty(client);
    }

    void seedNotificationsIfEmpty(const drogon::orm::DbClientPtr& client)
    {
        try
        {
            auto rows = client->execSqlSync("SELECT COUNT(*) FROM notification");
            if (!rows.empty() && rows[0][0].as<int64_t>() > 0)
                return;

            client->execSqlSync(
                R"SQL(
INSERT INTO notification (id, user_id, type, message, read, created_at) VALUES
(1, 1, 'timer', '착석 1시간 48분 경과 — 스트레칭을 해보세요', 0, '2026-07-02 07:10:00'),
(2, 1, 'sleep', '오늘 수면 목표까지 30분 부족합니다', 0, '2026-07-02 07:12:00'),
(3, 2, 'posture', '오래 앉아 있었어요. 자세를 바꿔보세요', 0, '2026-07-02 08:00:00')
)SQL");
        }
        catch (const std::exception&)
        {
            // notification table may not exist yet on very old DB paths
        }
    }

    void seedRoomsAndDevicesIfEmpty(const drogon::orm::DbClientPtr& client)
    {
        try
        {
            auto room_rows = client->execSqlSync("SELECT COUNT(*) FROM room");
            if (!room_rows.empty() && room_rows[0][0].as<int64_t>() == 0)
            {
                client->execSqlSync(
                    R"SQL(
INSERT INTO room (id, name, description) VALUES
(1, '거실', '거실'),
(2, '침실', '침실'),
(3, '부엌', '부엌')
)SQL");
                client->execSqlSync(
                    R"SQL(
INSERT INTO room_user_map (room_id, user_id) VALUES
(1, 1), (1, 2), (2, 1)
)SQL");
            }

            auto device_rows = client->execSqlSync("SELECT COUNT(*) FROM device");
            if (!device_rows.empty() && device_rows[0][0].as<int64_t>() > 0)
                return;

            client->execSqlSync(
                R"SQL(
INSERT INTO device (id, name, description, class, archived, enabled, interface_json, settings_json) VALUES
(1, '침실 하방 레이더', 'SRS R4SN mmWave 레이더', 'srs_r4sn', 0, 1,
 '{"host":"192.168.0.33","mac":"68:96:6A:4C:69:D4","point_cloud":{"enabled":true,"port":29172},"iq":{"enabled":true,"port":29171}}',
 '{"angle_z":0.0,"angle_y":0.0,"min_x":-5.0,"max_x":5.0,"min_y":0.0,"max_y":10.0,"min_z":-2.0,"max_z":2.0}'),
(2, 'Wave Station', '침실 Wave Station', 'wave_station', 0, 1,
 '{"host":"192.168.0.60","port":8765}',
 '{"sample_rate":16000,"sample_size":16,"channels":1,"capabilities":{"mic_pcm":true,"mic_opus":true,"speaker_pcm":false,"speaker_opus":true,"ir_receive":true,"ir_transmit":true,"ambient_light":true,"temperature":true,"humidity":true}}'),
(3, '거실 카메라', '거실 IoT 카메라', 'reolink_e1_pro', 0, 1,
 '{"host":"192.168.0.50","mac":"94:8C:D7:A2:6A:97","user":"enc:0500120444","password":"enc:0500120444595d5e51","rtsp_port":554,"go2rtc":true}',
 NULL),
(4, '플러그1 - 선풍기', '거실 선풍기 스마트 플러그', 'tuya_ep2h', 0, 1,
 '{"host":"192.168.0.37","mac":"50:8B:B9:9F:6E:83","device_id":"eb61aa6ce49add5d80yfcj","local_key":"s^q2?;Ur|q{SlG(>","version":"3.3"}',
 NULL),
(5, '침실 TV', '침실 책상 - 삼성 32인치 TV', 'samsung_g7', 0, 1,
 '{"host":"192.168.0.24","mac":"04:E4:B6:A9:8D:0A","token":"13135473"}',
 NULL),
(6, '거실 조명', '거실 조명 - 컬러', 'philips_wiz_e29_white', 0, 1,
 '{"host":"192.168.0.51","mac":"98:77:D5:D0:B4:42","port":38899}',
 NULL)
)SQL");

            client->execSqlSync(
                R"SQL(
INSERT INTO device_room_map (device_id, room_id) VALUES
(1, 2), (2, 2), (3, 1), (4, 1), (5, 2), (6, 1)
)SQL");
        }
        catch (const std::exception& e)
        {
            LOG_WARN("Room/device seed skipped: {}", e.what());
        }
    }
}

void configureConnectionSettings(const drogon::orm::DbClientPtr& client)
{
    if (!client)
        return;

    try
    {
        // WAL lets readers proceed while PowerManager/SleepManager write; busy_timeout
        // avoids wedging every Drogon worker thread on SQLITE_BUSY forever.
        client->execSqlSync("PRAGMA journal_mode=WAL");
        client->execSqlSync("PRAGMA busy_timeout=5000");
        client->execSqlSync("PRAGMA synchronous=NORMAL");
    }
    catch (const std::exception& e)
    {
        LOG_WARN("SQLite PRAGMA setup failed: {}", e.what());
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

bool validateDatabaseSchema(const drogon::orm::DbClientPtr& client)
{
    if (!client)
    {
        LOG_ERROR("Database client is null");
        return false;
    }

    try
    {
        const int version = currentVersion(client);
        if (version <= 0)
        {
            LOG_ERROR("Database schema_version is empty or missing");
            return false;
        }

        LOG_INFO("Database schema validated (version {}, migrations skipped)", version);
        return true;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Database schema validation failed: {}", e.what());
        return false;
    }
}

} // namespace db
WAVE_NAMESPACE_END
