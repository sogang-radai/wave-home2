# 에이전트 서버 연동 가이드

백엔드(`wave-server`, :8500)와 에이전트(`wave-home-agent`, :8501)를 함께 띄우고 스모크 테스트하는 절차.

## 저장소 구조

```
wave-home2/
├── wave-home-agent/     # git submodule (Python FastAPI + LangGraph)
├── bin/                 # 백엔드 실행·설정 (config.json)
├── docs/
│   ├── agent-api/           # API 계약 (역할별 분할, device-tool-api 포함)
│   ├── agent-tool-api.md    # → agent-api/ 리다이렉트
│   └── device-tool-api.md   # → agent-api/device-tool-api.md 리다이렉트
└── scripts/
    ├── setup-agent-submodule.sh
    └── test-agent-integration.sh
```

## 1. 서브모듈 추가 (최초 1회)

`wave-home-agent`는 **비공개** GitHub 저장소다. SSH 키 또는 HTTPS 로그인 후:

```bash
chmod +x scripts/*.sh

# 클론 + submodule gitlink 등록 (한 번에)
./scripts/setup-agent-submodule.sh

# 이미 git clone 으로 받아 둔 경우
./scripts/register-agent-submodule.sh
```

수동:

```bash
git clone https://github.com/sogang-radai/wave-home-agent.git wave-home-agent
./scripts/register-agent-submodule.sh
git commit -m "Add wave-home-agent submodule"
```

이후 `wave-home-agent/README.md` 를 따른다.

### 에이전트 개발 환경

```bash
./scripts/bootstrap-agent-dev.sh   # .env, venv, pip install
```

## 2. 포트·역할

| 서비스 | 포트 | Base URL |
|--------|------|----------|
| 백엔드 (real) | 8500 | `http://127.0.0.1:8500` |
| 백엔드 (demo) | 8502 | `http://127.0.0.1:8502` |
| 백엔드 (test) | 8503 | `http://127.0.0.1:8503` |
| 에이전트 | 8501 | `http://127.0.0.1:8501` |

프로필·`/internal` 격리 목표(18500/18502)는 [wave-server-boundaries.md](./wave-server-boundaries.md)가 SSOT다.

**호출 방향**

- 프론트 → 백엔드: `/api/v1/*`
- 백엔드 → 에이전트: `/chat/v1`, `/llm/v1`, `/sleep/v1`, `/power/v1`, `/insight/v1`, …
- 에이전트 → 백엔드: `/internal/v1/*` (`db/query`, `rag/search`, `tools/device.*` 등)  
  - real: `scripts/configure-agent-real.sh`  
  - demo: `scripts/configure-agent-demo.sh`

## 3. 설정

### 백엔드 `bin/config.json`

```json
"agent": {
    "base_url": "http://127.0.0.1:8501"
}
```

(백엔드가 에이전트를 호출하는 프록시·분석 잡 구현 시 사용. 현재는 참고용.)

### 에이전트 환경변수 (`wave-home-agent/.env`)

`bootstrap-agent-dev.sh` 가 `.env.example` 을 복사한다. 핵심 값:

```bash
GEMINI_API_KEY=...                    # 채팅·리포트 LLM
WAVEHOME_AGENT_INTERNAL_BASE_URL=http://127.0.0.1:8500/internal/v1
WAVEHOME_CORE_API_MOCK=true           # internal API 미구현 시 mock tool
OLLAMA_BASE_URL=http://127.0.0.1:11434  # /llm/v1, embed job
```

`WAVEHOME_CORE_API_MOCK=true` 이면 백엔드 `/internal/v1` 없이도 채팅 tool mock 동작.
백엔드 연동 테스트 시 `false` 로 바꾸고 internal API 구현 후 재시도.

## 4. 실행 순서

**터미널 1 — 백엔드**

```bash
cd bin
../build/wave-server    # 또는 프로젝트 빌드 산출물 경로
```

**터미널 2 — 에이전트**

`wave-home-agent/README.md` 의 실행 명령 (예: `uv run uvicorn ... --port 8501`).

## 5. 스모크 테스트

```bash
# 서브모듈·README만 확인
./scripts/test-agent-integration.sh --check-only

# HTTP 프로브 (양쪽 서버 기동 후)
./scripts/test-agent-integration.sh
./scripts/test-agent-integration.sh --verbose
```

### 수동 확인

**백엔드**

```bash
curl -s http://127.0.0.1:8500/api/v1/health | jq .
```

**에이전트**

```bash
curl -s http://127.0.0.1:8501/llm/v1/models | jq .
curl -N -X POST http://127.0.0.1:8501/chat/v1/turns \
  -H 'Content-Type: application/json' \
  -d '{"chatHistoryId":1,"userId":1,"messages":[{"role":"user","content":"안녕"}],"stream":true}'
```

**에이전트 → 백엔드 (internal, 구현 후)**

```bash
curl -s -X POST http://127.0.0.1:8500/internal/v1/db/query \
  -H 'Content-Type: application/json' \
  -d '{"queries":[{"table":"device","filter":{"archived":0},"limit":5}]}'
```

## 6. 구현 상태 체크리스트

연동 전에 백엔드·에이전트 각각 아래를 맞춘다.

| 기능 | 백엔드 | 에이전트 |
|------|--------|----------|
| Health | `GET /api/v1/health` | `/health` 또는 FastAPI `/docs` |
| Chat 프록시 | `POST /api/v1/chat/...` → agent `/chat/v1/turns` | `POST /chat/v1/turns` |
| DB 조회 tool | `POST /internal/v1/db/query` | httpx → 위 URL |
| RAG tool | `POST /internal/v1/rag/search` | httpx |
| 장치 tool | `POST /internal/v1/tools/device.*` | LangGraph tools |
| 수면/전력 분석 | agent 호출 + DB body 전달 | `/sleep/v1`, `/power/v1` |

`test-agent-integration.sh` 가 `/internal/v1/*` 에 404 를 내면 백엔드 internal API 가 아직 없는 것이다 (`docs/agent-api/` 구현 후 재테스트).

## 7. 트러블슈팅

| 증상 | 조치 |
|------|------|
| submodule clone 실패 | GitHub 로그인, repo 접근 권한 확인 |
| Agent `Connection refused` | :8501 기동 여부, README 포트 확인 |
| Chat 502 / LLM error | 에이전트 `.env` 의 모델·API 키 |
| tool 호출 404 | 백엔드 `/internal/v1` 라우트 미구현 |
| DB empty | `uv run scripts/gen_mock_data.py` 로 mock DB 생성 |

## 8. 관련 문서

- `docs/agent-api/README.md` — 챗, 분석, db/query, RAG, device-tool 계약 (분할 문서 인덱스)
- `docs/agent-api/device-tool-api.md` — 장치·룰·예약 Tool API
- `docs/db-schema.md` — SQLite 스키마
- `wave-home-agent/README.md` — 에이전트 설치·실행 (서브모듈 클론 후)
