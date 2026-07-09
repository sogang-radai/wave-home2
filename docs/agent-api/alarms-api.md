# Alarms API (에이전트 → 백엔드)

호출 방향: **에이전트(:8501) → 백엔드(:8500)**

> **프론트 API와 분리**: 웹 UI는 `/api/v1/alarms` ([`wave-home-front/docs/api/alarm.md`](../../wave-home-front/docs/api/alarm.md)). 세션 `activeAccount` 기준.
> 이 문서는 에이전트가 **명시적 `userId`** 로 알람 설정을 조회·변경할 때 쓴다.

- DB 테이블: `alarm` (`docs/db-schema.md`)
- 수면·기상 맞춤 분석 시 **조회만** 필요하면 [`db-query-api.md`](./db-query-api.md) 의 `table: alarm` 이 더 효율적이다.
- 알람 **실행**(조명·TTS·기상 맞춤 판단)은 이 API 범위가 아니다. 사용자 설정 CRUD만 다룬다.
- 구현 상태: **미구현** (스펙은 프론트 [`alarm.md`](../../wave-home-front/docs/api/alarm.md) 와 동일)

## 타입

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
  userId: number;
  name: string;
  timeMinute: number;            // 0~1439
  daysOfWeek: DayOfWeek[];       // [] = 1회성
  smartWake: boolean;
  radarDeviceId: string | null;  // smartWake=true 일 때 필수
  deviceId: string | null;
  method: AlarmMethod | null;
  enabled: boolean;
  createdAt: string;
  updatedAt: string;
};
```

`deviceId`·`radarDeviceId` 는 API에서 16자리 hex 외부 id (`device.external_id`).

## GET `/internal/v1/alarms`

**Query**

| 파라미터 | 필수 | 설명 |
|---------|------|------|
| `userId` | **필수** | 사용자 id |
| `enabled` | — | `true` \| `false` |

**Response 200** — `Alarm[]`, `timeMinute` 오름차순

## POST `/internal/v1/alarms`

**Request Body**

```json
{
  "userId": 1,
  "name": "평일 기상",
  "timeMinute": 420,
  "daysOfWeek": ["mon", "tue", "wed", "thu", "fri"],
  "smartWake": true,
  "radarDeviceId": "3a7f2c9d10b4e85f",
  "deviceId": "5c1e8b6402fda973",
  "method": { "type": "tts", "speakerId": 0, "text": "좋은 아침이에요!", "repeatCount": 3, "intervalSec": 20 },
  "enabled": true
}
```

**Response 201** — 생성된 `Alarm`

## PATCH `/internal/v1/alarms/{id}`

**Query**: `userId` (**필수**)

**Request Body** — 부분 수정

```json
{ "enabled": false }
```

**Response 200** — 갱신된 `Alarm`

## DELETE `/internal/v1/alarms/{id}`

**Query**: `userId` (**필수**)

**Response 200** `{ "id": 1 }`

## 엔드포인트 요약

```http
GET    /internal/v1/alarms?userId={userId}
POST   /internal/v1/alarms
PATCH  /internal/v1/alarms/{id}?userId={userId}
DELETE /internal/v1/alarms/{id}?userId={userId}
```

## 프론트·에이전트 매핑

| 용도 | 프론트 (`/api/v1`) | 에이전트 (`/internal/v1`) |
|------|-------------------|---------------------------|
| 알람 CRUD | `alarms` | `alarms` (`userId` 명시) |
| 배치 조회(수면 맥락) | `GET alarms` | `POST db/query` (`alarm`) |
