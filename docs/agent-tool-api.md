# 에이전트 API (이전 위치)

이 문서는 **`docs/agent-api/`** 로 분할되었습니다.

## 목차

- [agent-api/README.md](./agent-api/README.md) — 서버 구성·문서 인덱스

### 백엔드 → 에이전트

- [forwarding-api.md](./agent-api/forwarding-api.md) — LLM 모델 포워딩 (`/llm/v1`)
- [chat-api.md](./agent-api/chat-api.md) — 대화 API (`/chat/v1`)
- [sleep-analysis-api.md](./agent-api/sleep-analysis-api.md) — 수면 분석 (`/sleep/v1`)
- [power-analysis-api.md](./agent-api/power-analysis-api.md) — 전력 분석 (`/power/v1`)
- [insight-generation-api.md](./agent-api/insight-generation-api.md) — 인사이트 생성 (`/insight/v1`)
- [weekly-plan-analysis-api.md](./agent-api/weekly-plan-analysis-api.md) — 주간 배너 (`/weekly-plan/v1`)
- [posture-analysis-api.md](./agent-api/posture-analysis-api.md) — 자세 분석 (예약)

### 에이전트 → 백엔드

- [device-tool-api.md](./agent-api/device-tool-api.md) — 장치·룰·예약·`tools/device.*` (`/internal/v1/*`)
- [db-query-api.md](./agent-api/db-query-api.md) — DB 조회 (`POST /internal/v1/db/query`)
- [rag-api.md](./agent-api/rag-api.md) — RAG 검색 (`POST /internal/v1/rag/search`)

기존 링크 호환을 위해 이 파일은 리다이렉트 역할만 합니다. 내용 수정은 `docs/agent-api/` 아래 파일을 편집하세요.
