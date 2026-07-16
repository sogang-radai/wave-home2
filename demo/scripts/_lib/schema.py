"""docs/db-schema.md 전체 스키마(2026-07-08 버전)를 그대로 구현한다.

핵심 테이블(CORE_SCHEMA_SQL)은 항상 만들고, `vec_*` 가상 테이블(VEC_TABLES)은 sqlite-vec
확장이 로드 가능할 때만 만든다(확장이 없으면 건너뛰고 경고만 남긴다).
"""

from __future__ import annotations

import sqlite3

CORE_SCHEMA_SQL = """
CREATE TABLE user (
    id         INTEGER     NOT NULL,
    name       TEXT        NOT NULL,
    created_at VARCHAR(50),
    PRIMARY KEY (id)
);

CREATE TABLE user_session (
    id                INTEGER     PRIMARY KEY,
    active_user_id    INTEGER,
    access_token_hash TEXT        NOT NULL,
    created_at        VARCHAR(50) NOT NULL,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (active_user_id) REFERENCES user(id)
);

CREATE TABLE room (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,
    description TEXT    NOT NULL,
    PRIMARY KEY (id)
);

CREATE TABLE room_user_map (
    room_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,
    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE device (
    id              INTEGER NOT NULL,
    name            TEXT    NOT NULL,
    description     TEXT    NOT NULL,
    class           TEXT    NOT NULL,
    archived        INTEGER NOT NULL,
    enabled         INTEGER NOT NULL DEFAULT 1,
    interface_json  TEXT    NOT NULL DEFAULT '{}',
    settings_json   TEXT,
    PRIMARY KEY (id)
);

CREATE TABLE device_user_map (
    device_id INTEGER NOT NULL,
    user_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, user_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE device_room_map (
    device_id INTEGER NOT NULL,
    room_id   INTEGER NOT NULL,
    PRIMARY KEY (device_id, room_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (room_id) REFERENCES room(id)
);

CREATE TABLE user_sleep_config (
    user_id    INTEGER     PRIMARY KEY,
    config     TEXT        NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE user_general_settings (
    user_id    INTEGER     PRIMARY KEY,
    settings   TEXT        NOT NULL,
    updated_at VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE user_ai_agent_settings (
    user_id           INTEGER     PRIMARY KEY,
    personal_prompt   TEXT        NOT NULL DEFAULT '',
    selected_model_id TEXT        NOT NULL,
    ctrl_enter_send   INTEGER     NOT NULL DEFAULT 0,
    wave_ai_sound     INTEGER     NOT NULL DEFAULT 1,
    updated_at        VARCHAR(50) NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE gesture_set (
    id       INTEGER NOT NULL,
    name     TEXT    NOT NULL,
    archived INTEGER NOT NULL,
    PRIMARY KEY (id)
);

CREATE TABLE gesture_log (
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
);
CREATE INDEX idx_gesture_log_occurred ON gesture_log (timestamp);

CREATE TABLE gesture_device_map (
    device_id       INTEGER NOT NULL,
    gesture_set_id  INTEGER NOT NULL,
    PRIMARY KEY (device_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id)
);

CREATE TABLE automation_rule (
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
);
CREATE INDEX idx_automation_rule_user ON automation_rule (user_id);

CREATE TABLE home_event (
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
);
CREATE INDEX idx_home_event_user_time ON home_event (user_id, occurred_at);

CREATE TABLE insight (
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
);
CREATE INDEX idx_insight_user_surface_date ON insight (user_id, surface, date);

CREATE TABLE schedule_task (
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
);
CREATE INDEX idx_schedule_task_user_day ON schedule_task (user_id, day_of_week);
CREATE INDEX idx_schedule_task_user_event ON schedule_task (user_id, event_date);
CREATE INDEX idx_schedule_task_insight ON schedule_task (source_insight_id);

CREATE TABLE alarm (
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
);
CREATE INDEX idx_alarm_user_enabled ON alarm (user_id, enabled);
CREATE INDEX idx_alarm_user_time ON alarm (user_id, time_minute);

CREATE TABLE notification (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    type       VARCHAR(20)  NOT NULL,
    message    VARCHAR(200) NOT NULL,
    read       INTEGER      NOT NULL,
    created_at VARCHAR(50)  NOT NULL,
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_notification_user_created ON notification (user_id, created_at);

CREATE TABLE chat_history (
    id         INTEGER      NOT NULL,
    user_id    INTEGER      NOT NULL,
    title      VARCHAR(100) NOT NULL DEFAULT '새 대화',
    created_at VARCHAR(50)  NOT NULL,
    updated_at VARCHAR(50)  NOT NULL,
    message    TEXT         NOT NULL,
    PRIMARY KEY (id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_chat_history_user_updated ON chat_history (user_id, updated_at DESC);

CREATE TABLE push_subscription (
    session_id  INTEGER NOT NULL,
    endpoint    TEXT    NOT NULL,
    p256dh      TEXT    NOT NULL,
    auth_key    TEXT    NOT NULL,
    created_at  VARCHAR(50) NOT NULL,
    updated_at  VARCHAR(50) NOT NULL,
    PRIMARY KEY (session_id, endpoint),
    FOREIGN KEY (session_id) REFERENCES user_session(id)
);

CREATE TABLE schema_version (
    version     INTEGER NOT NULL,
    applied_at  VARCHAR(50) NOT NULL,
    description TEXT,
    PRIMARY KEY (version)
);

CREATE TABLE sleep_session (
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
);

CREATE TABLE sleep_stat (
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
);
CREATE INDEX idx_sleep_stat_user_granularity_time
    ON sleep_stat (user_id, granularity, time_start);
CREATE INDEX idx_sleep_session_user_night
    ON sleep_session (user_id, night_date);

CREATE TABLE sleep_report (
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
);

CREATE TABLE power_energy (
    id           INTEGER     PRIMARY KEY,
    device_id    INTEGER,
    granularity  VARCHAR(3)  NOT NULL,
    time_start   VARCHAR(50) NOT NULL,
    energy_wh    REAL        NOT NULL,
    coverage     REAL        NOT NULL,
    sample_count INTEGER     NOT NULL,
    CHECK (granularity IN ('5m', '1h', '24h', '1w', '1mo')),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE UNIQUE INDEX uq_power_energy ON power_energy (COALESCE(device_id, -1), granularity, time_start);

CREATE TABLE power_report (
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
);
CREATE UNIQUE INDEX uq_power_report ON power_report (COALESCE(device_id, -1), period, period_start);

CREATE TABLE posture_stat (
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
);

CREATE TABLE posture_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    metrics      TEXT,
    report_text  TEXT,
    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE weekly_plan_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period_start VARCHAR(50) NOT NULL,
    headline     VARCHAR(100),
    report_text  TEXT NOT NULL,
    created_at   VARCHAR(50) NOT NULL,
    UNIQUE (user_id, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE user_action_log (
    id            INTEGER      PRIMARY KEY,
    user_id       INTEGER      NOT NULL,
    action_type   VARCHAR(30)  NOT NULL,
    ref_type      VARCHAR(20)  NOT NULL,
    ref_id        INTEGER      NOT NULL,
    occurred_at   VARCHAR(50)  NOT NULL,
    metadata_json TEXT,
    category      VARCHAR(10),
    CHECK (action_type IN ('insight_applied', 'insight_canceled', 'schedule_task_completed', 'schedule_task_uncompleted', 'schedule_task_created')),
    CHECK (ref_type IN ('insight', 'schedule_task')),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_action_log_user_time ON user_action_log (user_id, occurred_at);
CREATE INDEX idx_action_log_ref ON user_action_log (ref_type, ref_id);
CREATE INDEX idx_action_log_user_category_time ON user_action_log (user_id, category, occurred_at);

CREATE TABLE sleep_plan (
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
);
CREATE INDEX idx_sleep_plan_user_date ON sleep_plan (user_id, plan_date);

CREATE TABLE power_report_embedding (
    report_id      INTEGER     PRIMARY KEY,
    dim            INTEGER     NOT NULL,
    embedding_blob BLOB        NOT NULL,
    updated_at     VARCHAR(50) NOT NULL,
    FOREIGN KEY (report_id) REFERENCES power_report(id)
);

CREATE INDEX idx_power_energy_gran_time ON power_energy (granularity, time_start, device_id);

CREATE TABLE goal (
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
);
CREATE INDEX idx_goal_user_status ON goal (user_id, status);

CREATE TABLE goal_coaching_report (
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
);

CREATE TABLE goal_recommendation (
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
);
CREATE INDEX idx_goal_recommendation_goal_date ON goal_recommendation (goal_id, date);
"""

VEC_TABLES: dict[str, str] = {
    "vec_sleep_stat": """
        CREATE VIRTUAL TABLE vec_sleep_stat USING vec0 (
            stat_id   INTEGER PRIMARY KEY,
            embedding float[768]
        )
    """,
    "vec_sleep_report": """
        CREATE VIRTUAL TABLE vec_sleep_report USING vec0 (
            report_id INTEGER PRIMARY KEY,
            embedding float[768]
        )
    """,
    "vec_power_report": """
        CREATE VIRTUAL TABLE vec_power_report USING vec0 (
            report_id INTEGER PRIMARY KEY,
            embedding float[768]
        )
    """,
    "vec_posture_report": """
        CREATE VIRTUAL TABLE vec_posture_report USING vec0 (
            report_id INTEGER PRIMARY KEY,
            embedding float[768]
        )
    """,
    "vec_weekly_plan_report": """
        CREATE VIRTUAL TABLE vec_weekly_plan_report USING vec0 (
            report_id INTEGER PRIMARY KEY,
            embedding float[768]
        )
    """,
    "vec_insight_dashboard": """
        CREATE VIRTUAL TABLE vec_insight_dashboard USING vec0 (
            insight_id INTEGER PRIMARY KEY,
            embedding  float[768]
        )
    """,
    "vec_insight_weekly_plan": """
        CREATE VIRTUAL TABLE vec_insight_weekly_plan USING vec0 (
            insight_id INTEGER PRIMARY KEY,
            embedding  float[768]
        )
    """,
    "vec_insight_sleep": """
        CREATE VIRTUAL TABLE vec_insight_sleep USING vec0 (
            insight_id INTEGER PRIMARY KEY,
            embedding  float[768]
        )
    """,
    "vec_insight_posture": """
        CREATE VIRTUAL TABLE vec_insight_posture USING vec0 (
            insight_id INTEGER PRIMARY KEY,
            embedding  float[768]
        )
    """,
    "vec_insight_power": """
        CREATE VIRTUAL TABLE vec_insight_power USING vec0 (
            insight_id INTEGER PRIMARY KEY,
            embedding  float[768]
        )
    """,
}

# insight.surface -> vec_insight_* 테이블 이름
INSIGHT_SURFACE_TO_VEC = {
    "dashboard_banner": "vec_insight_dashboard",
    "weekly_plan": "vec_insight_weekly_plan",
    "sleep_report": "vec_insight_sleep",
    "posture_report": "vec_insight_posture",
    "power": "vec_insight_power",
}


def try_load_vec_extension(conn: sqlite3.Connection) -> bool:
    try:
        import sqlite_vec  # type: ignore

        conn.enable_load_extension(True)
        sqlite_vec.load(conn)
        conn.enable_load_extension(False)
        return True
    except Exception as exc:  # noqa: BLE001 - best effort, report and continue
        print(
            "[schema] sqlite-vec 확장 로드 실패, vec_* 테이블은 건너뜁니다.\n"
            "         해결: uv run --with sqlite-vec demo/scripts/01_gen_raw_data.py\n"
            f"         ({exc})"
        )
        return False


def stamp_schema_version(conn: sqlite3.Connection, version: int = 7, description: str = "schema v7 (sleep_plan/goals/action_log)") -> None:
    conn.execute(
        "INSERT OR REPLACE INTO schema_version (version, applied_at, description) VALUES (?, datetime('now'), ?)",
        (version, description),
    )


def create_vec_tables(conn: sqlite3.Connection) -> int:
    """vec_* 가상 테이블을 만든다. sqlite-vec 로드 실패 시 0 반환."""
    if not try_load_vec_extension(conn):
        return 0

    created = 0
    for name, sql in VEC_TABLES.items():
        exists = conn.execute(
            "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
            (name,),
        ).fetchone()
        if exists:
            continue
        conn.execute(sql)
        created += 1
    if created:
        print(f"[schema] vec_* 테이블 {created}개 신규 생성")
    else:
        print(f"[schema] vec_* 테이블 {len(VEC_TABLES)}개 이미 존재")
    return created


def ensure_runtime_schema(conn: sqlite3.Connection, with_vec: bool = True) -> bool:
    """기존 demo.db에 누락된 테이블·인덱스·schema_version·vec_* 를 보강한다(데이터 유지)."""
    conn.executescript(
        """
CREATE TABLE IF NOT EXISTS push_subscription (
    session_id  INTEGER NOT NULL,
    endpoint    TEXT    NOT NULL,
    p256dh      TEXT    NOT NULL,
    auth_key    TEXT    NOT NULL,
    created_at  VARCHAR(50) NOT NULL,
    updated_at  VARCHAR(50) NOT NULL,
    PRIMARY KEY (session_id, endpoint),
    FOREIGN KEY (session_id) REFERENCES user_session(id)
);
CREATE TABLE IF NOT EXISTS schema_version (
    version     INTEGER NOT NULL,
    applied_at  VARCHAR(50) NOT NULL,
    description TEXT,
    PRIMARY KEY (version)
);
CREATE INDEX IF NOT EXISTS idx_chat_history_user_updated ON chat_history (user_id, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_sleep_stat_user_granularity_time
    ON sleep_stat (user_id, granularity, time_start);
CREATE INDEX IF NOT EXISTS idx_sleep_session_user_night
    ON sleep_session (user_id, night_date);
CREATE TABLE IF NOT EXISTS gesture_device_map (
    device_id       INTEGER NOT NULL,
    gesture_set_id  INTEGER NOT NULL,
    PRIMARY KEY (device_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id)
);

CREATE TABLE IF NOT EXISTS user_action_log (
    id            INTEGER      PRIMARY KEY,
    user_id       INTEGER      NOT NULL,
    action_type   VARCHAR(30)  NOT NULL,
    ref_type      VARCHAR(20)  NOT NULL,
    ref_id        INTEGER      NOT NULL,
    occurred_at   VARCHAR(50)  NOT NULL,
    metadata_json TEXT,
    category      VARCHAR(10),
    CHECK (action_type IN ('insight_applied', 'insight_canceled', 'schedule_task_completed', 'schedule_task_uncompleted', 'schedule_task_created')),
    CHECK (ref_type IN ('insight', 'schedule_task')),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX IF NOT EXISTS idx_action_log_user_time ON user_action_log (user_id, occurred_at);
CREATE INDEX IF NOT EXISTS idx_action_log_ref ON user_action_log (ref_type, ref_id);
CREATE INDEX IF NOT EXISTS idx_action_log_user_category_time ON user_action_log (user_id, category, occurred_at);

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
);
CREATE INDEX IF NOT EXISTS idx_sleep_plan_user_date ON sleep_plan (user_id, plan_date);

CREATE TABLE IF NOT EXISTS power_report_embedding (
    report_id      INTEGER     PRIMARY KEY,
    dim            INTEGER     NOT NULL,
    embedding_blob BLOB        NOT NULL,
    updated_at     VARCHAR(50) NOT NULL,
    FOREIGN KEY (report_id) REFERENCES power_report(id)
);

CREATE INDEX IF NOT EXISTS idx_power_energy_gran_time ON power_energy (granularity, time_start, device_id);

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
);
CREATE INDEX IF NOT EXISTS idx_goal_user_status ON goal (user_id, status);

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
);

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
);
CREATE INDEX IF NOT EXISTS idx_goal_recommendation_goal_date ON goal_recommendation (goal_id, date);
"""
    )
    # Existing DBs may have user_action_log without category (pre-v7).
    cols = {
        row[1]
        for row in conn.execute("PRAGMA table_info(user_action_log)").fetchall()
    }
    if cols and "category" not in cols:
        conn.execute("ALTER TABLE user_action_log ADD COLUMN category VARCHAR(10)")
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_action_log_user_category_time "
            "ON user_action_log (user_id, category, occurred_at)"
        )

    stamp_schema_version(conn)
    vec_ready = False
    if with_vec:
        create_vec_tables(conn)
        vec_ready = conn.execute(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name LIKE 'vec_%'"
        ).fetchone()[0] >= len(VEC_TABLES)
    conn.commit()
    return vec_ready


def build_schema(conn: sqlite3.Connection, with_vec: bool = True) -> bool:
    """CORE 스키마를 항상 만들고, 가능하면 vec_* 가상 테이블도 만든다.

    Returns: vec_* 테이블이 실제로 생성되었는지 여부.
    """
    conn.executescript(CORE_SCHEMA_SQL)
    stamp_schema_version(conn)

    vec_ready = False
    if with_vec:
        create_vec_tables(conn)
        vec_ready = conn.execute(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name LIKE 'vec_%'"
        ).fetchone()[0] >= len(VEC_TABLES)
    conn.commit()
    return vec_ready
