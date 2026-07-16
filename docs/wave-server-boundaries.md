# wave-server 경계·계약 (Phase 0)

- 작성일: 2026-07-16
- 목적: 리팩터링 Phase 1+ 전에 **포트·호출 방향·데모 불변식·internal 격리 목표**를 코드 변경 없이 고정한다.
- 상세 API 스키마: [`docs/agent-api/`](./agent-api/README.md) · 데모 UX: [`docs/demo-chat-features.md`](./demo-chat-features.md)

---

## 1. 프로세스와 포트 (현재 AS-IS)

단일 바이너리 `wave-server`가 `--profile real|demo|test`로 분기한다. 에이전트는 별도 프로세스다.

| 프로세스 | 역할 | Listen 포트 | 설정 위치 |
|----------|------|-------------|-----------|
| `wave-server --profile real` | 공개 API + **동일 포트에** `/internal/v1` | **8500** | `bin/config.json` → `real.server.port` |
| `wave-server --profile demo` | 공개 API + **동일 포트에** `/internal/v1` | **8502** | `demo.server.port` |
| `wave-server --profile test` | 정적 사이트 위주 (DB/장치 없음) | **8503** | `test.server.port` |
| `wave-home-agent` | LLM·채팅·분석 잡 · tool로 백엔드 `/internal` 호출 | **8501** | 에이전트 uvicorn / 각 profile `agent.base_url` |

**오해 정리**

- 에이전트 포트는 **8501**이다. **8001은 이 레포에서 사용하지 않는다.**
- `/internal/v1`은 에이전트가 *듣는* 포트가 아니라, **wave-server가 듣고 에이전트가 호출하는 inbound**다.
- real/demo를 바꿀 때 에이전트 `.env`의 `WAVEHOME_AGENT_INTERNAL_BASE_URL`을 함께 바꿔야 한다.
  - real: `scripts/configure-agent-real.sh` → `http://127.0.0.1:8500/internal/v1`
  - demo: `scripts/configure-agent-demo.sh` → `http://127.0.0.1:8502/internal/v1`

```
브라우저 ──► wave-server :8500|8502|8503
               │  /api/v1/* , static
               │  /internal/v1/*   ◄── 현재는 공개 리스너와 동일 (격리 없음)
               │
               └──► agent :8501   (/chat|/sleep|/power|/insight|/goal-coaching|/llm|…)
                      │
                      └──► wave-server …/internal/v1/*  (tools, db, rag, alarms…)
```

---

## 2. 세 가지 HTTP 표면

### 2.1 공개 API — 클라이언트 → 백엔드

| 항목 | 값 |
|------|-----|
| Base | `/api/v1` |
| Listen | profile 공개 포트 (8500 / 8502 / 8503) |
| 호출자 | 프론트 SPA (`site` / `site-demo` / `site-test`) |
| 소유 | `src/wave-server/web/http/v1/*_controller.*` |
| 프론트 계약 | `wave-home-front/docs/api/` |

대표 도메인: `health`, `session`, `accounts`, `rooms`, `devices`, `iot/*`, `alarms`, `schedule-tasks`, `chat`, `sleep`, `power`, `insights`, `goals`, `weekly-plan`, `settings`, `notifications`, `push`, `dashboard`.

### 2.2 Internal API — 에이전트 → 백엔드

| 항목 | 값 |
|------|-----|
| Base | `/internal/v1` |
| Listen (AS-IS) | **공개 포트와 동일** |
| 호출자 | `wave-home-agent` only (의도). **현재 인증·바인딩 제한 없음** |
| 소유 | `src/wave-server/web/http/internal/internal_controller.*` |
| 계약 | [`docs/agent-api/`](./agent-api/README.md) (device-tool, alarms, schedule-tasks, db-query, rag) |
| 에이전트 env | `WAVEHOME_AGENT_INTERNAL_BASE_URL` (…`/internal/v1`) |

라우트 목록 (코드 `InternalController` 기준):

| Method | Path |
|--------|------|
| POST | `/internal/v1/db/query` |
| POST | `/internal/v1/rag/search` |
| GET | `/internal/v1/devices`, `/devices/{id}`, `/devices/{id}/state` |
| GET | `/internal/v1/device-classes` |
| POST | `/internal/v1/devices/{id}/query/{queryName}` |
| POST | `/internal/v1/devices/{id}/actions/{actionName}` |
| GET/POST… | `/internal/v1/devices/{id}/ptz/*`, `/stream`, `/snapshot`, `/tts` |
| CRUD | `/internal/v1/rules`, `/rules/{id}`, `…/enabled`, `…/execute` |
| GET | `/internal/v1/ir-commands`, `/ir-commands/{id}`, `/events` |
| POST | `/internal/v1/tools/device.list\|control\|query\|schedule\|schedule.list\|schedule.cancel` |
| CRUD | `/internal/v1/alarms`, `/alarms/{id}` |
| CRUD | `/internal/v1/schedule-tasks`, `/schedule-tasks/{taskId}` |

공개 `/api/v1/iot|alarms|schedule-tasks` 와 **도메인 로직이 상당 부분 중복**된다. Phase 3에서 facade로 모은다.

### 2.3 에이전트 outbound — 백엔드 → 에이전트

| 항목 | 값 |
|------|-----|
| Base URL | `bin/config.json` → `*.agent.base_url` (기본 `http://127.0.0.1:8501`) |
| 호출자 | `service/agent_client.*`, sleep/power/insight/goal 경로 |
| 계약 | agent-api의 chat / sleep / power / insight / weekly-plan / posture / forwarding |

| Base (에이전트) | 용도 |
|-----------------|------|
| `/chat/v1` | 대화 SSE |
| `/sleep/v1` | 수면 요약·리포트 job |
| `/power/v1` | 전력 리포트 job |
| `/insight/v1` | 인사이트 배치 |
| `/goal-coaching/v1` | 목표 코칭 |
| `/weekly-plan/v1` | 주간 배너 (문서상) |
| `/llm/v1` | LLM·임베딩 포워딩 |
| `/posture/v1` | 예약(스펙 미확정) |

에이전트는 **DB에 직접 접근하지 않는다.** 조회·쓰기는 전부 백엔드 `/internal` 또는 잡 결과 저장은 백엔드가 한다.

---

## 3. Internal 격리 목표 (Phase 2 TO-BE)

### 3.1 원칙

- **에이전트 listen 포트(8501)로 inbound를 제한하지 않는다.** (구조적으로 성립하지 않음)
- wave-server가 **공개 listener와 분리된 내부 listener**를 연다.
- 내부 listener는 **루프백 바인딩**을 기본으로 한다.
- (권장 보조) 공유 토큰 헤더로 실수·프록시 경유를 한 겹 더 막는다.

### 3.2 포트 후보 (확정안)

공개 포트 + 10000, 바인드 `127.0.0.1`만:

| Profile | 공개 (기존) | Internal (신규) | 에이전트 `WAVEHOME_AGENT_INTERNAL_BASE_URL` |
|---------|-------------|-----------------|---------------------------------------------|
| real | 8500 | **18500** | `http://127.0.0.1:18500/internal/v1` |
| demo | 8502 | **18502** | `http://127.0.0.1:18502/internal/v1` |
| test | 8503 | (미사용 가능) | — |

설정 키 후보 (Phase 2에서 `bin/config.json`에 추가):

```json
"server": {
  "port": 8500,
  "internal_port": 18500,
  "internal_bind": "127.0.0.1"
}
```

- 공개 listener: `/api/v1`, static. **`/internal/v1` 요청은 403/404.**
- 내부 listener: `/internal/v1`만. health는 공개 포트 유지.

Phase 2 구현 전까지는 AS-IS(동일 포트)를 유지한다. 스크립트·문서는 위 URL을 **목표값**으로 적는다.

### 3.3 비권장

- 소스 포트가 8501이면 허용
- 공개 NIC에 internal 포트를 그대로 바인딩

---

## 4. 데모 불변식

기준 설정: `bin/config.json` → `demo`  
기준 UX: [`demo-chat-features.md`](./demo-chat-features.md)  
목업 생성: [`demo/demo.md`](../demo/demo.md)

| 불변식 | 현재 값 / 동작 |
|--------|----------------|
| Profile | `--profile demo` → `demo_mode=true`, `devices_enabled=false` → 실물 `DeviceManager` 미기동 |
| DB 경로 | `bin/data/demo.db` (`database_path`: `data/demo.db`, config 기준) |
| DB 모드 | `read_only: true`, `skip_migrations: true` — **프로세스 수명 동안 SQLite에 쓰지 않음** |
| 앵커 날짜 | `anchor_date`: `2026-06-30` (수면·전력 등 조회 기준일) |
| 시연 세션 | `demoRuntimeId` (쿠키 / `X-Wave-Demo-Runtime-Id`) — 브라우저(시연자) 단위 격리 |
| 세션 메모리에만 쓰는 것 | 가상 장치 상태, 알람·일정·룰, 채팅 대화 overlay, AI 개인 프롬프트 등 |
| 세션 만료/새로고침 | 공유 `demo.db` 원본 조회로 복귀 (세션 쓰기 유실) |
| 쓰기 정책 | `web/demo_policy.cpp` — 허용된 mutation만 통과, 그 외 **403 `DEMO_READ_ONLY`** |
| 허용 write (요약) | 채팅; 가상 IoT control/query; alarms / schedule-tasks / rules(내부·공개); `settings/ai-agent`; 일부 read성 POST(`db/query`, `rag/search`, `tools/device.list|query|schedule.list`) |
| 차단 write (요약) | accounts/rooms/devices CRUD, IR 학습 저장, 그 외 영구 설정 변경 등 |
| 에이전트 연동 | 에이전트는 **8501** 유지, internal base는 **8502**(AS-IS) → Phase 2 후 **18502** |
| 데모 런타임 코드 | `src/wave-server/demo/*`, `DemoAutomationRuntime` — Phase 4에서 `DemoProfileRuntime`으로 소유권 이전 예정 |

**레거시 표기:** 예전 `mock/data/mock.db` 경로는 폐기 방향이다. 런타임·생성 파이프라인은 `demo.db`를 쓴다. (`mock/` 폴더는 잔존할 수 있으나 Phase 0 계약의 SSOT는 `demo.db`.)

---

## 5. 프로필별 런타임 차이 (리팩터 시 분리 기준)

| | real | demo | test |
|--|------|------|------|
| 공개 포트 | 8500 | 8502 | 8503 |
| DB | `data/database.db` R/W + migrations | `data/demo.db` RO | 없음 |
| 장치 | `DeviceManager` | 가상 백엔드 + 세션 | 없음 |
| 알람/스케줄 | `AlarmManager` + DB | `DemoSessionRegistry` + `DemoAutomationRuntime` | 없음 |
| Sleep/Power managers | 기동 | 스킵(조회는 DB) | 스킵 |
| demo_policy | off | on | off |

Phase 1 목표: 위 차이를 `IProfileRuntime` / `RealProfileRuntime` / `DemoProfileRuntime`으로 옮기고, `AppState`는 config·server·db 핸들·running만 남긴다.

---

## 6. 리팩터 페이즈와의 연결

| Phase | 내용 | 이 문서에서의 상태 |
|-------|------|-------------------|
| **0** | 경계·불변식 고정 | **본 문서** |
| 1 | 기동/수명 → ProfileRuntime | 계약만 존재 |
| 2 | internal 별도 listen (1850x) | §3 목표 포트 |
| 3 | Controllers thin + demo/prod Facade | §2 표면 유지, 구현 분리 |
| 4 | 데모 상태 소유권을 DemoProfileRuntime으로 | §4·§5 |
| 5 (선택) | 공개/internal 중복 축소, demo 링크 분리 | — |

---

## 7. Phase 0 완료 체크리스트

- [x] 포트·호출 방향 AS-IS 표기 (8500/8501/8502/8503, 8001 미사용)
- [x] 공개 `/api/v1` · `/internal/v1` · 에이전트 outbound 구분
- [x] Internal 격리 TO-BE 포트 후보 확정 (18500 / 18502)
- [x] 데모 DB·세션 메모리·policy 불변식 문서화 (`demo.db`)
- [x] 관련 문서 교차 링크·구식 `mock.db` 표기 정리 (`demo-chat-features`, `agent-api/README`, `agent-integration`, `README`)

코드·설정·스크립트 변경은 Phase 1+에서 한다.
