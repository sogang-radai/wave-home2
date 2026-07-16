# wave-home2

스마트홈 백엔드(C++), 프론트(React), 에이전트(Python) 모노레포.

## 서브모듈

```bash
git submodule update --init --recursive
```

| 경로 | 설명 |
|------|------|
| `wave-home-front` | React SPA |
| `wave-home-agent` | FastAPI + LangGraph 에이전트 (:8501) |

에이전트 최초 클론(비공개 repo) — **본인 터미널**에서 실행:

```bash
git clone https://github.com/sogang-radai/wave-home-agent.git wave-home-agent
./scripts/register-agent-submodule.sh
./scripts/bootstrap-agent-dev.sh
```

## 연동 테스트

```bash
./scripts/test-agent-integration.sh --check-only   # README·환경
./scripts/test-agent-integration.sh              # :8500 / :8501 스모크
```

자세한 절차: [docs/agent-integration.md](docs/agent-integration.md)  
API 계약: [docs/agent-tool-api.md](docs/agent-tool-api.md)  
포트·경계: [docs/wave-server-boundaries.md](docs/wave-server-boundaries.md)
