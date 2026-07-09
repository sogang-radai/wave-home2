# Schedule Tasks API (에이전트 → 백엔드)

호출 방향: **에이전트(:8501) → 백엔드(:8500)**

> **프론트 API와 분리**: 웹 UI는 `/api/v1/schedule-tasks` ([`wave-home-front/docs/api/schedule-tasks.md`](../../wave-home-front/docs/api/schedule-tasks.md)). 세션 `activeAccount` 기준.
> 이 문서는 에이전트가 챗·일정 변경 툴에서 **명시적 `userId`** 로 CRUD할 때 쓴다.

- DB 테이블: `schedule_task` (`docs/db-schema.md`)
- **배치 조회**만 필요하면 [`db-query-api.md`](./db-query-api.md) 의 `table: schedule_task` 가 더 효율적이다. 이 REST API는 **생성·수정·삭제**용.
- 구현 상태: **미구현** (스펙은 프론트 mock·[`schedule-tasks.md`](../../wave-home-front/docs/api/schedule-tasks.md) 와 동일)

## 타입

```ts
type ScheduleKind = 'weekly' | 'once';
type DayOfWeek = 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun';

type ScheduleTask = {
  id: number;
  userId: number;
  title: string;
  createdAt: string | null;
  createdBy: 'user' | 'agent';
  category: string;
  scheduleKind: ScheduleKind;
  dayOfWeek: DayOfWeek;
  eventDate: string | null;     // once: 'YYYY-MM-DD'
  startMinute: number | null;
  endMinute: number | null;
  done: boolean;
  sourceInsightId: number | null;
};
```

## GET `/internal/v1/schedule-tasks`

**Query**

| 파라미터 | 필수 | 설명 |
|---------|------|------|
| `userId` | **필수** | 사용자 id |
| `dayOfWeek` | — | 요일 필터 |
| `eventDate` | — | `once` 일정 날짜 `'YYYY-MM-DD'` |
| `scheduleKind` | — | `weekly` \| `once` |
| `from` / `to` | — | `eventDate` 기준 `[from, to)` (`once` 위주) |
| `done` | — | `true` \| `false` |

**Response 200** — `ScheduleTask[]`

## POST `/internal/v1/schedule-tasks`

에이전트가 일정을 추가할 때 `createdBy: 'agent'` 로 저장한다.

**Request Body** — 직접 추가

```json
{
  "userId": 1,
  "title": "저녁 스트레칭 10분",
  "category": "posture",
  "scheduleKind": "weekly",
  "dayOfWeek": "wed",
  "startMinute": 1200,
  "endMinute": 1210
}
```

**Request Body** — 1회성 일정

```json
{
  "userId": 1,
  "title": "병원 예약",
  "category": "posture",
  "scheduleKind": "once",
  "dayOfWeek": "mon",
  "eventDate": "2026-07-14",
  "startMinute": 1140,
  "endMinute": 1170
}
```

**Response 201** — 생성된 `ScheduleTask`

## PATCH `/internal/v1/schedule-tasks/{id}`

**Query**: `userId` (**필수**)

**Request Body** — 예: 완료 처리

```json
{ "done": true }
```

**Response 200** — 갱신된 `ScheduleTask`

**Response 404** — `{ "error": { "code": "NOT_FOUND", "message": "..." } }`

## DELETE `/internal/v1/schedule-tasks/{id}`

**Query**: `userId` (**필수**)

**Response 200** `{ "id": 5 }`

## 엔드포인트 요약

```http
GET    /internal/v1/schedule-tasks?userId={userId}
POST   /internal/v1/schedule-tasks
PATCH  /internal/v1/schedule-tasks/{id}?userId={userId}
DELETE /internal/v1/schedule-tasks/{id}?userId={userId}
```

## 프론트·에이전트 매핑

| 용도 | 프론트 (`/api/v1`) | 에이전트 (`/internal/v1`) |
|------|-------------------|---------------------------|
| 주간 반복·1회 일정 CRUD | `schedule-tasks` | `schedule-tasks` (`userId` 명시) |
| 배치 조회 | `GET schedule-tasks` | `POST db/query` (`schedule_task`) |
| 인사이트 원탭 적용 | `POST /insights/{id}/apply` | — (프론트 전용 UX) |

## LangGraph 예시

```python
import httpx

BACKEND = "http://127.0.0.1:8500/internal/v1"

async def update_schedule_task(user_id: int, task_id: int, **fields) -> dict:
    async with httpx.AsyncClient() as client:
        r = await client.patch(
            f"{BACKEND}/schedule-tasks/{task_id}",
            params={"userId": user_id},
            json=fields,
        )
        r.raise_for_status()
        return r.json()
```
