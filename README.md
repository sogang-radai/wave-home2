# wave-home2

스마트홈 백엔드(C++), 프론트(React), 에이전트(Python) 모노레포.

## 서브모듈

```bash
git submodule update --init --recursive
```

| 경로 | 설명 |
|------|------|
| `wave-home-front` | React SPA |
| `wave-home-agent` | FastAPI + LangGraph 에이전트 (prod :8502 / demo :8512) |

에이전트 최초 클론(비공개 repo) — **본인 터미널**에서 실행:

```bash
git clone https://github.com/sogang-radai/wave-home-agent.git wave-home-agent
./scripts/configure/agent-dev.sh
```

## 빌드·실행

```bash
./scripts/build/server.sh
./scripts/build/site.sh          # production front → site/
./scripts/run/prod.sh            # backend client :8500, agent-api :8501
# agent: uvicorn … --port 8502
```

데모: `./scripts/build/site-demo.sh` → `./scripts/run/demo.sh` (client :8510 / agent-api :8511 / agent :8512)

포트: [docs/ports.txt](docs/ports.txt) · [docs/wave-server-boundaries.md](docs/wave-server-boundaries.md)  
API 계약: [docs/agent-api/README.md](docs/agent-api/README.md)
