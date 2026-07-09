> `db-schema.md`·`agent-api/`·`wave-home-front/docs/api/` 에 **2026-07-08 반영 완료**. 이 파일은 설계 초안·메모용으로 유지.

## 변경 내역 (통합 시 추가)

- **2026-07-08** — 알람·주간 일정·인사이트·리포트 전면 재설계
  - `routine_task` → `schedule_task` 개명 + `schedule_kind`·`event_date`
  - `alarm` 테이블 추가
  - 기존 `insight` 테이블 **폐기·재정의** — `surface` + `date` 중심, `rule_json`·`schedule_task_json` 초안 보관·원탭 적용
  - **REST API** 초안 — 룰(`iot/rules`), 알람(`alarms`), 루틴·일정(`schedule-tasks`), 인사이트(`insights`)
  - `posture_stat`(초안), `posture_report`, `weekly_plan_report` 추가
  - 인사이트·리포트별 **RAG 벡터 테이블** 추가 (`vec_insight_`*, `vec_posture_report`, `vec_weekly_plan_report`)
  - `docs/agent-api/db-query-api.md`, `docs/agent-api/rag-api.md` 연동 변경 (아래 「API 변경 메모」)
  - 통합 시 `wave-home-front/docs/api/` 에 룰·알람·schedule-tasks·insights 문서 추가 (README 변동사항 참고)

## 목차 (추가·수정)

- 루틴/일정(TODO) — `schedule_task`
- **알람**
- **인사이트** (재설계)
- **리포트** — `sleep_report`(기존), `posture_report`, `power_report`(기존), `weekly_plan_report`
- **통계** — `posture_stat`(초안)
- 알림 — `notification`(기존, 푸시 전용)
- **REST API (`/api/v1`)** — 룰·알람·루틴·일정·인사이트
- API 변경 메모 — `db-query-api`, `rag-api`

---



## 루틴/일정(TODO)

기존 `routine_task`는 **매주 반복**(`schedule_kind='weekly'`)하는 할 일만 표현했다.
주간 계획 UI에서 특정 날짜에만 있는 일정을 위해 `schedule_kind`·`event_date`를 추가하고,
테이블명을 `schedule_task` 로 통일한다.

> **TODO**는 별도 테이블이 아니다. `schedule_task` 행을 주간 캘린더·대시보드 "오늘 할일"에서
> 필터링해 보여 주는 뷰다.



### 스키마

```sql
CREATE TABLE schedule_task (
    id                INTEGER      PRIMARY KEY,
    user_id           INTEGER      NOT NULL,
    title             VARCHAR(100) NOT NULL,
    created_at        VARCHAR(50),
    created_by        VARCHAR(10)  NOT NULL,   -- 'user' | 'agent'
    category          VARCHAR(10)  NOT NULL,   -- 'posture' | 'sleep' | 'diet' | 'mental' | ...
    schedule_kind     VARCHAR(10)  NOT NULL DEFAULT 'weekly',  -- 'weekly' | 'once'
    day_of_week       VARCHAR(3)   NOT NULL,   -- 'mon'…'sun'
    event_date        VARCHAR(10),             -- once: 'YYYY-MM-DD'. weekly: NULL
    start_minute      INTEGER,
    end_minute        INTEGER,
    done              INTEGER      NOT NULL,   -- 0 | 1
    source_insight_id INTEGER,                 -- insight 에서 파생 시 (nullable)

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
```

### 저장·런타임

- **저장**: SQLite `schedule_task` 테이블 (사용자·에이전트 CRUD).
- **뷰**: 주간 계획 캘린더·대시보드 "오늘 할일"은 동일 테이블을 `day_of_week` / `event_date` 로 필터링.
- REST 상세는 아래 [Schedule Tasks API](#schedule-tasks-api) 참고.

---



## 알람

```sql
CREATE TABLE alarm (
    id              INTEGER      PRIMARY KEY,
    user_id         INTEGER      NOT NULL,
    name            VARCHAR(100) NOT NULL,
    time_minute     INTEGER      NOT NULL,   -- 0~1439
    days_of_week    TEXT         NOT NULL,   -- JSON []. '[]'=1회성
    smart_wake      INTEGER      NOT NULL,   -- 0 | 1
    radar_device_id INTEGER,
    device_id       INTEGER,
    method          TEXT         NOT NULL,   -- JSON AlarmMethod
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
```

### 저장·런타임

- **저장**: SQLite `alarm` 테이블.
- REST 상세는 아래 [Alarms API](#alarms-api) 참고. 프론트 초안: `wave-home-front/docs/api/alarm.md`.

---



## 인사이트 (재설계)

기존 `insight`(`domain`, `label`, `title`, `text`, `approved`)는 프론트 표면이 늘면서
역할이 섞였다. **표시 위치(**`surface`**)** 를 1차 키로 두고, 리포트·배너·추천 카드를 한 테이블에
통합하되 RAG·조회는 **surface 별 벡터 테이블**로 분리한다.

### 설계 원칙

- `surface` — UI 슬롯 (아래 CHECK 목록). 페이지·API 조회의 1차 필터.
- `kind` — 카드 성격: `banner`(한 줄 배너), `action`(실행 가능), `goal`(목표), `tip`(정보).
- `date` — 인사이트 **발행일** `'YYYY-MM-DD'` 하나만 둔다. 매일 갱신되므로 시각·주기(`period`) 필드는 없다.
  당일 조회는 `date = 오늘`. 과거 날짜 행은 이력·RAG용으로 보존. 리포트 내 카드도 `date` 만으로 조회한다.
- `label` — UI 섹션 제목 (예: '오늘의 권장 액션', '다음 주 목표'). `group_key`는 사용하지 않는다.
- **정렬** — `sort_order` 없음. 같은 `surface`·`date` 안에서는 `created_at` 오름차순(생성 순).
- `actionable` — `1`이면 카드 **클릭 한 번**으로 바로 반영 가능(아래 `action_type` 참고).
- `action_type` — `actionable=1` 일 때만. 클릭 시 백엔드가 수행할 동작 종류.
- `approved` — 클릭 적용 **완료** 여부 (`0`=미적용, `1`=적용됨). 별도 승인 단계 없음.
- `rule_json` — `action_type` 이 `automation_rule` 또는 `reservation` 일 때,
  `device/rules.json` 의 **Rule 객체 1건**을 JSON 그대로 보관 (`device-tool-api.md` 의 `Rule` 타입).
  - 에이전트가 인사이트 생성 시 **초안**으로 채운다. 아직 `rules.json` 에는 없다.
  - 사용자가 **실행(적용)** 하면 `POST /insights/{id}/apply` 가 `rule_json` 을 검증·`rules.json` 에 upsert하고 `approved=1`.
  - 적용 후에도 `rule_json` 은 스냅샷으로 유지(최종 `id`·`enabled` 반영본으로 갱신 가능).
- `schedule_task_json` — `action_type` 이 `schedule_task` 일 때,
  `schedule_task` 테이블 1건과 동일 구조의 JSON (`id`·`sourceInsightId`·`createdBy` 제외).
  - 에이전트가 초안으로 채운다. 적용 전에는 DB `schedule_task` 에 없다.
  - 사용자 **실행(적용)** 시 `POST /insights/{id}/apply` 가 `schedule_task_json` → `schedule_task` 행 생성.
  - 미해당 타입이거나 미적용이면 각각 NULL.
- `created_at` — 생성 시각 `'YYYY-MM-DD HH:MM:SS'` (날짜+시간). `updated_at` 없음 — 당일 행은 재생성·교체로 갱신.
- RAG 텍스트 — 임베딩 대상은 `title + "\n" + text`. 원문은 `insight`에만 저장.

### actionable 클릭 동작 (원탭 적용)

`actionable=1` 인 카드는 사용자가 **한 번 클릭**하면 `POST /insights/{id}/apply` 로 즉시 반영한다.
추가 확인 모달·2단계 승인은 없다(프론트 기본 UX).

| `action_type` | 클릭 시 동작 | 비고 |
|---------------|-------------|------|
| `schedule_task` | `schedule_task_json` → `schedule_task` 1건 생성 (`source_insight_id` 연결) | 주간 계획·대시보드 추천 |
| `automation_rule` | `rule_json` → `device/rules.json` 에 트리거 룰 등록·활성화 | 홈 자동화 |
| `reservation` | `rule_json` → `device/rules.json` 에 예약 룰 등록·활성화 | IoT 예약 |

- 적용 성공 시 `approved=1` (+ `rule_json` 최종본 갱신 when applicable). 이미 `approved=1` 이면 `409 ALREADY_APPLIED`.
- `automation_rule` / `reservation` 적용은 내부적으로 `POST /api/v1/iot/rules` 와 동일한 RuleStore 경로를 탄다.
- 적용 취소(`PATCH`) 시 `approved=0`, `rules.json` 에서 `rule_json.id` 룰 삭제, `source_insight_id` 로 파생 `schedule_task` 삭제.
- `actionable=0` (`tip`, 배너 등) — 클릭 적용 없음. 배너는 표시만.

### `surface` 값

| surface | 용도 | `date` |
|---------|------|--------|
| `dashboard_banner` | 대시보드 히어로 배너 | 오늘 |
| `weekly_plan` | 주간 계획 우측 AI 맞춤 추천 | 오늘 |
| `sleep_report` | 수면 리포트 내 권장 카드 | 리포트 `period_start`와 동일 날짜 |
| `posture_report` | 자세 리포트 내 권장 카드 | 리포트 `period_start`와 동일 날짜 |
| `power` | 전력 소비 권장 (프론트 추후) | 오늘 |




### 스키마

```sql
CREATE TABLE insight (
    id            INTEGER      PRIMARY KEY,
    user_id       INTEGER      NOT NULL,
    surface       VARCHAR(20)  NOT NULL,
    kind          VARCHAR(10)  NOT NULL,   -- 'banner' | 'action' | 'goal' | 'tip'
    date          VARCHAR(10)  NOT NULL,   -- 발행일 'YYYY-MM-DD'
    label         VARCHAR(50),             -- UI 섹션 제목 (예: '오늘의 권장 액션')
    title         VARCHAR(100) NOT NULL,
    text          VARCHAR(500) NOT NULL,
    actionable    INTEGER      NOT NULL DEFAULT 0,  -- 1 = 클릭 원탭 적용
    action_type   VARCHAR(20),             -- actionable=1 일 때: 'schedule_task' | 'automation_rule' | 'reservation'
    approved      INTEGER      NOT NULL DEFAULT 0,  -- 1 = 클릭 적용 완료
    rule_json            TEXT,                    -- automation_rule | reservation: Rule JSON
    schedule_task_json   TEXT,                    -- schedule_task: ScheduleTask JSON 초안
    created_at    VARCHAR(50)  NOT NULL,   -- 'YYYY-MM-DD HH:MM:SS'

    CHECK (surface IN ('dashboard_banner', 'weekly_plan', 'sleep_report', 'posture_report', 'power')),
    CHECK (kind IN ('banner', 'action', 'goal', 'tip')),
    CHECK (actionable IN (0, 1)),
    CHECK (approved IN (0, 1)),
    CHECK (actionable = 0 OR action_type IS NOT NULL),
    CHECK (
        action_type NOT IN ('automation_rule', 'reservation')
        OR rule_json IS NOT NULL
    ),
    CHECK (
        action_type != 'schedule_task'
        OR schedule_task_json IS NOT NULL
    ),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_insight_user_surface_date ON insight (user_id, surface, date);
```

`rule_json` 예시 (`action_type='reservation'`):

```json
{
  "id": "rule_insight_42_tv_off",
  "name": "30분 뒤 TV 끄기",
  "enabled": true,
  "trigger": null,
  "schedule": { "repeat": "once", "delayMinutes": 30 },
  "action": { "deviceId": "2c9f6a1b4d78e350", "name": "off", "params": {} },
  "execMode": "once",
  "cooldownMs": 0
}
```

에이전트가 `id` 를 미리 넣을 수 있다. 적용 시 동일 `id` 가 `rules.json` 에 없으면 생성, 있으면 `409 RULE_ID_CONFLICT`.

`schedule_task_json` 예시 (`action_type='schedule_task'`):

```json
{
  "title": "저녁 스트레칭 10분",
  "category": "posture",
  "scheduleKind": "weekly",
  "dayOfWeek": "wed",
  "eventDate": null,
  "startMinute": 1200,
  "endMinute": 1210
}
```

### API 매핑 예시

| API | 조회·동작 |
|-----|----------|
| `GET /dashboard/daily-message` | `surface='dashboard_banner'`, `date=오늘`, `kind='banner'`, `ORDER BY created_at` → 최신 1건 `{ headline: title, body: text }` |
| `GET /weekly-plan/recommendations` | `surface='weekly_plan'`, `date=오늘`, `ORDER BY created_at` → `label` 로 프론트 그룹핑 |
| `GET /sleep/insights` | `surface='sleep_report'`, `date` 필터, `ORDER BY created_at` |
| `GET /posture/insights` | `surface='posture_report'`, `date` 필터, `ORDER BY created_at` |
| (추후) `GET /power/insights` | `surface='power'`, `date=오늘`, `ORDER BY created_at` |
| `POST /insights/{id}/apply` | `actionable=1` 카드 클릭 → `action_type` 에 따라 즉시 반영 (`rule_json` → `rules.json` 등), `approved=1` |
| `PATCH /insights/{id}` | (선택) 적용 취소 시 `approved=0`, `rule_json.id` 룰 삭제 + 파생 `schedule_task` 정리 |




### RAG — 인사이트별 벡터 테이블

임베딩 모델·차원은 기존과 동일(nomic-embed-text, 768). **surface 마다 별도** `vec_insight_`* 를 둔다.
검색 시 해당 컬렉션만 조회하므로 필터 축이 단순해진다.

```sql
CREATE VIRTUAL TABLE vec_insight_dashboard USING vec0 (
    insight_id INTEGER PRIMARY KEY,   -- insight.id, surface='dashboard_banner'
    embedding  float[768]
);

CREATE VIRTUAL TABLE vec_insight_weekly_plan USING vec0 (
    insight_id INTEGER PRIMARY KEY,   -- surface='weekly_plan'
    embedding  float[768]
);

CREATE VIRTUAL TABLE vec_insight_sleep USING vec0 (
    insight_id INTEGER PRIMARY KEY,   -- surface='sleep_report'
    embedding  float[768]
);

CREATE VIRTUAL TABLE vec_insight_posture USING vec0 (
    insight_id INTEGER PRIMARY KEY,   -- surface='posture_report'
    embedding  float[768]
);

CREATE VIRTUAL TABLE vec_insight_power USING vec0 (
    insight_id INTEGER PRIMARY KEY,   -- surface='power'
    embedding  float[768]
);
```

인사이트 생성·갱신 시: 해당 `surface` 의 `vec_insight_*` 에 upsert. `surface` 변경 시 이전 벡터 행 삭제.

---



## 리포트



### sleep_report (기존 — 변경 없음)

`db-schema.md` 정의 유지. `vec_sleep_report` 유지.

### posture_report (신규)

수면 `sleep_report` 와 동일한 패턴. 자세 통계·세션 집계 후 LLM 이 `report_text` 생성.

```sql
CREATE TABLE posture_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,    -- 'daily' | 'weekly'
    period_start VARCHAR(50) NOT NULL,    -- daily: 'YYYY-MM-DD', weekly: 주 시작일(월요일)
    metrics      TEXT,                    -- json: 구조화 지표(점수, 거북목 횟수, 연속 착석 등)
    report_text  TEXT,                    -- LLM 생성 리포트 본문

    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE VIRTUAL TABLE vec_posture_report USING vec0 (
    report_id INTEGER PRIMARY KEY,   -- posture_report.id
    embedding float[768]
);
```

리포트 내 권장 카드는 `insight` (`surface='posture_report'`, `date` = 리포트 `period_start`).

### power_report (기존 — 변경 없음)

`db-schema.md` 정의·`vec_power_report` 유지. 전력 권장 인사이트는 `insight` (`surface='power'`) 로 추가.

### weekly_plan_report (신규)

주간 계획 **상단 배너** 전용 narrative. (현재 mock `getWeeklyAgentReport()` 대체)

에이전트는 배너 생성 시 `db/query`·`rag/search` 툴로 `schedule_task`·수면/자세 리포트 등을 **직접 조회**한다.
구조화 `metrics` 컬럼은 두지 않는다.

```sql
CREATE TABLE weekly_plan_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period_start VARCHAR(50) NOT NULL,    -- 해당 주 월요일 'YYYY-MM-DD'
    headline     VARCHAR(100),            -- 배너 제목. NULL 이면 report_text 앞줄 사용
    report_text  TEXT NOT NULL,           -- 배너 본문 (프론트 body)
    created_at   VARCHAR(50) NOT NULL,

    UNIQUE (user_id, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE VIRTUAL TABLE vec_weekly_plan_report USING vec0 (
    report_id INTEGER PRIMARY KEY,   -- weekly_plan_report.id
    embedding float[768]
);
```

프론트 매핑: `{ headline, body: report_text }` ← 해당 주 `period_start` 행 1건.
생성: 백엔드가 `POST /weekly-plan/v1/reports` 잡을 에이전트에 요청 → 완료 후 DB upsert.

---



## 통계 — posture_stat (초안)

`posture_stat` 상세 스펙이 확정되기 전 **placeholder**. 컬럼·granularity 는 추후 조정한다.
지금은 리포트·인사이트·RAG 경로만 열어두고, raw 통계 RAG 는 **포함하지 않는다**.

```sql
-- 초안: 상세 스펙 미확정. 통합 시 주석으로 '초안' 표시 권장.
CREATE TABLE posture_stat (
    id          INTEGER     PRIMARY KEY,
    user_id     INTEGER     NOT NULL,
    granularity VARCHAR(3)  NOT NULL,    -- '1h' | '1d' (추후 확장)
    time_start  VARCHAR(50) NOT NULL,    -- 구간 시작
    time_end    VARCHAR(50),
    score       INTEGER,                 -- 자세 점수 0~100
    metrics     TEXT,                    -- json: 거북목 횟수, 연속 착석 분 등 (스펙 확정 후 정규화)

    CHECK (granularity IN ('1h', '1d')),
    UNIQUE (user_id, granularity, time_start),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
```

- `vec_posture_stat` — **이번 단계에서는 추가하지 않음** (스펙 안정 후 `sleep_stat` 패턴 참고).

---



## REST API (`/api/v1`)

> 통합 검토 후 `wave-home-front/docs/api/` 에 개별 md 로 분리하고,
> `wave-home-front/docs/api/README.md` **변동사항** 섹션에 아래 요약을 붙인다.

### README 변동사항 (통합 시 추가)

| 문서 | 상태 | 비고 |
|------|------|------|
| `iot.md` (룰) | **갱신** | 기존 mock `IotApi`·wave-server `RulesController` 와 정렬. 저장소 = `device/rules.json` |
| `alarm.md` | **유지** | DB `alarm` 테이블과 1:1. wave-server CRUD **미구현** |
| `schedule-tasks.md` | **신규** | `routine_task` → `schedule_task`, `scheduleKind`·`eventDate` 추가 |
| `insights.md` | **갱신** | `surface`/`date`/`ruleJson`/`POST …/apply` 원탭 적용 |

공통: Base URL `/api/v1`, 세션 `activeAccount` 기준, camelCase JSON, 에러 `{ "error": { "code", "message" } }`.

---



### Rules API

IoT **트리거·예약** 통합. 런타임 저장소는 SQLite가 아니라 **`device/rules.json`**
(`app_config.rules_path`, 기본 `device/rules.json`). wave-server `RuleStore` + `TriggerManager` 가 로드·실행.

- `trigger` 만 있으면 **자동화**, `schedule` 만 있으면 **예약**. 둘 다 없으면 무효.
- 스키마·타입: `docs/device-tool-api.md` 의 `Rule`, `RuleView`, `RuleSchedule`, `RuleTrigger`.

#### 구현 상태

| 엔드포인트 | wave-server |
|-----------|-------------|
| `GET/POST /iot/rules`, `PUT/DELETE /iot/rules/{ruleId}`, … | **구현됨** (`RulesController`) |
| `GET /iot/rules?hasSchedule=true` | **미구현** (클라이언트 필터 또는 추후 query 추가) |

#### GET `/iot/rules`

**Query** (선택): `deviceId` — 트리거·액션 장치 id 로 필터.

**Response 200** — `RuleView[]` (배열)

```json
[
{
    "id": "rule_schedule_tv_off_once",
  "name": "30분 뒤 TV 끄기",
  "enabled": true,
  "trigger": null,
  "schedule": { "repeat": "once", "delayMinutes": 30 },
  "action": { "deviceId": "2c9f6a1b4d78e350", "name": "off", "params": {} },
  "execMode": "once",
    "cooldownMs": 0,
    "repeatIntervalMs": 0,
    "actionDeviceName": "TV"
  }
]
```

#### POST `/iot/rules`

**Request Body** — `CreateRuleRequest` (`id` 생략 시 서버 생성)

```json
{
  "name": "플러그 과부하 차단",
  "enabled": true,
  "trigger": {
    "kind": "device_state",
    "deviceId": "1f8c5a2e7b93064d",
    "query": "power",
    "op": ">",
    "value": 1500
  },
  "schedule": null,
  "action": { "deviceId": "1f8c5a2e7b93064d", "name": "off", "params": {} },
  "execMode": "once",
  "cooldownMs": 10000
}
```

**Response 201** — `RuleView`

**Response 400** — `INVALID_RULE` (trigger·schedule 둘 다 없음, capability 불일치 등)

#### PUT `/iot/rules/{ruleId}`

부분 수정. **Response 200** — `RuleView`

#### DELETE `/iot/rules/{ruleId}`

예약 취소 = 룰 삭제. **Response 200** `{ "id": "rule_schedule_tv_off_once" }`

#### PUT `/iot/rules/{ruleId}/enabled`

```json
{ "enabled": false }
```

#### POST `/iot/rules/{ruleId}/execute`

즉시 1회 실행(테스트·수동). **Response 200** — 실행 결과 요약.

#### 인사이트와의 연동

1. 에이전트가 `insight.rule_json` 에 Rule 초안 저장 (`actionable=1`, `approved=0`).
2. 사용자 클릭 → `POST /insights/{id}/apply` → 내부적으로 `POST /iot/rules` (또는 동일 RuleStore API).
3. 성공 시 `insight.approved=1`, `insight.rule_json` 을 persist 된 본문으로 갱신.

#### 엔드포인트 요약

```http
GET    /api/v1/iot/rules
POST   /api/v1/iot/rules
PUT    /api/v1/iot/rules/{ruleId}
DELETE /api/v1/iot/rules/{ruleId}
PUT    /api/v1/iot/rules/{ruleId}/enabled
POST   /api/v1/iot/rules/{ruleId}/execute
```

프론트: `iotApi.getRules()`, `createRule()`, `updateRule()`, `deleteRule()`, `setRuleEnabled()`, `executeRule()`.

---



### Alarms API

SQLite `alarm` 테이블 CRUD. 스키마는 위 [알람](#알람) 섹션.

#### 구현 상태

wave-server **미구현**. 프론트 mock·스펙: `wave-home-front/docs/api/alarm.md`.

#### 타입

```ts
type DayOfWeek = 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun';

type AlarmMethod =
  | { type: 'light_blink'; brightness: number; intervalSec: number }
  | { type: 'light_on'; brightness: number }
  | { type: 'plug_toggle' }
  | { type: 'plug_on' }
  | { type: 'plug_off' }
  | { type: 'tts'; speakerId: number; text: string; repeatCount: number; intervalSec: number };

type Alarm = {
  id: number;
  name: string;
  timeMinute: number;           // 0~1439
  daysOfWeek: DayOfWeek[];      // [] = 1회성
  smartWake: boolean;
  radarDeviceId: string | null; // smartWake 시 필수, class='srs_r4sn'
  deviceId: string | null;
  method: AlarmMethod | null;
  enabled: boolean;
  createdAt: string;
  updatedAt: string;
};
```

#### GET `/alarms`

**Response 200** — `Alarm[]`, `timeMinute` asc.

#### POST `/alarms`

**Response 201** — 생성된 `Alarm`

#### PATCH `/alarms/{id}`

부분 수정(활성 토글 포함). **Response 200** — `Alarm`

#### DELETE `/alarms/{id}`

**Response 200** `{ "id": 1 }`

#### 엔드포인트 요약

```http
GET    /api/v1/alarms
POST   /api/v1/alarms
PATCH  /api/v1/alarms/{id}
DELETE /api/v1/alarms/{id}
```

프론트: `alarmApi.getAlarms()`, `createAlarm()`, `updateAlarm()`, `deleteAlarm()`.

---



### Schedule Tasks API

SQLite `schedule_task` CRUD. 주간 계획·대시보드 할일의 **단일 소스**.

#### 구현 상태

wave-server **미구현**. 프론트 `weeklyPlanApi` mock 대상.

#### 타입

```ts
type ScheduleKind = 'weekly' | 'once';
type DayOfWeek = 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun';
type TaskCategory = 'posture' | 'sleep' | 'diet' | 'mental' | string;

type ScheduleTask = {
  id: number;
  title: string;
  createdAt: string | null;
  createdBy: 'user' | 'agent';
  category: TaskCategory;
  scheduleKind: ScheduleKind;
  dayOfWeek: DayOfWeek;
  eventDate: string | null;     // once: 'YYYY-MM-DD', weekly: null
  startMinute: number | null;
  endMinute: number | null;
  done: boolean;
  sourceInsightId: number | null;
};
```

#### GET `/schedule-tasks`

**Query** (선택):

| 파라미터 | 설명 |
|---------|------|
| `dayOfWeek` | 요일 필터 |
| `eventDate` | `once` 일정 날짜 |
| `scheduleKind` | `weekly` \| `once` |
| `from` / `to` | `eventDate` 범위 (`once` 위주) |
| `done` | `true` \| `false` |

**Response 200** — `ScheduleTask[]`

#### POST `/schedule-tasks`

**Request Body** — 사용자 직접 추가

```json
{
  "title": "스트레칭 10분",
  "dayOfWeek": "wed",
  "startMinute": 720,
  "endMinute": 730
}
```

`category` 생략 시 서버가 `title` 기반 자동 분류(프론트 weekly-plan 관례).

**Request Body** — 인사이트에서 추가

```json
{
  "sourceInsightId": 42,
  "dayOfWeek": "mon"
}
```

`title`·`category`·`scheduleKind` 등은 인사이트에서 파생.

**Response 201** — `ScheduleTask`

#### PATCH `/schedule-tasks/{id}`

```json
{ "done": true }
```

**Response 200** — `ScheduleTask`

#### DELETE `/schedule-tasks/{id}`

**Response 200** `{ "id": 5 }`

#### 엔드포인트 요약

```http
GET    /api/v1/schedule-tasks
POST   /api/v1/schedule-tasks
PATCH  /api/v1/schedule-tasks/{id}
DELETE /api/v1/schedule-tasks/{id}
```

프론트(예정): `weeklyPlanApi.getTasks()`, `createTask()`, `updateTask()`, `deleteTask()`.
경로 별칭 `GET /weekly-plan/tasks` 는 통합 시 **하나만** 선택(권장: `/schedule-tasks`).

---



### Insights API

SQLite `insight` + surface 별 RAG. 조회는 페이지별 thin wrapper 또는 공통 CRUD.

#### 타입

```ts
type InsightSurface =
  | 'dashboard_banner'
  | 'weekly_plan'
  | 'sleep_report'
  | 'posture_report'
  | 'power';

type InsightKind = 'banner' | 'action' | 'goal' | 'tip';
type InsightActionType = 'schedule_task' | 'automation_rule' | 'reservation';

type Insight = {
  id: number;
  surface: InsightSurface;
  kind: InsightKind;
  date: string;              // 'YYYY-MM-DD'
  label: string | null;
  title: string;
  text: string;
  actionable: boolean;
  actionType: InsightActionType | null;
  approved: boolean;
  ruleJson: Rule | null;
  scheduleTaskJson: ScheduleTaskDraft | null;
  createdAt: string;
};
```

`Rule` — [Rules API](#rules-api). `ScheduleTaskDraft` — `schedule_task` 생성 필드(`title`, `category`, `scheduleKind`, `dayOfWeek`, `eventDate`, `startMinute`, `endMinute`).

#### GET `/insights`

**Query**: `surface`, `date`, `kind`, `approved`, `actionable`

**Response 200** — `Insight[]`, `createdAt` asc.

#### GET `/insights/{id}`

**Response 200** — `Insight`

#### POST `/insights/{id}/apply`

`actionable=1` 이고 `approved=0` 인 카드 **원탭 적용**.

| `actionType` | 동작 |
|--------------|------|
| `schedule_task` | `insight.scheduleTaskJson` → `POST /schedule-tasks` |
| `automation_rule` | `insight.ruleJson` → `POST /iot/rules` → `rules.json` |
| `reservation` | 동일 (`schedule` 필드 있는 Rule) |

**Response 200** — `derivedScheduleTaskId` (schedule_task 적용 시), `ruleJson` (룰 적용 시 최종본)

**Response 409** — `ALREADY_APPLIED` | `RULE_ID_CONFLICT`

#### PATCH `/insights/{id}`

적용 취소(선택):

```json
{ "approved": false }
```

→ `ruleJson.id` 룰 `DELETE /iot/rules/{ruleId}`, 파생 `schedule_task` 삭제.

#### 페이지별 thin wrapper (기존 프론트 호환)

| Wrapper | 내부 조회 |
|---------|----------|
| `GET /dashboard/daily-message` | `surface=dashboard_banner`, `date=오늘`, `kind=banner`, limit 1 |
| `GET /weekly-plan/recommendations` | `surface=weekly_plan`, `date=오늘` |
| `GET /sleep/insights?date=` | `surface=sleep_report` |
| `GET /posture/insights?date=` | `surface=posture_report` |

#### 엔드포인트 요약

```http
GET    /api/v1/insights
GET    /api/v1/insights/{id}
POST   /api/v1/insights/{id}/apply
PATCH  /api/v1/insights/{id}
```

---



## API 변경 메모

> 아래는 `docs/agent-api/db-query-api.md`, `docs/agent-api/rag-api.md` 에 반영할 **변경 요약**이다.
> 통합 검토 후 각 파일 본문을 갱신한다.



### db-query-api.md



#### DbTable 변경

```diff
  type DbTable =
    ...
-   | 'routine_task' | 'alarm' | 'notification' | 'chat_history' | 'insight';
+   | 'schedule_task' | 'alarm' | 'notification' | 'chat_history' | 'insight'
+   | 'posture_stat' | 'posture_report' | 'weekly_plan_report';
```

(`sleep_report`, `power_report` 는 기존 유지)

#### 테이블별 변경


| 테이블                  | 변경                                                                                                                                    |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `routine_task`       | **삭제** → `schedule_task` 로 대체. 필터에 `scheduleKind`, `eventDate`, `from`/`to`(eventDate) 유지                                             |
| `insight`            | 필터: `surface`, `kind`, `date`, `actionable`, `actionType`, `approved`. `ruleJson`·`scheduleTaskJson` 파싱 JSON. 정렬 `createdAt` asc |
| `posture_stat`       | **신규**. `userId` 필수, `granularity`, `from`/`to`(`timeStart`). 초안                                            |
| `posture_report`     | **신규**. `sleep_report` 와 동일 필터(`userId`, `period`, `periodStart`, `from`/`to`)                        |
| `weekly_plan_report` | **신규**. `userId` 필수, `periodStart`, `from`/`to`(`periodStart`). `metrics` 없음                          |
| `alarm`              | 기존 초안 유지                                                                                                                              |




#### insight 조회 예시 (신규)

```json
{
  "table": "insight",
  "filter": {
  "userId": 1,
    "surface": "weekly_plan",
    "date": "2026-07-08",
    "approved": 0
  },
  "order": "asc"
}
```

```json
{
  "table": "insight",
  "filter": {
    "userId": 1,
    "surface": "dashboard_banner",
    "date": "2026-07-08",
    "kind": "banner"
  },
  "limit": 1,
  "order": "asc"
}
```

```json
{
  "table": "insight",
  "filter": {
    "userId": 1,
    "surface": "sleep_report",
    "date": "2026-07-08"
  }
}
```



#### schedule_task 조회 예시

```json
{
  "table": "schedule_task",
  "filter": {
    "userId": 1,
    "scheduleKind": "once",
    "from": "2026-07-07",
    "to": "2026-07-14"
  }
}
```

---



### rag-api.md



#### 컬렉션 추가·변경

기존 `sleep_report`, `sleep_stat`, `power_report` 유지. 아래 **추가**:


| collection            | vec 테이블                   | 필터 축                                                  | 비고  |
| --------------------- | ------------------------- | ----------------------------------------------------- | --- |
| `posture_report`      | `vec_posture_report`      | `userId`, `period`, `from`/`to`                       | 신규  |
| `weekly_plan_report`  | `vec_weekly_plan_report`  | `userId`, `from`/`to`(`periodStart`)                  | 신규  |
| `insight_dashboard`   | `vec_insight_dashboard`   | `userId`, `date`, `from`/`to`(`date`)     | 신규  |
| `insight_weekly_plan` | `vec_insight_weekly_plan` | `userId`, `date`, `from`/`to`             | 신규  |
| `insight_sleep`       | `vec_insight_sleep`       | `userId`, `date`, `from`/`to`(`date`)     | 신규  |
| `insight_posture`     | `vec_insight_posture`     | `userId`, `date`, `from`/`to`(`date`)     | 신규  |
| `insight_power`       | `vec_insight_power`       | `userId`, `date`, `from`/`to`             | 신규  |


`collection` 이름은 RAG API 의 `targets[].collection` 과 1:1. DB `insight.surface` 와는
`insight_*` 접두로 대응 (`dashboard_banner` → `insight_dashboard`).

#### 요청 예시 추가 (rag-api 통합 시)

```json
{
  "query": "이번 주 전력을 줄이려면?",
  "targets": [
    { "collection": "insight_power", "userId": 1, "date": "2026-07-08", "topK": 3 },
    { "collection": "power_report", "deviceId": null, "period": "24h", "from": "2026-07-01", "to": "2026-07-08", "topK": 2 },
    { "collection": "weekly_plan_report", "userId": 1, "from": "2026-06-29", "to": "2026-07-08", "topK": 1 }
  ]
}
```



#### Chat `context.retrieved` 평탄화

기존 3종(`sleep_report`, `sleep_stat`, `power_report`)에 위 컬렉션 추가 가능.
인사이트 hit 의 `text` 는 `title + text` 조합 스니펫.

---

## 폐기·주의

| 항목 | 처리 |
|------|------|
| `routine_task` | **삭제** → `schedule_task` |
| `insight.domain` | **삭제** → `surface` |
| `insight.period` / `period_start` / `report_id` | **삭제** → `date` |
| `insight.group_key` / `sort_order` / `updated_at` | **삭제** |
| `insight.rule_id` / `schedule_task` title 파생 | **삭제** → `rule_json` + `schedule_task_json` |
| `weekly_plan_report.metrics` | **삭제** — 에이전트가 `db/query` 로 조회 |
| `insight.rule_json` | **신규** — Rule 전체 JSON |
| `insight.schedule_task_json` | **신규** — schedule_task 초안 JSON |
| `insight.action_type` + `POST /insights/{id}/apply` | **신규** — 원탭 적용 |
| 룰 런타임 저장 | SQLite `automation_rule` **아님** → `device/rules.json` |
