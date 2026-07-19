# 에이전트 서버 연동 가이드

백엔드(`wave-server`)와 에이전트(`wave-home-agent`)를 함께 띄우는 절차.
포트 SSOT: [ports.txt](./ports.txt) · [wave-server-boundaries.md](./wave-server-boundaries.md).

## 저장소 구조

```
wave-home2/
├── wave-home-agent/     # git submodule (Python FastAPI + LangGraph)
├── bin/                 # 백엔드 실행·설정 (config.json)
├── docs/
│   ├── ports.txt
│   ├── agent-api/
│   └── wave-server-boundaries.md
└── scripts/
    ├── build/
    ├── configure/
    ├── download/
    └── run/
```

## 1. 에이전트 개발 환경

```bash
./scripts/configure/agent-dev.sh   # .env, venv, pip install
```

## 2. 포트·역할

| 서비스 | Production | Demo | Test |
|--------|------------|------|------|
| Backend client-api | 8500 | 8510 | 8520 |
| Backend agent-api | 8501 | 8511 | — |
| Agent server | 8502 | 8512 | — |

**호출 방향**

- 프론트 → 백엔드 client-api: `/api/v1/*`
- 백엔드 → 에이전트: `/chat/v1`, `/llm/v1`, `/sleep/v1`, `/power/v1`, …
- 에이전트 → 백엔드 agent-api: `/internal/v1/*`
  - real: `scripts/configure/agent-real.sh`
  - demo: `scripts/configure/agent-demo.sh`

## 3. 설정

### 백엔드 `bin/config.json`

```json
"agent": {
    "base_url": "http://127.0.0.1:8502"
}
```

### 에이전트 환경변수 (`wave-home-agent/.env`)

```bash
GEMINI_API_KEY=...
WAVEHOME_CORE_API_BASE_URL=http://127.0.0.1:8500
WAVEHOME_AGENT_INTERNAL_BASE_URL=http://127.0.0.1:8501/internal/v1
WAVEHOME_CORE_API_MOCK=false
OLLAMA_BASE_URL=http://127.0.0.1:11434
```

## 4. 실행

프론트는 먼저 `./scripts/build/site.sh`(prod) / `site-demo.sh`(demo) / `site-test.sh`(test)로 빌드합니다.
전체 절차는 루트 [README.md](../README.md)의 **빠른 시작**을 보세요.

```bash
# Terminal 1 — backend
./scripts/run/prod.sh          # or ./scripts/run-prod.sh

# Terminal 2 — agent
cd wave-home-agent && source .venv/bin/activate
uvicorn app.main:app --reload --host 127.0.0.1 --port 8502
```

데모:

```bash
./scripts/run/demo.sh
# agent:
cd wave-home-agent && source .venv/bin/activate
uvicorn app.main:app --reload --host 127.0.0.1 --port 8512
```

## 5. 스모크

```bash
curl -s http://127.0.0.1:8500/api/v1/health
curl -s http://127.0.0.1:8502/health
curl -s -X POST http://127.0.0.1:8501/internal/v1/db/query \
  -H 'Content-Type: application/json' \
  -d '{"queries":[{"sql":"SELECT 1 AS ok"}]}'
```
