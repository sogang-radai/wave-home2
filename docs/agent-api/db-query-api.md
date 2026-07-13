# DB 조회 API

호출 방향: **에이전트(:8501) → 백엔드(:8500)** · **POST** `/internal/v1/db/query`

에이전트가 SQLite 에 직접 접근하지 않고, **백엔드에 배치 조회를 위임**한다.
한 HTTP 요청에 여러 테이블 조회를 `queries[]` 로 묶어 보내고, `results[]` 로 1:1 받는다.
URL 쿼리스트링 대신 JSON Body 를 쓰므로 복잡한 필터·길이 제한 문제를 피한다.

### 공통

- **POST** `/internal/v1/db/query`
- `queries[]` 최대 **10**개. 각 query 의 `limit` 기본 **100**, 상한 **1000**.
- 요청 `queries[i]` 와 응답 `results[i]` 가 **1:1 대응**한다.
- 시간 범위 `from`/`to`는 **반열림** `[from, to)` 이다. 해당 테이블의 시간 컬럼 기준(아래 표 참고).
- `filter` 에 허용되지 않은 키·값이 있으면 해당 query 만 `error` 로 실패하고, 나머지 query 는 계속 처리한다.
- 응답 row 는 분석 API 타입과 동일하게 **camelCase** 필드명을 쓴다(`db-schema.md` 컬럼 대응).
- `vec_*` 가상 테이블은 조회 대상이 **아니다**(벡터 검색은 RAG API 사용).
- 공통 에러(전체 요청): `{ "error": { "code": "...", "message": "..." } }` — `queries` 누락, 배열 초과 등.



### 타입

```ts
type DbQueryRequest = {
  queries: DbQuery[];
};

type DbQuery = {
  table: DbTable;
  filter: Record<string, unknown>;   // 테이블별 허용 필터(아래 참고)
  limit?: number;                    // 기본 100, 최대 1000
  order?: 'asc' | 'desc';            // 시간/날짜 정렬. 기본값은 테이블별(아래 참고)
};

type DbTable =
  | 'user' | 'room' | 'room_user_map'
  | 'device' | 'device_user_map' | 'device_room_map'
  | 'sleep_session' | 'sleep_stat' | 'sleep_report'
  | 'power_energy' | 'power_report'
  | 'posture_stat' | 'posture_report' | 'weekly_plan_report'
  | 'gesture_set' | 'gesture_log'
  | 'schedule_task' | 'alarm' | 'automation_rule' | 'notification' | 'chat_history' | 'insight';

type DbQueryResult = {
  table: DbTable;
  items: object[];
  count: number;
  error?: { code: string; message: string; field?: string };
};
```



### POST `/internal/v1/db/query`

**Request Body** — 배치 예시

```json
{
  "queries": [
    {
      "table": "sleep_stat",
      "filter": {
        "userId": 1,
        "granularity": "30m",
        "from": "2026-07-01 00:00:00",
        "to": "2026-07-02 00:00:00"
      },
      "limit": 48,
      "order": "asc"
    },
    {
      "table": "power_energy",
      "filter": {
        "deviceId": null,
        "granularity": "1h",
        "from": "2026-07-01 00:00:00",
        "to": "2026-07-02 00:00:00"
      },
      "limit": 24
    },
    {
      "table": "device",
      "filter": { "roomId": 2, "archived": 0 }
    }
  ]
}
```

**Response 200**

```json
{
  "results": [
    {
      "table": "sleep_stat",
      "count": 48,
      "items": [
        {
          "id": 4120, "userId": 1, "roomId": 1, "sessionId": 88,
          "granularity": "30m", "timeStart": "2026-07-01 03:00:00", "timeEnd": "2026-07-01 03:30:00",
          "coverage": 0.97, "stageLabel": "light", "hrMean": 62.0, "snoreRatio": 0.18
        }
      ]
    },
    {
      "table": "power_energy",
      "count": 24,
      "items": [
        {
          "id": 20401, "deviceId": null, "deviceName": null, "granularity": "1h",
          "timeStart": "2026-07-01 22:00:00", "energyWh": 1180.4, "coverage": 0.98, "sampleCount": 12
        }
      ]
    },
    {
      "table": "device",
      "count": 3,
      "items": [
        { "id": 7714208883279181, "name": "거실 에어컨", "class": "tuya_ep2h", "archived": 0 }
      ]
    }
  ]
}
```

**Response 200** — query 하나 실패(나머지는 성공)

```json
{
  "results": [
    {
      "table": "sleep_stat",
      "count": 0,
      "items": [],
      "error": { "code": "INVALID_FILTER", "message": "userId 는 필수입니다.", "field": "userId" }
    }
  ]
}
```

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "queries 는 1~10개여야 합니다.",
    "field": "queries"
  }
}
```



### 테이블별 허용 필터

공통 키 설명:

| 키 | 의미 |
|----|------|
| `id` | PK exact match |
| `from` / `to` | 해당 테이블 시간·날짜 컬럼 기준 `[from, to)` |
| `limit` | query 수준에서 지정(위 `DbQuery.limit`) |

`deviceId` 특수 규칙(전력 테이블):

- 키 **생략** — 모든 장치 + 합산행 모두(필터 없음)
- **`null`** — `device_id IS NULL` 합산행만
- **정수** — 해당 장치만



#### `user`

| 필터 | 필수 | 설명 |
|------|------|------|
| `id` | — | 사용자 id |

시간축 없음. 기본 정렬: `id` asc.

```json
{ "table": "user", "filter": { "id": 1 } }
```



#### `room`

| 필터 | 필수 | 설명 |
|------|------|------|
| `id` | — | 방 id |
| `userId` | — | `room_user_map` 조인 — 해당 사용자가 속한 방만 |

기본 정렬: `id` asc.

```json
{ "table": "room", "filter": { "userId": 1 } }
```



#### `room_user_map`

| 필터 | 필수 | 설명 |
|------|------|------|
| `roomId` | * | 방 id |
| `userId` | * | 사용자 id |

\* `roomId`·`userId` 중 **최소 1개** 필수.

```json
{ "table": "room_user_map", "filter": { "userId": 1 } }
```



#### `device`

| 필터 | 필수 | 설명 |
|------|------|------|
| `id` | — | 장치 id |
| `class` | — | 장치 클래스(`tuya_ep2h`, `philips_wiz_e29` 등) |
| `archived` | — | `0`=활성, `1`=보관. **기본 0**(활성만) |
| `roomId` | — | `device_room_map` 조인 |
| `userId` | — | `device_user_map` 조인 |

기본 정렬: `id` asc.

```json
{ "table": "device", "filter": { "roomId": 2, "archived": 0 } }
```



#### `device_user_map`

| 필터 | 필수 | 설명 |
|------|------|------|
| `deviceId` | * | 장치 id |
| `userId` | * | 사용자 id |

\* 최소 1개 필수.

```json
{ "table": "device_user_map", "filter": { "userId": 1 } }
```



#### `device_room_map`

| 필터 | 필수 | 설명 |
|------|------|------|
| `deviceId` | * | 장치 id |
| `roomId` | * | 방 id |

\* 최소 1개 필수.

```json
{ "table": "device_room_map", "filter": { "roomId": 2 } }
```



#### `sleep_session`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 세션 id |
| `roomId` | — | 방 id |
| `nightDate` | — | 온셋 기준 날짜 exact `'YYYY-MM-DD'` |
| `from` / `to` | — | `onset` 기준 `[from, to)` |

기본 정렬: `nightDate` desc, `onset` desc.

```json
{
  "table": "sleep_session",
  "filter": { "userId": 1, "nightDate": "2026-07-01" }
}
```

```json
{
  "table": "sleep_session",
  "filter": {
    "userId": 1,
    "from": "2026-06-29",
    "to": "2026-07-06"
  },
  "limit": 7
}
```



#### `sleep_stat`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | stat id |
| `sessionId` | — | 수면 세션 id |
| `roomId` | — | 방 id |
| `granularity` | — | `'1m'` \| `'30m'` |
| `from` / `to` | — | `timeStart` 기준 `[from, to)` |

기본 정렬: `timeStart` asc.

```json
{
  "table": "sleep_stat",
  "filter": {
    "userId": 1,
    "sessionId": 88,
    "granularity": "30m"
  }
}
```

```json
{
  "table": "sleep_stat",
  "filter": {
    "userId": 1,
    "granularity": "1m",
    "from": "2026-07-01 03:00:00",
    "to": "2026-07-01 03:30:00"
  },
  "order": "asc"
}
```



#### `sleep_report`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 리포트 id |
| `period` | — | `'daily'` \| `'weekly'` |
| `periodStart` | — | exact — daily: `'YYYY-MM-DD'`, weekly: 롤링 7일 창 **첫날** `'YYYY-MM-DD'` |
| `from` / `to` | — | `periodStart` 기준 `[from, to)` |

기본 정렬: `periodStart` desc.

```json
{
  "table": "sleep_report",
  "filter": { "userId": 1, "period": "daily", "periodStart": "2026-07-01" }
}
```

```json
{
  "table": "sleep_report",
  "filter": {
    "userId": 1,
    "period": "weekly",
    "from": "2026-06-01",
    "to": "2026-07-01"
  },
  "limit": 4
}
```



#### `power_energy`

| 필터 | 필수 | 설명 |
|------|------|------|
| `deviceId` | — | 생략=전체, `null`=합산행, 정수=해당 장치 |
| `id` | — | energy id |
| `granularity` | — | `'5m'` \| `'1h'` \| `'24h'` \| `'1w'` \| `'1mo'` |
| `from` / `to` | — | `timeStart` 기준 `[from, to)` |
| `roomId` | — | `device_room_map` 조인(해당 방 장치 + 합산은 미포함) |
| `userId` | — | `device_user_map` 조인 |

기본 정렬: `timeStart` asc.

```json
{
  "table": "power_energy",
  "filter": {
    "deviceId": null,
    "granularity": "24h",
    "from": "2026-06-27",
    "to": "2026-07-04"
  }
}
```

```json
{
  "table": "power_energy",
  "filter": {
    "deviceId": 7714208883279181,
    "granularity": "1h",
    "from": "2026-07-01 00:00:00",
    "to": "2026-07-02 00:00:00"
  },
  "limit": 24
}
```

응답 행에는 `deviceId`와 함께 `deviceName`(장치 표시 이름)이 포함됩니다. 합산행(`deviceId: null`)은 `deviceName`도 null입니다.



#### `power_report`

| 필터 | 필수 | 설명 |
|------|------|------|
| `deviceId` | — | 생략=전체, `null`=합산, 정수=해당 장치 |
| `id` | — | 리포트 id |
| `energyId` | — | 원본 `power_energy.id` |
| `period` | — | `'1h'` \| `'24h'` \| `'1w'` \| `'1mo'` |
| `periodStart` | — | exact |
| `from` / `to` | — | `periodStart` 기준 `[from, to)` |
| `roomId` | — | `device_room_map` 조인 |
| `userId` | — | `device_user_map` 조인 |

기본 정렬: `periodStart` desc.

```json
{
  "table": "power_report",
  "filter": {
    "deviceId": null,
    "period": "24h",
    "periodStart": "2026-07-01"
  }
}
```



#### `gesture_set`

| 필터 | 필수 | 설명 |
|------|------|------|
| `id` | — | 세트 id |
| `archived` | — | `0` \| `1`. **기본 0** |

기본 정렬: `id` asc.

```json
{ "table": "gesture_set", "filter": { "archived": 0 } }
```



#### `gesture_log`

| 필터 | 필수 | 설명 |
|------|------|------|
| `gestureSetId` | — | 제스처 세트 id |
| `radarId` | — | 레이더 장치 id |
| `deviceId` | — | 제어된 대상 장치 id |
| `classId` | — | 세트 내 class_id |
| `from` / `to` | — | `timestamp` 기준 `[from, to)` |

기본 정렬: `timestamp` desc.

```json
{
  "table": "gesture_log",
  "filter": {
    "radarId": 7714208883279181,
    "from": "2026-07-01 00:00:00",
    "to": "2026-07-02 00:00:00"
  },
  "limit": 50
}
```



#### `schedule_task`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 할 일 id |
| `category` | — | `'posture'` \| `'sleep'` \| `'diet'` \| `'mental'` \| … |
| `scheduleKind` | — | `'weekly'` \| `'once'`. 생략 시 둘 다 조회 |
| `dayOfWeek` | — | `'mon'`…`'sun'` |
| `eventDate` | — | exact `'YYYY-MM-DD'` (`schedule_kind='once'`) |
| `from` / `to` | — | `eventDate` 기준 `[from, to)` — **once 일정** 기간 조회용 |
| `done` | — | `0` \| `1` |
| `createdBy` | — | `'user'` \| `'agent'` |
| `sourceInsightId` | — | 파생 인사이트 id |

- `weekly` 행은 `eventDate`가 NULL. `once` 행만 `eventDate`를 가진다.
- 기본 정렬: `scheduleKind` asc, `eventDate` asc, `dayOfWeek` asc, `startMinute` asc.

```json
{
  "table": "schedule_task",
  "filter": { "userId": 1, "dayOfWeek": "mon", "scheduleKind": "weekly", "done": 0 }
}
```

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



#### `alarm`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 알람 id |
| `enabled` | — | `0` \| `1` |
| `smartWake` | — | `0` \| `1` |
| `deviceId` | — | 알람 실행 장치 id |
| `radarDeviceId` | — | 기상 맞춤 레이더 장치 id |
| `from` / `to` | — | `updatedAt` 기준 `[from, to)` |

기본 정렬: `timeMinute` asc, `id` asc.

응답 row: `daysOfWeek`(JSON 파싱 배열), `method`(JSON 객체), `smartWake`/`enabled`(boolean) 등
camelCase. 스키마는 `docs/db-schema.md` 의 `alarm` 테이블 참고.

```json
{
  "table": "alarm",
  "filter": { "userId": 1, "enabled": 1 }
}
```

```json
{
  "table": "alarm",
  "filter": { "userId": 1, "smartWake": 1 },
  "limit": 20
}
```



#### `automation_rule`

IoT 트리거·예약 룰. API `ruleId` = DB `external_id`. 룰 **쓰기**는 [`device-tool-api.md`](./device-tool-api.md) 의 `/internal/v1/rules` 사용.

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | PK |
| `externalId` | — | API 룰 id exact (예: `rule_schedule_tv_off_once`) |
| `enabled` | — | `0` \| `1` |
| `hasTrigger` | — | `1` — `trigger_json` 이 NULL 이 아닌 행(자동화) |
| `hasSchedule` | — | `1` — `schedule_json` 이 NULL 이 아닌 행(예약) |
| `from` / `to` | — | `updatedAt` 기준 `[from, to)` |

기본 정렬: `updatedAt` desc, `id` asc.

응답 row: `triggerJson`·`scheduleJson`·`actionsJson` 은 TEXT 컬럼을 파싱한 JSON. `enabled` 는 boolean.
`actionsJson` 구조는 `device-tool-api.md` 의 `RuleAction` + `execMode`·`repeatIntervalMs`.

```json
{
  "table": "automation_rule",
  "filter": { "userId": 1, "hasSchedule": 1, "enabled": 1 }
}
```

```json
{
  "table": "automation_rule",
  "filter": { "userId": 1, "externalId": "rule_schedule_tv_off_once" }
}
```



#### `notification`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 알림 id |
| `type` | — | `'timer'` \| `'sleep'` \| `'posture'` \| … |
| `read` | — | `0`=안읽음, `1`=읽음 |
| `from` / `to` | — | `createdAt` 기준 `[from, to)` |

기본 정렬: `createdAt` desc.

```json
{
  "table": "notification",
  "filter": { "userId": 1, "read": 0 },
  "limit": 20
}
```



#### `chat_history`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 대화 세션 id |
| `from` / `to` | — | `createdAt` 기준 `[from, to)` |

기본 정렬: `createdAt` desc. `message` 필드는 json 전체를 반환한다(용량 주의, `limit` 권장).

```json
{
  "table": "chat_history",
  "filter": { "userId": 1 },
  "limit": 5
}
```



#### `insight`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `id` | — | 인사이트 id |
| `surface` | — | `'dashboard_banner'` \| `'weekly_plan'` \| `'sleep_report'` \| `'posture_report'` \| `'power'` |
| `kind` | — | `'banner'` \| `'action'` \| `'goal'` \| `'tip'` |
| `date` | — | 발행일 exact `'YYYY-MM-DD'` |
| `actionable` | — | `0` \| `1` |
| `actionType` | — | `'schedule_task'` \| `'automation_rule'` \| `'reservation'` |
| `approved` | — | `0` \| `1` |
| `from` / `to` | — | `date` 기준 `[from, to)` |

기본 정렬: `createdAt` asc.

응답 row: `ruleJson`·`scheduleTaskJson` 은 TEXT 컬럼을 파싱한 JSON 객체.

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
    "surface": "sleep_report",
    "date": "2026-07-08"
  }
}
```



#### `posture_stat`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `granularity` | — | `'1h'` \| `'1d'` |
| `from` / `to` | — | `timeStart` 기준 `[from, to)` |

기본 정렬: `timeStart` asc. **스펙 초안** — 컬럼은 `db-schema.md` 참고.

```json
{
  "table": "posture_stat",
  "filter": {
    "userId": 1,
    "granularity": "1d",
    "from": "2026-07-01",
    "to": "2026-07-08"
  }
}
```



#### `posture_report`

`sleep_report` 와 동일 필터 패턴.

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `period` | — | `'daily'` \| `'weekly'` |
| `periodStart` | — | exact |
| `from` / `to` | — | `periodStart` 기준 `[from, to)` |

```json
{
  "table": "posture_report",
  "filter": { "userId": 1, "period": "weekly", "periodStart": "2026-06-29" }
}
```



#### `weekly_plan_report`

| 필터 | 필수 | 설명 |
|------|------|------|
| `userId` | **필수** | 사용자 id |
| `periodStart` | — | 해당 주 월요일 exact `'YYYY-MM-DD'` |
| `from` / `to` | — | `periodStart` 기준 `[from, to)` |

기본 정렬: `periodStart` desc.

```json
{
  "table": "weekly_plan_report",
  "filter": { "userId": 1, "periodStart": "2026-06-29" }
}
```



### 전체 엔드포인트 요약

```http
POST /internal/v1/db/query
```



### 백엔드 연동 지점

- 에이전트(LangGraph tool)가 챗 턴·수면/전력 분석 보강 시 호출한다.
- 백엔드는 `filter` 키를 테이블별 허용 목록으로 검증하고, camelCase row 를 반환한다.
- `vec_*` 조회는 거부하고 RAG API 로 안내한다.



### LangGraph DB 조회 예시

```python
import httpx
from langchain_core.tools import tool

BACKEND = "http://127.0.0.1:8500/internal/v1"

@tool
def query_db(queries: list[dict]) -> list[dict]:
    """백엔드 DB에서 테이블 데이터를 배치 조회한다. queries: [{table, filter, limit?}, ...]"""
    r = httpx.post(f"{BACKEND}/db/query", json={"queries": queries}, timeout=5.0)
    r.raise_for_status()
    return r.json()["results"]   # [{table, count, items, error?}, ...]

# 예: 수면 30m + 거실 장치 목록을 한 번에
results = query_db.invoke([
    {"table": "sleep_stat", "filter": {"userId": 1, "granularity": "30m",
     "from": "2026-07-01 00:00:00", "to": "2026-07-02 00:00:00"}, "limit": 48},
    {"table": "device", "filter": {"roomId": 2, "archived": 0}},
])
llm_with_tools = llm.bind_tools([query_db, control_device])
```
