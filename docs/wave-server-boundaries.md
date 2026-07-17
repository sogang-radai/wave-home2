# wave-server 경계·계약 (Phase 0+)

- 작성일: 2026-07-16 (포트 갱신: 2026-07-17)
- 포트 SSOT: [`docs/ports.txt`](./ports.txt)
- 상세 API 스키마: [`docs/agent-api/`](./agent-api/README.md) · 데모 UX: [`docs/demo-chat-features.md`](./demo-chat-features.md)

---

## 1. 프로세스와 포트

| Profile | Backend client-api | Backend agent-api | Agent server |
|---------|-------------------:|------------------:|-------------:|
| **Production** | **8500** | **8501** (`127.0.0.1`) | **8502** |
| **Demo** | **8510** | **8511** (`127.0.0.1`) | **8512** |
| **Test** | **8520** | unused | unused |

설정: `bin/config.json` (`server.port`, `server.agent_api_port`, `server.agent_api_bind`, `agent.base_url`).

**용어**

| 이름 | 의미 |
|------|------|
| client-api | 프론트·브라우저용 공개 HTTP (`/api/v1`, static) |
| Backend agent-api | 에이전트 tool용 `/internal/v1` 전용 listen (루프백) |
| Agent server agent-api | `wave-home-agent`가 듣는 포트 (`/chat`, `/sleep`, …) |

```
브라우저 ──► wave-server client-api :8500|8510|8520
               │  /api/v1/* , static
               │
               ├── agent-api :8501|8511 (127.0.0.1)  /internal/v1/*
               │         ▲
               │         │ tools
               └──► agent :8502|8512
                      │  /chat|/sleep|/power|/insight|…
                      └── (outbound jobs from backend use agent.base_url)
```

스크립트:

- real: `scripts/configure/agent-real.sh` → internal `http://127.0.0.1:8501/internal/v1`
- demo: `scripts/configure/agent-demo.sh` → internal `http://127.0.0.1:8511/internal/v1`
- 실행: `scripts/run/prod.sh` · `scripts/run/demo.sh` · `scripts/run/test.sh` (루트 별칭 `scripts/run-*.sh`)

---

## 2. 세 가지 HTTP 표면

### 2.1 공개 API — 클라이언트 → 백엔드

| 항목 | 값 |
|------|-----|
| Base | `/api/v1` |
| Listen | client-api (8500 / 8510 / 8520) |
| 호출자 | 프론트 SPA |
| 소유 | `src/wave-server/web/http/v1/*` |

### 2.2 Internal API — 에이전트 → 백엔드

| 항목 | 값 |
|------|-----|
| Base | `/internal/v1` |
| Listen | **agent-api** (8501 / 8511, 기본 `127.0.0.1`) |
| 호출자 | `wave-home-agent` |
| 소유 | `src/wave-server/web/http/internal/*` |
| env | `WAVEHOME_AGENT_INTERNAL_BASE_URL` |

> Drogon은 두 listener에 동일 라우트를 올리지만, `web/listener_policy.cpp` SyncAdvice가
> client-api에서 `/internal/*`를, agent-api에서 그 외 경로를 **403**으로 거절한다.
> agent-api는 기본 `127.0.0.1` 바인딩이다.

### 2.3 에이전트 outbound — 백엔드 → 에이전트

| 항목 | 값 |
|------|-----|
| Base URL | `agent.base_url` — prod `http://127.0.0.1:8502`, demo `http://127.0.0.1:8512` |
| 계약 | [`docs/agent-api/`](./agent-api/README.md) |

| Base (에이전트) | 용도 |
|-----------------|------|
| `/chat/v1` | 대화 SSE |
| `/sleep/v1` | 수면 job |
| `/power/v1` | 전력 job |
| `/insight/v1` | 인사이트 |
| `/goal-coaching/v1` | 목표 코칭 |
| `/weekly-plan/v1` | 주간 배너 |
| `/llm/v1` | LLM·임베딩 |
| `/posture/v1` | 예약(스펙 미확정) |

---

## 3. 데모 불변식

| 불변식 | 값 |
|--------|-----|
| Profile | `--profile demo` |
| client-api | **8510** |
| agent-api | **8511** |
| Agent server | **8512** |
| DB | `bin/data/demo.db`, `read_only` + `skip_migrations` |
| 앵커 | `2026-06-30` |
| 세션 | `demoRuntimeId` / `X-Wave-Demo-Runtime-Id` — 메모리 쓰기, DB RO |
| 정책 | `demo/demo_policy.cpp` — DemoProfileRuntime::startServices에서 등록; 미허용 mutation은 403 `DEMO_READ_ONLY` |
| 세션·전력·자동화 | `DemoProfileRuntime` 멤버 (`demoSessions` / `demoPowerMeter` / `m_automation`); 싱글톤 아님 |

---

## 4. 프로필 런타임 차이 (리팩터 기준)

| | real | demo | test |
|--|------|------|------|
| client-api | 8500 | 8510 | 8520 |
| agent-api | 8501 | 8511 | — |
| Agent | 8502 | 8512 | — |
| DB | `database.db` R/W | `demo.db` RO | 없음 |
| 장치 | DeviceManager | 가상+세션 | 없음 |

---

## 5. 리팩터 페이즈

| Phase | 내용 |
|-------|------|
| 0 | 경계·포트 고정 (`ports.txt` + 본 문서) |
| 1 | ProfileRuntime / 기동 정리 |
| 2 | `/internal` 경로를 agent-api listener로 격리 (`listener_policy`) |
| 3 | Controllers thin + demo/prod Facade (`facade/`, `demo/*_facade`) |
| 4 | DemoProfileRuntime 소유권 (세션·전력·자동화·demo policy) |

스크립트 레이아웃: `scripts/build/` · `scripts/download/` · `scripts/configure/` · `scripts/run/`.
