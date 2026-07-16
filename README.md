# WaveHome

![thumbnail](docs/images/thumbnail.png)

서강대학교 팀 **RADAI** - 2026 AI·SW 중심대학 디지털 경진대회(SW 부문)

사용자의 수면과 행동 패턴을 이해하고, AI Agent 기반 맞춤형 스마트홈 제어로 더 건강한 일상을 만드는 **홈 헬스 인텔리전스** 솔루션입니다.

비접촉 레이더·Wave Station으로 수면·바이탈·환경을 수집하고, 제스처·예약·룰로 가전을 제어하며, 멀티 에이전트(수면·전력·환경 + 총괄)가 인사이트와 자동화를 제공합니다. 웨어러블 없이 로컬 네트워크 중심으로 프라이버시를 지키는 것을 목표로 합니다.

핵심 구성은 **프론트(React SPA)** · **백엔드(C++ Drogon,** `wave-server`**)** · **에이전트(Python FastAPI + LangGraph)** 입니다.

---

## 폴더 구조

```text
wave-home2/
├── README.md
├── LICENSE
├── CMakeLists.txt              # 루트 CMake (wave-server / 테스트 타깃)
├── .gitmodules                 # wave-home-front, wave-home-agent, thirdparty 등
│
├── docs/                       # 설계·계약·스키마 문서 (아래 상세)
├── scripts/                    # 빌드·다운로드·에이전트 설정·실행 헬퍼
│   ├── build/                  # server / site / site-demo / site-test / test-*
│   ├── download/               # TTS·STT·pose 모델
│   ├── configure/              # agent-real / agent-demo / agent-dev
│   ├── run/                    # prod / demo / test  (루트에 run-*.sh 래퍼)
│   └── _lib/                   # cmake-build 등 공용 bash
│
├── src/
│   ├── wave-server/            # 백엔드 본체 (HTTP, DB, 장치, 데모, nn, service)
│   ├── common/                 # 공용 유틸
│   ├── r4sn/                   # 레이더/펌웨어·iq-server 관련
│   └── test/                   # 장치·STT/TTS·sleep-net 등 단독 테스트
│
├── bin/                        # 실행 시 작업 디렉터리 (바이너리·설정·런타임 데이터)
│   ├── wave-server             # [ignore] 빌드 산출물
│   ├── config.json             # real/demo/test 프로필·포트
│   ├── data/                   # DB·장치 목록 등 (database.db 등은 [ignore])
│   ├── models/                 # [ignore] TTS/STT/pose 가중치 일부
│   ├── device/ gestures/ …
│   └── test/                   # [ignore] 테스트 바이너리
│
├── demo/                       # 데모용 mock DB 생성 파이프라인 (scripts + 시나리오 md)
│   ├── scripts/                # 01~05 생성·에이전트 리포트·임베딩
│   │   └── _lib/
│   ├── ai_reports/ ai_manual/  # [ignore] 중간 JSON
│   └── demo.md / sleep.md / power.md
│
├── sleep-net/                  # 수면 인식 학습·전처리·export (온디바이스 ncnn)
│   ├── networks/ scripts/
│   ├── dataset/ samples/ models/ training/   # [ignore]
│   └── test/                   # 로그 분석용 (대용량 csv·plots [ignore])
│
├── gesture-net/                # 제스처 인식 학습·전처리·export
│   ├── networks/ scripts/ pcr_loader.py
│   └── dataset/ samples/ models/ training/   # [ignore]
│
├── wave-home-front/            # [submodule] React SPA 소스
├── wave-home-agent/            # [submodule] LangGraph 에이전트 소스
│
├── site/ site-demo/ site-test/ # [ignore] 프론트 빌드 배포물
├── build/                      # [ignore] CMake 빌드 트리
├── cmake/                      # Find* / deps / macos 등 CMake 모듈
├── thirdparty/                 # drogon, ncnn, sherpa-onnx, lzav, … (일부 submodule)
└── .deps/ .cache/ .venv/       # [ignore] 툴체인·캐시·venv
```



### `docs/` 상세


| 경로                                                            | 설명                                                                         |
| ------------------------------------------------------------- | -------------------------------------------------------------------------- |
| `[ports.txt](docs/ports.txt)`                                 | **포트 SSOT** — prod/demo/test의 client-api · backend agent-api · agent 서버 포트 |
| `[wave-server-boundaries.md](docs/wave-server-boundaries.md)` | 공개 `/api/v1` · `/internal/v1` · 에이전트 outbound 경계, 데모 불변식, 리팩터 페이즈          |
| `[agent-integration.md](docs/agent-integration.md)`           | 백엔드↔에이전트 로컬 기동·`.env`·스모크 curl                                             |
| `[db-schema.md](docs/db-schema.md)`                           | SQLite 스키마(수면·전력·채팅·룰·알람 등)                                                |
| `[style.md](docs/style.md)`                                   | C++/프로젝트 코드 스타일                                                            |
| `[agent-api/](docs/agent-api/README.md)`                      | **백엔드↔에이전트 HTTP 계약** 모음                                                    |
| `agent-api/chat-api.md`                                       | 대화 SSE (`/chat/v1`)                                                        |
| `agent-api/forwarding-api.md`                                 | LLM·임베딩 포워딩 (`/llm/v1`)                                                    |
| `agent-api/sleep-analysis-api.md`                             | 수면 요약·리포트 job (`/sleep/v1`)                                                |
| `agent-api/power-analysis-api.md`                             | 전력 리포트 job (`/power/v1`)                                                   |
| `agent-api/insight-generation-api.md`                         | 인사이트 배치 (`/insight/v1`)                                                    |
| `agent-api/weekly-plan-analysis-api.md`                       | 주간 배너 리포트 (`/weekly-plan/v1`)                                              |
| `agent-api/posture-analysis-api.md`                           | 자세 분석 경로 예약 (`/posture/v1`)                                                |
| `agent-api/device-tool-api.md`                                | 장치·룰·IR·`tools/device.*` (`/internal/v1`)                                  |
| `agent-api/alarms-api.md`                                     | 알람 CRUD (에이전트→백엔드)                                                         |
| `agent-api/schedule-tasks-api.md`                             | 일정 CRUD                                                                    |
| `agent-api/db-query-api.md`                                   | 배치 DB 조회                                                                   |
| `agent-api/rag-api.md`                                        | 벡터 RAG 검색                                                                  |
| `[wave-station/](docs/wave-station/)`                         | Wave Station(ESP32) 프로토콜·연동 가이드                                            |
| `wave-station/wave-station-protocol.md`                       | TCP 프로토콜                                                                   |
| `wave-station/wave-station-esp32-guide.md`                    | 펌웨어/백엔드 연동                                                                 |
| `images/`                                                     | 문서용 스크린·다이어그램 (sleep/power 등)                                              |
| `ppt/`                                                        | [ignore] 발표용 HTML 등 로컬 자료                                                  |


---



## 서브모듈

```bash
git submodule update --init --recursive
```


| 경로                | 설명                                                 |
| ----------------- | -------------------------------------------------- |
| `wave-home-front` | React SPA                                          |
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