# 에이전트 API 명세

백엔드(:8500)와 에이전트 서버(:8501) 사이의 HTTP 계약입니다.
기존 단일 문서 `docs/agent-tool-api.md`를 역할별로 분할했습니다.

## 변경 내역 (2026-07-08)

- **문서 표기**: 에이전트→백엔드 API는 full path (`/internal/v1/...`) 로 통일
- `db-query-api.md`: `routine_task` → `schedule_task`, `insight` 필터 전면 개편(`surface`, `date`, `ruleJson`, `scheduleTaskJson`), `posture_stat`·`posture_report`·`weekly_plan_report` 추가
- `rag-api.md`: `posture_report`, `weekly_plan_report`, `insight_*` 컬렉션 5종 추가
- **신규** `insight-generation-api.md` — 백엔드 → 에이전트 인사이트 배치 생성
- **신규** `weekly-plan-analysis-api.md` — 주간 배너 리포트 생성 (metrics 없음, 에이전트가 db/rag 툴 조회)
- **신규** `posture-analysis-api.md` — 경로 예약만 (메트릭 미확정)
- **신규** [device-tool-api.md](./device-tool-api.md) — 에이전트→백엔드 장치·룰·예약 (`/internal/v1/*`). 구 `tool-api.md` 통합·삭제
- **신규** [schedule-tasks-api.md](./schedule-tasks-api.md) — 에이전트→백엔드 주간·1회 일정 CRUD (`/internal/v1/schedule-tasks`)
- **신규** [alarms-api.md](./alarms-api.md) — 에이전트→백엔드 알람 CRUD (`/internal/v1/alarms`)
- `db-query-api.md`: `automation_rule` 테이블 조회 추가

## 서버 구성

- **프론트엔드** (React SPA) — API 미제공. 백엔드 `/api/v1` 만 호출하는 클라이언트.
- **백엔드** (C++ Drogon, real :8500 / demo :8502 / test :8503) — 공개 게이트웨이. 프론트 API + 에이전트 대상 내부 API.
  SQLite 소유·R/W 전담(데모는 RO). RAG·DB 조회 수행.
- **에이전트** (Python FastAPI + LangGraph, :8501) — 내부 서버. **DB에 직접 접근하지 않는다.**

포트·호출 방향·`/internal` 격리 목표의 SSOT: [wave-server-boundaries.md](../wave-server-boundaries.md).  
시연(데모) 모드 채팅 기능·제한: [demo-chat-features.md](../demo-chat-features.md) (데모 코어는 :8502).

### 호출 방향 요약

| 방향 | 문서 |
|------|------|
| 백엔드 → 에이전트 | [forwarding-api.md](./forwarding-api.md), [chat-api.md](./chat-api.md), [sleep-analysis-api.md](./sleep-analysis-api.md), [power-analysis-api.md](./power-analysis-api.md), [insight-generation-api.md](./insight-generation-api.md), [weekly-plan-analysis-api.md](./weekly-plan-analysis-api.md), [posture-analysis-api.md](./posture-analysis-api.md) |
| 에이전트 → 백엔드 | [device-tool-api.md](./device-tool-api.md), [schedule-tasks-api.md](./schedule-tasks-api.md), [alarms-api.md](./alarms-api.md), [db-query-api.md](./db-query-api.md), [rag-api.md](./rag-api.md) |

예시:

- 프론트 → 백엔드(:8500): `/api/v1/*`
- 백엔드 → 에이전트(:8501): LLM 포워딩, 수면·전력·인사이트·주간배너 분석 잡
- 에이전트 → 백엔드(:8500): 장치 제어·룰·예약, 일정·알람 CRUD, RAG 검색, DB **조회** 등 `/internal/v1/*`

### 룰·일정·알람 (프론트 vs 에이전트)

| 도메인 | 프론트 (`/api/v1`) | 에이전트 조회 | 에이전트 쓰기 |
|--------|-------------------|---------------|---------------|
| IoT 룰·예약 | [`iot/rules`](../../wave-home-front/docs/api/iot.md) | `db/query` (`automation_rule`) | [`device-tool-api`](./device-tool-api.md) `/internal/v1/rules` |
| 주간·1회 일정 | [`schedule-tasks`](../../wave-home-front/docs/api/schedule-tasks.md) | `db/query` (`schedule_task`) | [`schedule-tasks-api`](./schedule-tasks-api.md) |
| 알람 | [`alarms`](../../wave-home-front/docs/api/alarm.md) | `db/query` (`alarm`) | [`alarms-api`](./alarms-api.md) |

## 문서 표기 규칙

- **에이전트 → 백엔드** 문서의 엔드포인트는 항상 **full path** (`/internal/v1/...`) 로 적는다. Base URL만 따로 두지 않고 섹션 제목·예시·요약 블록에 동일하게 쓴다.
- **백엔드 → 에이전트** 문서는 각 서비스 Base URL (`/chat/v1`, `/insight/v1` 등) 아래 상대 경로를 쓸 수 있다. full path 예: `POST /chat/v1/turns`.
- **프론트 공개 API**는 [`wave-home-front/docs/api/README.md`](../wave-home-front/docs/api/README.md) — Base URL `/api/v1`.

## 문서 목록

### 백엔드 → 에이전트

| 파일 | Base URL | 설명 |
|------|----------|------|
| [forwarding-api.md](./forwarding-api.md) | `/llm/v1` | OpenAI 호환 LLM·임베딩 포워딩 |
| [chat-api.md](./chat-api.md) | `/chat/v1` | LangGraph 에이전틱 대화 (SSE) |
| [sleep-analysis-api.md](./sleep-analysis-api.md) | `/sleep/v1` | 수면 요약·리포트 생성 (비동기 job) |
| [power-analysis-api.md](./power-analysis-api.md) | `/power/v1` | 전력 리포트 생성 (비동기 job) |
| [insight-generation-api.md](./insight-generation-api.md) | `/insight/v1` | 인사이트 배치 생성 (비동기 job) |
| [weekly-plan-analysis-api.md](./weekly-plan-analysis-api.md) | `/weekly-plan/v1` | 주간 계획 배너 리포트 |
| [posture-analysis-api.md](./posture-analysis-api.md) | `/posture/v1` | 자세 분석 (경로 예약, 스펙 미확정) |

### 에이전트 → 백엔드

| 파일 | Base URL | 설명 |
|------|----------|------|
| [device-tool-api.md](./device-tool-api.md) | `/internal/v1` | 장치 조회·제어·룰·예약·IR·이벤트·`tools/device.*` |
| [schedule-tasks-api.md](./schedule-tasks-api.md) | `/internal/v1` | 주간·1회 일정 CRUD `schedule-tasks` |
| [alarms-api.md](./alarms-api.md) | `/internal/v1` | 알람 설정 CRUD `alarms` |
| [db-query-api.md](./db-query-api.md) | `/internal/v1` | 배치 DB **조회** `POST /internal/v1/db/query` |
| [rag-api.md](./rag-api.md) | `/internal/v1` | 벡터 RAG 검색 `POST /internal/v1/rag/search` |

## 관련 문서

- [wave-server-boundaries.md](../wave-server-boundaries.md) — 포트·공개/internal/에이전트 경계 (Phase 0)
- [agent-integration.md](../agent-integration.md) — 통합 테스트·배포
- [db-schema.md](../db-schema.md) — SQLite 스키마
