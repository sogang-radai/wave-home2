## Device Tool API

에이전트가 **장치 목록·기능(capability)·실시간 상태 조회**, **동작 실행**, **자동화 룰·예약 관리**를
백엔드에 위임하는 API.

호출 방향: **에이전트(:8501) → 백엔드(:8500)**

### 설계 원칙

1. **장치 런타임 모델 일치** — `device.h` 의 `Queryable` / `Actionable` 과 동일한 개념을 쓴다.
  - `actions[]`: 실행 가능한 동작 (`name`, `attributes`, `paramsSchema`)
  - `queries[]`: 조회 가능한 상태 (`name`, `paramsSchema`)
  - `attributes`: `Toggle` | `Repeat` | `Momentary` | `Stateful` (`Action::Attribute` 비트 플래그)
  - `Momentary` 는 룰 엔진(홀드 트리거) 전용이며 REST `execMode` 로는 노출하지 않는다.
2. **룰·예약 통합** — `bin/device/rules.json` 과 동일한 **Rule** 모델 하나로 자동화(trigger)와 예약(schedule)을 모두 표현한다.
  - `trigger` 만 있으면 자동화, `schedule` 만 있으면 예약, 둘 다 없으면 무효.
3. **장치 ID** — 런타임 API는 `device_list.json` 과 동일한 **16자리 소문자 hex 문자열**을 쓴다
  (`deviceIDToString` / `parseDeviceID`). DB `device.id`(INTEGER)와 1:1 대응한다.
4. **에이전트 편의 레이어** — LLM 툴은 `roomId` + 장치 이름으로 해석하는 고수준 API(`tools/device.`*)를 쓰고,
  내부적으로는 아래 REST를 호출한다. 방·장치 이름 → id 해석은 `device_room_map` + `device` 테이블(`db/query`)을 쓴다.
5. **DB 조회와 역할 분리** — 등록 메타·과거 로그 배치 조회는 기존 `POST /internal/v1/db/query` 를 쓴다.
  이 API는 **실시간 제어·상태·룰 실행** 전용이다.
6. **필드명 통일** — 요청·응답 모두 `device` / `params` 를 쓴다(`appliance`, `props` 사용 안 함).



### 공통

- Base URL: `/internal/v1` (`http://<backend>:8500/internal/v1`)
- Content-Type: `application/json`
- 타임아웃 권장: 제어·조회 **5s**, 룰 CRUD **3s**
- 공통 에러: `{ "error": { "code": "...", "message": "...", "field"?: "..." } }`
- 성공 응답의 장치·룰·이벤트 필드명은 **camelCase** (`db/query` 와 동일)
- 전체 action/query 목록의 source of truth: `GET /device-classes` (아래 클래스별 예시는 대표 샘플)

**공통 에러 코드**


| code                | HTTP | 설명                                           |
| ------------------- | ---- | -------------------------------------------- |
| `INVALID_REQUEST`   | 400  | Body/필드 형식 오류                                |
| `INVALID_PARAMS`    | 400  | `params` 가 `paramsSchema` 와 불일치 (`field` 포함) |
| `NOT_FOUND`         | 404  | 장치·룰·IR 커맨드 없음                               |
| `AMBIGUOUS_DEVICE`  | 409  | `roomId` 범위에서 장치 이름 부분 일치가 2건 이상             |
| `DEVICE_OFFLINE`    | 409  | 장치 미연결 (state/query/invoke 공통)               |
| `ACTION_NOT_FOUND`  | 404  | 해당 클래스에 없는 action                            |
| `QUERY_NOT_FOUND`   | 404  | 해당 클래스에 없는 query                             |
| `INVALID_EXEC_MODE` | 400  | action attribute 와 맞지 않는 execMode            |
| `RULE_DISABLED`     | 409  | 비활성 룰 실행 시도                                  |
| `COOLDOWN_ACTIVE`   | 409  | 룰 쿨다운 중 (수동 실행 시)                            |
| `DEVICE_TIMEOUT`    | 504  | 장치 응답 시간 초과                                  |


---



### 타입

```ts
// ── 장치 ──────────────────────────────────────────────────────────────────

type DeviceId = string;   // 16자리 hex, 예: "5d0a3f8c26b91e74"

type ActionAttribute = 'Toggle' | 'Repeat' | 'Momentary' | 'Stateful';

type DeviceAction = {
  name: string;
  description: string;
  attributes: ActionAttribute[];
  paramsSchema: object;
};

type DeviceQuery = {
  name: string;
  description: string;
  paramsSchema?: object;
};

type DeviceClassCapabilities = {
  class: string;
  label: string;
  actions: DeviceAction[];
  queries: DeviceQuery[];
  triggerKinds: ('gesture' | 'device_state' | 'ir_recv')[];
  triggerableQueries?: string[];
  ptz?: boolean;
};

type DeviceSummary = {
  id: DeviceId;
  name: string;
  description: string;
  class: string;
  classLabel: string;
  vendor?: string;
  model?: string;
  enabled: boolean;
  connected: boolean;
  lastSeenAt?: string;
  stateSummary: string;
  room?: { id: number; name: string };
};

type DeviceDetail = DeviceSummary & {
  capabilities: DeviceClassCapabilities;
};

// ── 실행 ──────────────────────────────────────────────────────────────────

type ExecMode = 'once' | 'repeat' | 'toggle';

type InvokeDeviceRequest = {
  params?: object;
  execMode?: ExecMode;
  repeatIntervalMs?: number;
  triggeredBy?: string;
};

type InvokeDeviceResponse = {
  ok: true;
  deviceId: DeviceId;
  action: string;
  state: object;
  eventId: string;
};

type QueryDeviceRequest = {
  params?: object;
};

type QueryDeviceResponse = {
  deviceId: DeviceId;
  query: string;
  result: object;
};

// ── 룰·예약 ───────────────────────────────────────────────────────────────

type TriggerKind = 'gesture' | 'device_state' | 'ir_recv';

type GestureTrigger = {
  kind: 'gesture';
  deviceId: DeviceId;
  gestureSetPath: string;
  classId: number;
};

type DeviceStateTrigger = {
  kind: 'device_state';
  deviceId: DeviceId;
  query: string;
  op: '>' | '>=' | '<' | '<=' | '==';
  value: number;
};

type IrRecvTrigger = {
  kind: 'ir_recv';
  deviceId: DeviceId;
  commandId: string;
};

type RuleTrigger = GestureTrigger | DeviceStateTrigger | IrRecvTrigger;

type ScheduleOnce = {
  repeat: 'once';
  delayMinutes: number;
};

type ScheduleDaily = {
  repeat: 'daily';
  time: string;
};

type ScheduleWeekly = {
  repeat: 'weekly';
  time: string;
  daysOfWeek: ('mon'|'tue'|'wed'|'thu'|'fri'|'sat'|'sun')[];
};

type RuleSchedule = ScheduleOnce | ScheduleDaily | ScheduleWeekly;

type RuleAction = {
  deviceId: DeviceId;
  name: string;
  params?: object;
};

type Rule = {
  id: string;
  name: string;
  enabled: boolean;
  trigger: RuleTrigger | null;
  schedule: RuleSchedule | null;
  action: RuleAction;
  execMode: ExecMode;
  repeatIntervalMs?: number;
  cooldownMs: number;
};

type RuleView = Rule & {
  actionDeviceName: string;
  triggerDeviceName?: string;
};

type CreateRuleRequest = Omit<Rule, 'id'>;
type UpdateRuleRequest = Partial<CreateRuleRequest>;

// ── IR · 이벤트 ───────────────────────────────────────────────────────────

type IrCommand = {
  id: string;
  name: string;
  description?: string;
  deviceHint?: string;
  unit: 'us';
  timings: number[];
  source: 'learned' | 'manual';
  createdAt: string;
};

type DeviceEventType = 'connection' | 'gesture' | 'ir' | 'execution' | 'schedule';

type DeviceEvent = {
  id: string;
  type: DeviceEventType;
  occurredAt: string;
  deviceId: DeviceId | null;
  deviceName: string | null;
  message: string;
  triggeredBy: string | null;
  detail: object;
};

// ── 고수준 툴 (LLM용) ─────────────────────────────────────────────────────

type DeviceListToolRequest = {
  userId?: number;
  roomId?: number;
};

type DeviceControlToolRequest = {
  userId?: number;
  roomId: number;
  device: string;
  action: string;
  params?: object;
  execMode?: ExecMode;
};

type DeviceQueryToolRequest = {
  userId?: number;
  roomId: number;
  device: string;
  query: string;
  params?: object;
};

type DeviceScheduleToolRequest = {
  userId?: number;
  roomId: number;
  device: string;
  action: string;
  params?: object;
  schedule: RuleSchedule;
  name?: string;
};

type DeviceScheduleListToolRequest = {
  userId?: number;
  roomId?: number;
  deviceId?: DeviceId;
  enabled?: boolean;
};

type DeviceScheduleCancelToolRequest = {
  userId?: number;
  ruleId: string;
};
```

---



### GET `/devices`

활성 장치 목록과 런타임 연결 상태를 반환한다.

**Query**


| 파라미터        | 설명                                  |
| ----------- | ----------------------------------- |
| `userId`    | 해당 사용자에 매핑된 장치만 (`device_user_map`) |
| `roomId`    | 방 필터 (`device_room_map`)            |
| `class`     | 클래스 필터, 예: `tuya_ep2h`              |
| `connected` | `true` | `false`                    |
| `enabled`   | `true`(기본) | `false`                |


**Response 200**

```json
{
  "items": [
    {
      "id": "5d0a3f8c26b91e74",
      "name": "거실 조명",
      "description": "거실 조명 - 컬러",
      "class": "philips_wiz_e29_color",
      "classLabel": "WiZ 컬러 조명",
      "vendor": "Philips",
      "model": "WiZ Color E29",
      "enabled": true,
      "connected": true,
      "lastSeenAt": "2026-07-06T10:00:05+09:00",
      "stateSummary": "켜짐 · 밝기 70%",
      "room": { "id": 2, "name": "거실" }
    }
  ],
  "count": 11
}
```

---



### GET `/devices/{deviceId}`

단일 장치 상세 + **capabilities** 를 반환한다.

**Response 200** — `tuya_ep2h` 예시

```json
{
  "id": "6b0f3e8a92c47d15",
  "name": "플러그1",
  "class": "tuya_ep2h",
  "classLabel": "스마트 플러그",
  "enabled": true,
  "connected": true,
  "stateSummary": "켜짐 · 27.7W",
  "room": { "id": 2, "name": "거실" },
  "capabilities": {
    "class": "tuya_ep2h",
    "label": "스마트 플러그",
    "actions": [
      { "name": "on", "description": "전원 켜기", "attributes": ["Stateful"], "paramsSchema": {} },
      { "name": "off", "description": "전원 끄기", "attributes": ["Stateful"], "paramsSchema": {} },
      { "name": "toggle", "description": "전원 토글", "attributes": ["Toggle", "Stateful"], "paramsSchema": {} }
    ],
    "queries": [
      { "name": "power", "description": "순간 전력(W)" },
      { "name": "voltage", "description": "AC 전압(V)" },
      { "name": "status", "description": "전체 datapoint" }
    ],
    "triggerKinds": ["device_state"],
    "triggerableQueries": ["power", "voltage", "current"]
  }
}
```

---



### GET `/device-classes`

클래스별 정적 capability 레지스트리 전체.

---



### GET `/devices/{deviceId}/state`

장치 **전체 런타임 상태** 스냅샷 (`query/status` 또는 `query/state` 와 동일).

**Response 200** — `tuya_ep2h`

```json
{
  "deviceId": "6b0f3e8a92c47d15",
  "connected": true,
  "state": {
    "switch": true,
    "voltage": 234.6,
    "current": 118.2,
    "power": 27.7,
    "energy": 12.4
  }
}
```

**Response 409** — `DEVICE_OFFLINE`

---



### POST `/devices/{deviceId}/query/{queryName}`

특정 query 하나를 실행한다. `actions/{actionName}` 과 동일하게 **이름은 path**, 파라미터는 body.

**Request Body**

```json
{
  "params": {}
}
```

**Response 200**

```json
{
  "deviceId": "6b0f3e8a92c47d15",
  "query": "power",
  "result": { "power": 27.7 }
}
```

`queryName` 이 `status` 또는 `state` 이면 클래스 전체 상태 객체를 `result` 에 담는다.

---



### POST `/devices/{deviceId}/actions/{actionName}`

장치 동작 실행. 백엔드는 `Actionable::invoke(name, params)` 호출 후 `device_event` 기록.

**Request Body**

```json
{
  "params": { "value": 20 },
  "execMode": "once",
  "triggeredBy": "agent:manual"
}
```

**Response 200**

```json
{
  "ok": true,
  "deviceId": "5d0a3f8c26b91e74",
  "action": "brightness",
  "state": { "on": true, "brightness": 20, "color": { "r": 255, "g": 196, "b": 120 } },
  "eventId": "evt_20260706100512ab3c"
}
```

**execMode 검증**


| execMode | 필요 attribute | 용도                                   |
| -------- | ------------ | ------------------------------------ |
| `once`   | (없음)         | 1회 실행                                |
| `toggle` | `Toggle`     | on/off 토글                            |
| `repeat` | `Repeat`     | 홀드 중 반복 (`repeatIntervalMs`, 기본 200) |


---



### 클래스별 제어·조회 JSON 예시

아래는 `POST .../actions/{actionName}` / `POST .../query/{queryName}` 의 **대표 request·response** 이다.
전체 목록은 `GET /device-classes` 또는 `GET /devices/{deviceId}` 의 `capabilities` 를 따른다.

#### `tuya_ep2h` (스마트 플러그)

```http
POST /internal/v1/devices/6b0f3e8a92c47d15/actions/on
```

```json
{ "params": {}, "execMode": "once", "triggeredBy": "agent:manual" }
```

```json
{
  "ok": true,
  "deviceId": "6b0f3e8a92c47d15",
  "action": "on",
  "state": { "switch": true, "voltage": 234.6, "current": 118.2, "power": 27.7, "energy": 12.4 },
  "eventId": "evt_..."
}
```

```http
POST /internal/v1/devices/6b0f3e8a92c47d15/actions/toggle
```

```json
{ "params": {}, "execMode": "toggle" }
```

```http
POST /internal/v1/devices/6b0f3e8a92c47d15/query/power
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "6b0f3e8a92c47d15",
  "query": "power",
  "result": { "power": 27.7 }
}
```



#### `philips_wiz_e29_color` (WiZ 컬러 조명)

```http
POST /internal/v1/devices/5d0a3f8c26b91e74/actions/brightness
```

```json
{ "params": { "value": 30 }, "execMode": "once" }
```

```json
{
  "ok": true,
  "deviceId": "5d0a3f8c26b91e74",
  "action": "brightness",
  "state": { "on": true, "brightness": 30, "color": { "r": 255, "g": 196, "b": 120 } },
  "eventId": "evt_..."
}
```

```http
POST /internal/v1/devices/5d0a3f8c26b91e74/actions/color
```

```json
{ "params": { "r": 255, "g": 120, "b": 64 }, "execMode": "once" }
```

```http
POST /internal/v1/devices/5d0a3f8c26b91e74/query/brightness
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "5d0a3f8c26b91e74",
  "query": "brightness",
  "result": { "brightness": 30 }
}
```



#### `philips_wiz_e29_white` (WiZ 화이트 조명)

```http
POST /internal/v1/devices/3f7c2a9e14d8065b/actions/temperature
```

```json
{ "params": { "value": 3000 }, "execMode": "once" }
```

```json
{
  "ok": true,
  "deviceId": "3f7c2a9e14d8065b",
  "action": "temperature",
  "state": { "on": true, "brightness": 70, "temperature": 3000 },
  "eventId": "evt_..."
}
```

```http
POST /internal/v1/devices/3f7c2a9e14d8065b/query/state
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "3f7c2a9e14d8065b",
  "query": "state",
  "result": { "on": true, "brightness": 70, "temperature": 3000 }
}
```



#### `tizen_tv`

```http
POST /internal/v1/devices/2c9f6a1b4d78e350/actions/volume_up
```

```json
{ "params": {}, "execMode": "repeat", "repeatIntervalMs": 200 }
```

```json
{
  "ok": true,
  "deviceId": "2c9f6a1b4d78e350",
  "action": "volume_up",
  "state": { "on": true, "volume": 18, "channel": 7, "muted": false, "app": "youtube" },
  "eventId": "evt_..."
}
```

```http
POST /internal/v1/devices/2c9f6a1b4d78e350/actions/open_app
```

```json
{ "params": { "app": "netflix" }, "execMode": "once" }
```

```http
POST /internal/v1/devices/2c9f6a1b4d78e350/actions/mute
```

```json
{ "params": {}, "execMode": "toggle" }
```

```http
POST /internal/v1/devices/2c9f6a1b4d78e350/query/state
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "2c9f6a1b4d78e350",
  "query": "state",
  "result": { "on": true, "volume": 18, "channel": 7, "muted": false, "app": "netflix" }
}
```

리모컨 네비(`nav_up`, `select`, `home` 등)도 동일 패턴: `params: {}`, `execMode: "once"`.

#### `wave_station`

```http
POST /internal/v1/devices/5c1e8b6402fda973/actions/send_ir
```

```json
{ "params": { "commandId": "ir_ac_power" }, "execMode": "once" }
```

```json
{ "ok": true, "deviceId": "5c1e8b6402fda973", "action": "send_ir", "state": {}, "eventId": "evt_..." }
```

```http
POST /internal/v1/devices/5c1e8b6402fda973/query/env
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "5c1e8b6402fda973",
  "query": "env",
  "result": { "lux": 320, "temperature": 24.5, "humidity": 48.2 }
}
```



#### `reolink_e1_pro` (카메라 — Actionable 없음, 보조 API)

```http
POST /internal/v1/devices/27d9a4f3c85b016e/ptz/move
```

```json
{ "pan": 0.5, "tilt": -0.3 }
```

```http
PUT /internal/v1/devices/27d9a4f3c85b016e/stream
```

```json
{ "streaming": true }
```

```http
POST /internal/v1/devices/27d9a4f3c85b016e/query/mic_level
```

```json
{ "params": {} }
```

```json
{
  "deviceId": "27d9a4f3c85b016e",
  "query": "mic_level",
  "result": { "mic_level": 0.12 }
}
```



#### `srs_r4sn` (mmWave 레이더)

제어 action 없음. 제스처 트리거 소스로만 사용. 상태 조회는 `/events` 또는 `gesture_log`(`db/query`).

---



### 카메라 PTZ / 스트림 (`reolink_e1_pro`)

```http
GET  /devices/{deviceId}/ptz/capabilities
POST /devices/{deviceId}/ptz/move        { "pan": 0.5, "tilt": -0.3 }
POST /devices/{deviceId}/ptz/stop        {}
POST /devices/{deviceId}/ptz/zoom        { "delta": 10 }
GET  /devices/{deviceId}/stream          → { "status": "idle"|"streaming", "url": string|null }
PUT  /devices/{deviceId}/stream          { "streaming": true }
POST /devices/{deviceId}/snapshot        → { "occurredAt": "..." }
```

---



### GET `/rules`

**Query**: `deviceId`, `enabled`, `hasSchedule`, `hasTrigger`

**Response 200** — 예약 룰 예시

```json
{
  "items": [
    {
      "id": "rule_schedule_tv_off_once",
      "name": "30분 뒤 TV 끄기",
      "enabled": true,
      "trigger": null,
      "schedule": { "repeat": "once", "delayMinutes": 30 },
      "action": { "deviceId": "2c9f6a1b4d78e350", "name": "off", "params": {} },
      "execMode": "once",
      "cooldownMs": 0,
      "actionDeviceName": "TV",
      "triggerDeviceName": null
    }
  ],
  "count": 7
}
```

---



### POST `/rules`

**검증**: `trigger`·`schedule` 중 최소 하나, `action` 은 capabilities 와 일치, `schedule.once` 실행 후 룰은 **자동 삭제**.

**Response 201** — `RuleView`

---



### PUT `/rules/{ruleId}` · DELETE `/rules/{ruleId}` · PUT `/rules/{ruleId}/enabled` · POST `/rules/{ruleId}/execute`

예약 취소는 `DELETE /rules/{ruleId}`.

`POST .../execute` — `enabled: false` 이면 `409 RULE_DISABLED`, 쿨다운 중이면 `409 COOLDOWN_ACTIVE`.

---



### GET `/ir-commands` · GET `/ir-commands/{commandId}` · POST `/ir-commands/{commandId}/send`

IR 목록·상세·송신. 상세 응답에 전체 `timings` 포함.

---



### GET `/events`

**Query**: `types`, `deviceId`, `from`, `to`, `limit`(기본 50, 최대 200)

---



## 고수준 Tool API (LLM 바인딩용)

백엔드가 `roomId` + 장치 이름 부분 일치로 `deviceId` 를 해석한 뒤 위 REST를 호출한다.
동일 `roomId` 에 2건 이상 매칭되면 `409 AMBIGUOUS_DEVICE`.

### POST `/tools/device.list`

```json
{ "userId": 1, "roomId": 2 }
```

→ `GET /devices?userId=1&roomId=2` 요약.

**Response 200**

```json
{
  "items": [
    {
      "id": "5d0a3f8c26b91e74",
      "name": "거실 조명",
      "class": "philips_wiz_e29_color",
      "connected": true,
      "stateSummary": "켜짐 · 밝기 70%",
      "actions": ["on", "off", "toggle", "brightness", "color"]
    }
  ]
}
```

---



### POST `/tools/device.control`

```json
{
  "userId": 1,
  "roomId": 2,
  "device": "조명",
  "action": "brightness",
  "params": { "value": 30 },
  "execMode": "once"
}
```

**Response 200**

```json
{
  "ok": true,
  "deviceId": "5d0a3f8c26b91e74",
  "deviceName": "거실 조명",
  "action": "brightness",
  "state": { "on": true, "brightness": 30, "color": { "r": 255, "g": 196, "b": 120 } },
  "eventId": "evt_..."
}
```

---



### POST `/tools/device.query`

```json
{
  "userId": 1,
  "roomId": 2,
  "device": "플러그1",
  "query": "power",
  "params": {}
}
```

내부: `POST /devices/{deviceId}/query/power`

---



### POST `/tools/device.schedule`

```json
{
  "userId": 1,
  "roomId": 3,
  "device": "TV",
  "action": "off",
  "params": {},
  "schedule": { "repeat": "once", "delayMinutes": 30 },
  "name": "30분 뒤 TV 끄기"
}
```

**Response 201**

```json
{
  "ok": true,
  "rule": {
    "id": "rule_agent_20260706_100612",
    "name": "30분 뒤 TV 끄기",
    "enabled": true,
    "schedule": { "repeat": "once", "delayMinutes": 30 },
    "action": { "deviceId": "2c9f6a1b4d78e350", "name": "off", "params": {} }
  }
}
```

---



### POST `/tools/device.schedule.list`

등록된 **예약 룰** 목록. 내부: `GET /rules?hasSchedule=true` + 필터.

**Request Body**

```json
{
  "userId": 1,
  "roomId": 2,
  "enabled": true
}
```

`roomId` 가 있으면 해당 방 장치에 연결된 예약만. `deviceId` 로 특정 장치 대상만 좁힐 수 있다.

**Response 200**

```json
{
  "items": [
    {
      "id": "rule_schedule_tv_off_once",
      "name": "30분 뒤 TV 끄기",
      "enabled": true,
      "schedule": { "repeat": "once", "delayMinutes": 30 },
      "actionDeviceName": "TV",
      "action": { "deviceId": "2c9f6a1b4d78e350", "name": "off", "params": {} }
    },
    {
      "id": "rule_schedule_night_dim",
      "name": "매일 22:30 거실 조명 밝기 낮춤",
      "enabled": true,
      "schedule": { "repeat": "daily", "time": "22:30" },
      "actionDeviceName": "거실 조명",
      "action": { "deviceId": "5d0a3f8c26b91e74", "name": "brightness", "params": { "value": 20 } }
    }
  ],
  "count": 2
}
```

---



### POST `/tools/device.schedule.cancel`

예약 취소. 내부: `DELETE /rules/{ruleId}`.

**Request Body**

```json
{
  "userId": 1,
  "ruleId": "rule_schedule_tv_off_once"
}
```

**Response 200**

```json
{
  "ok": true,
  "ruleId": "rule_schedule_tv_off_once",
  "name": "30분 뒤 TV 끄기"
}
```

**Response 404** — `NOT_FOUND`

---



## 전체 엔드포인트 요약

```http
# 장치 조회
GET  /internal/v1/devices
GET  /internal/v1/devices/{deviceId}
GET  /internal/v1/device-classes
GET  /internal/v1/devices/{deviceId}/state
POST /internal/v1/devices/{deviceId}/query/{queryName}

# 장치 제어
POST /internal/v1/devices/{deviceId}/actions/{actionName}

# 카메라 (reolink_e1_pro)
GET  /internal/v1/devices/{deviceId}/ptz/capabilities
POST /internal/v1/devices/{deviceId}/ptz/move
POST /internal/v1/devices/{deviceId}/ptz/stop
POST /internal/v1/devices/{deviceId}/ptz/zoom
GET  /internal/v1/devices/{deviceId}/stream
PUT  /internal/v1/devices/{deviceId}/stream
POST /internal/v1/devices/{deviceId}/snapshot

# 룰·예약
GET    /internal/v1/rules
GET    /internal/v1/rules/{ruleId}
POST   /internal/v1/rules
PUT    /internal/v1/rules/{ruleId}
DELETE /internal/v1/rules/{ruleId}
PUT    /internal/v1/rules/{ruleId}/enabled
POST   /internal/v1/rules/{ruleId}/execute

# IR
GET  /internal/v1/ir-commands
GET  /internal/v1/ir-commands/{commandId}
POST /internal/v1/ir-commands/{commandId}/send

# 이벤트
GET  /internal/v1/events

# LLM 고수준 툴
POST /internal/v1/tools/device.list
POST /internal/v1/tools/device.control
POST /internal/v1/tools/device.query
POST /internal/v1/tools/device.schedule
POST /internal/v1/tools/device.schedule.list
POST /internal/v1/tools/device.schedule.cancel
```

---



## LangGraph tool 예시

```python
import httpx
from langchain_core.tools import tool

BACKEND = "http://127.0.0.1:8500/internal/v1"

@tool
def list_devices(room_id: int | None = None, user_id: int = 1) -> dict:
    """집 안 장치 목록과 지원 actions 를 조회한다. room_id 는 db/query room 테이블 id."""
    params = {"userId": user_id}
    if room_id is not None:
        params["roomId"] = room_id
    r = httpx.get(f"{BACKEND}/devices", params=params, timeout=5.0)
    r.raise_for_status()
    return r.json()

@tool
def control_device(room_id: int, device: str, action: str, params: dict | None = None,
                   user_id: int = 1) -> dict:
    """room_id 범위에서 장치 이름(부분 일치)으로 제어한다."""
    r = httpx.post(
        f"{BACKEND}/tools/device.control",
        json={"userId": user_id, "roomId": room_id, "device": device,
              "action": action, "params": params or {}},
        timeout=5.0,
    )
    r.raise_for_status()
    return r.json()

@tool
def query_device(room_id: int, device: str, query: str, params: dict | None = None,
                 user_id: int = 1) -> dict:
    """장치 실시간 상태/센서값 조회. query 예: power, brightness, state."""
    r = httpx.post(
        f"{BACKEND}/tools/device.query",
        json={"userId": user_id, "roomId": room_id, "device": device,
              "query": query, "params": params or {}},
        timeout=5.0,
    )
    r.raise_for_status()
    return r.json()

@tool
def schedule_device(room_id: int, device: str, action: str, schedule: dict,
                    params: dict | None = None, name: str | None = None,
                    user_id: int = 1) -> dict:
    """장치 동작 예약. schedule 예: {repeat:'once', delayMinutes:30}."""
    r = httpx.post(
        f"{BACKEND}/tools/device.schedule",
        json={"userId": user_id, "roomId": room_id, "device": device,
              "action": action, "params": params or {}, "schedule": schedule, "name": name},
        timeout=5.0,
    )
    r.raise_for_status()
    return r.json()

@tool
def list_schedules(room_id: int | None = None, user_id: int = 1) -> dict:
    """등록된 예약(스케줄 룰) 목록."""
    body = {"userId": user_id}
    if room_id is not None:
        body["roomId"] = room_id
    r = httpx.post(f"{BACKEND}/tools/device.schedule.list", json=body, timeout=3.0)
    r.raise_for_status()
    return r.json()

@tool
def cancel_schedule(rule_id: str, user_id: int = 1) -> dict:
    """예약 취소."""
    r = httpx.post(
        f"{BACKEND}/tools/device.schedule.cancel",
        json={"userId": user_id, "ruleId": rule_id},
        timeout=3.0,
    )
    r.raise_for_status()
    return r.json()

llm_with_tools = llm.bind_tools([
    list_devices, control_device, query_device,
    schedule_device, list_schedules, cancel_schedule,
])
```

챗 SSE 매핑:

```text
data: {"type":"tool.start","name":"control_device","args":{"roomId":2,"device":"조명","action":"on","params":{}}}

data: {"type":"tool.end","name":"control_device","ok":true,"result":{"deviceName":"거실 조명","state":{"on":true}}}

data: {"type":"tool.start","name":"list_schedules","args":{"roomId":3}}

data: {"type":"tool.end","name":"list_schedules","ok":true,"result":{"items":[...],"count":2}}

data: {"type":"tool.start","name":"cancel_schedule","args":{"ruleId":"rule_schedule_tv_off_once"}}

data: {"type":"tool.end","name":"cancel_schedule","ok":true,"result":{"ruleId":"rule_schedule_tv_off_once","name":"30분 뒤 TV 끄기"}}
```

---



## 백엔드 연동 지점


| 구분       | 저장·실행                                                            |
| -------- | ---------------------------------------------------------------- |
| 장치 등록 메타 | DB `device` + `device_room_map` + `device_user_map` (`db/query`) |
| 장치 런타임   | `device_list.json` → `Device` 인스턴스, `Queryable`/`Actionable`     |
| 룰·예약     | `bin/device/rules.json` (초기 시드), 런타임 CRUD 후 persist              |
| IR 커맨드   | `bin/device/ir_list.json`                                        |
| 이벤트 로그   | 메모리 링버퍼 + (후순위) `device_event` 테이블                               |


**DB 조회 API 와의 관계**

- `room` / `device` 테이블 → `roomId`·장치 이름 해석 (정적)
- **이 API** → 연결 상태·순시 전력·밝기 등 **라이브 제어/조회**
- `gesture_log` → 과거 제스처 기록 (분석용)
- `/events` → 제어·연결·예약 발화 등 **운영 타임라인**

**범위 제외 (별도 API·후순위)**

- TTS (`POST /devices/{id}/tts`), 알림 생성, 제스처 룰 CRUD(에이전트는 예약 위주)

---



## 구현 우선순위 (제안)

1. `GET /devices`, `GET /devices/{id}`, `POST .../actions/{name}`, `POST .../query/{name}`, `GET .../state`
2. `POST /tools/device.control`, `POST /tools/device.query`, `POST /tools/device.list`
3. `GET /rules`, `POST /rules`, `DELETE /rules/{id}`, `POST /tools/device.schedule`, `schedule.list`, `schedule.cancel`
4. `GET /events`, `GET /ir-commands`, `POST /ir-commands/{id}/send`
5. PTZ/스트림, 룰 trigger 자동 실행 엔진 — 후순위

