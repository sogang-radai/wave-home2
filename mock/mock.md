# Mock DB 생성 가이드

`docs/db-schema.md`(2026-07-08 버전) 기준으로 2026년 6월 1일~30일(30일) 목업 데이터를 생성한다.
사용자는 `김건강`(user_id=1, 침실에서 취침)과 `박헬스`(user_id=2, 침실에서 취침하지 않음— 수면 데이터 없음)
두 명이다.

관련 문서: [`sleep.md`](./sleep.md)(수면 시나리오), [`power.md`](./power.md)(전력 시나리오).

## 폴더 구조

```
mock/
├── mock.md                    # 이 문서
├── sleep.md                   # 수면 30일 시나리오(레이더+삼성헬스 분석 포함)
├── power.md                   # 전력 시나리오(플러그별 모델)
├── data/
│   └── mock.db                # 최종 산출물(SQLite, sqlite-vec 포함)
├── scripts/
│   ├── lib/                   # 공용 모듈(schema, devices, timeutil, ollama_client, agent_client,
│   │                          #  power_model, sleep_model, sleep_scenario, narrative, manual_texts)
│   ├── 01_gen_raw_data.py      # AI 불필요 원시 데이터 전체(수면 제외)
│   ├── 01b_gen_sleep_raw.py    # sleep_session/sleep_stat 원시 데이터
│   ├── 02_call_agent_reports.py# 에이전트(:8501) 실호출 — 수면 daily/weekly, 전력 24h/1w/1mo
│   ├── 03_gen_manual_ai_texts.py # 에이전트 미지원 영역 수동 작성 + 1h전력/30m수면 템플릿
│   ├── 04_embed_manual_texts.py  # 03 산출물을 Ollama로 직접 임베딩
│   └── 05_load_ai_json_to_db.py  # 02+03/04 산출물을 최종 mock.db에 반영
├── ai_reports/                 # 02 산출물(중간 JSON, git 커밋 대상 아님 권장)
│   ├── power_reports.json
│   └── sleep_reports.json
└── ai_manual/                   # 03/04 산출물(중간 JSON)
    ├── insight.json
    ├── weekly_plan_report.json
    ├── power_report_1h.json
    └── sleep_stat_30m_summary.json
```

## 실행 순서(전체 재생성 시)

사전 준비:

1. Ollama가 로컬에서 떠 있어야 한다(`nomic-embed-text` 모델 필요). 기본 `http://127.0.0.1:11434`.
2. 에이전트 서버(`wave-home-agent`)가 `:8501`에서 떠 있어야 한다(02 단계에만 필요):
   ```bash
   cd wave-home-agent
   .venv/bin/uvicorn app.main:app --port 8501
   ```
   헬스체크: `curl http://127.0.0.1:8501/health`

실행:

```bash
cd mock/scripts
python3 01_gen_raw_data.py        # DB 새로 생성 + 스키마 + AI 불필요 데이터 전체
python3 01b_gen_sleep_raw.py      # sleep_session / sleep_stat(1m, 30m + 템플릿 summary_text)
python3 02_call_agent_reports.py  # 에이전트 실호출(아래 "02 단계 상세" 참고, 시간이 오래 걸림)
python3 03_gen_manual_ai_texts.py # insight / weekly_plan_report / 1h전력 / 30m수면 텍스트 작성
python3 04_embed_manual_texts.py  # 위 텍스트 Ollama 임베딩
python3 05_load_ai_json_to_db.py  # 02+03/04 결과를 mock.db에 반영(여러 번 실행해도 안전)
```

## 02 단계 상세 — 에이전트 실호출 (가장 오래 걸리는 단계)

축소 범위(합의된 스코프): **실제 에이전트(로컬 Ollama `gemma4:12b-mlx`) 호출은 수면
daily(30) + weekly(5) = 35건, 전력 24h(30) + 1w(24) + 1mo(1) = 55건, 총 90건만** 수행한다.
나머지(전력 1h 720건, 수면 30m 요약 494건)는 03 단계에서 스크립트가 템플릿 텍스트로 채우고
Ollama로 직접 임베딩한다(LLM 생성 호출 없음).

**속도**: 이 PC에서 `gemma4:12b-mlx` 1건 생성에 약 50~270초가 걸린다(동시 호출 시 서로
경합해서 오히려 더 느려지므로 `MAX_WORKERS=1`로 직렬 실행 권장). 90건 전체를 돌리면
**대략 1.5~2시간** 소요된다. 그래서 이 단계는 백그라운드로 오래 켜두고(예: 밤새) 완료를
기다리는 것을 전제로 한다.

- 더 빠른 로컬 모델(`gemma2:2b`)로 바꾸면 건당 5~10초로 줄지만, 한국어 리포트 품질이
  눈에 띄게 떨어진다(어색한 문장, 숫자 오용). 필요하면 요청 Body에 `"model": "gemma2:2b"`를
  추가해 바꿀 수 있다(`docs/agent-api/sleep-analysis-api.md`·`power-analysis-api.md`의
  `model` 옵션 참고).

**재실행/이어하기**: `02_call_agent_reports.py`는 실행할 때마다
`mock/ai_reports/power_reports.json`·`sleep_reports.json`을 읽어 **이미 끝난 대상은
건너뛴다**. 매 건 완료 후 즉시 파일에 체크포인트 저장하므로, 중간에 서버를 껐다 켜도
`python3 02_call_agent_reports.py`를 다시 실행하면 남은 것만 이어서 진행한다.

**주의 — 절대 동시에 2개 이상 실행하지 말 것**: 같은 `mock.db`·같은 에이전트 서버를
쓰기 때문에, 두 인스턴스를 동시에 켜면 같은 대상에 대해 `JOB_ALREADY_RUNNING` 오류가
나고, 체크포인트 파일에 경쟁 상태(race condition)가 생겨 진행 상황이 덮어써질 수 있다.
실행 전 `ps aux | grep 02_call_agent_reports`로 기존 프로세스가 없는지 꼭 확인한다.
같은 이유로 에이전트 서버(uvicorn)도 항상 하나만 띄운다(`curl :8501/health`로 확인).

**진행 상황 확인**:

```bash
# 남은/완료 건수
python3 -c "import json; print('power', len(json.load(open('../ai_reports/power_reports.json'))))"
python3 -c "import json; print('sleep', len(json.load(open('../ai_reports/sleep_reports.json'))))"
# 목표: power 55건, sleep 35건
```

완료 후 `05_load_ai_json_to_db.py`를 실행해 `mock.db`에 반영한다.

## 검증 쿼리(로드 후)

```sql
SELECT COUNT(*) FROM power_report WHERE period IN ('24h','1w','1mo');  -- 55
SELECT COUNT(*) FROM power_report WHERE period='1h';                   -- 720
SELECT COUNT(*) FROM sleep_report;                                     -- 35
SELECT COUNT(*) FROM sleep_stat WHERE granularity='30m' AND summary_text IS NOT NULL; -- 494
SELECT COUNT(*) FROM insight;                                          -- ~23
SELECT COUNT(*) FROM weekly_plan_report;                               -- 60 (30일 x 2명 슬라이딩)
SELECT COUNT(*) FROM vec_power_report;   -- 775 (55+720, sqlite-vec 로드 성공 시)
SELECT COUNT(*) FROM vec_sleep_report;   -- 35
SELECT COUNT(*) FROM vec_sleep_stat;     -- 494
```

## 참고 — 자세(posture) 데이터는 생성하지 않음

`posture_stat`/`posture_report`/`vec_posture_report`는 스펙이 아직 확정되지 않아 스키마만
만들고 데이터는 채우지 않는다. `insight.surface='posture_report'`는 실데이터 없이 "준비 중"
안내 1건만 넣어 5개 surface 값을 모두 커버한다.
