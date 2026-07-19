# Demo DB 생성 가이드

`docs/db-schema.md`(2026-07-08 버전) 기준으로 2026년 6월 1일~30일(30일) 목업 데이터를 생성한다.
사용자는 `김건강`과 `박헬스`두 명이다.

관련 문서: [`sleep.md`](./sleep.md)(수면 시나리오), [`power.md`](./power.md)(전력 시나리오), [`chat.md`](./chat.md)(채팅 시나리오).

## 폴더 구조

```
demo/
├── demo.md                       # 이 문서
├── sleep.md                      # 수면 30일 시나리오
├── power.md                      # 전력 시나리오
├── chat.md                       # 채팅 시나리오(검수·첫 대사)
├── scripts/
│   ├── _lib/
│   ├── 01_gen_raw_data.py
│   ├── 01b_gen_sleep_raw.py
│   ├── 02_call_agent_reports.py  # 에이전트 실호출 → demo/agent/*_reports.json
│   ├── 03_gen_manual_ai_texts.py
│   ├── 04_embed_manual_texts.py
│   ├── 05_load_ai_json_to_db.py  # agent/·ai_manual/ → demo.db
│   └── 06_collect_chat_turns.py  # 채팅 실호출 → demo/agent/chat.json
├── agent/                        # 에이전트 실호출 산출물
│   ├── power_reports.json        # [ignore] 02 산출
│   ├── sleep_reports.json        # [ignore] 02 산출
│   └── chat.json                 # 06 산출(대화+툴콜, DB 시드용)
└── ai_manual/                    # 03/04 산출물(중간 JSON)
    ├── insight.json
    ├── weekly_plan_report.json
    ├── power_report_1h.json
    └── sleep_stat_30m_summary.json

최종 DB 산출물: `bin/data/demo.db` (SQLite, sqlite-vec 포함)
```

## 실행 순서(전체 재생성 시)

사전 준비:

1. Ollama가 로컬에서 떠 있어야 한다(`nomic-embed-text` 모델 필요). 기본 `http://127.0.0.1:11434`.
2. 에이전트 서버(`wave-home-agent`)가 `:8502`에서 떠 있어야 한다(02 단계에만 필요):
   ```bash
   cd wave-home-agent
   .venv/bin/uvicorn app.main:app --port 8502
   ```
   헬스체크: `curl http://127.0.0.1:8502/health`
3. (선택) `vec_*` 테이블까지 채우려면 Python 환경에 `sqlite-vec`가 있어야 한다.
   없으면 `05_load_ai_json_to_db.py`가 `sleep_report`·`power_report` 등 본 테이블만 반영하고
   `vec_*`는 건너뛴다(데모 UI 조회에는 영향 없음, RAG/임베딩 검색만 비어 있음).
   ```bash
   uv run --with sqlite-vec demo/scripts/05_load_ai_json_to_db.py
   ```

실행:

```bash
cd demo/scripts
python3 01_gen_raw_data.py        # DB 새로 생성 + 스키마 + AI 불필요 데이터 전체
python3 01b_gen_sleep_raw.py      # sleep_session / sleep_stat(1m, 30m + 템플릿 summary_text)
python3 02_call_agent_reports.py  # 에이전트 실호출(아래 "02 단계 상세" 참고, 시간이 오래 걸림)
python3 03_gen_manual_ai_texts.py # insight / weekly_plan_report / 1h전력 / 30m수면 텍스트 작성
python3 04_embed_manual_texts.py  # 위 텍스트 Ollama 임베딩
python3 05_load_ai_json_to_db.py  # 02+03/04 결과를 demo.db에 반영(여러 번 실행해도 안전)
```

데모 서버 반영: `05` 이후 `wave-server --profile demo`를 재시작하고 브라우저를 새로고침한다.
프론트는 `demo.db`를 직접 읽지 않고 API를 통해 조회한다.

## 02 단계 상세 — 에이전트 실호출 (가장 오래 걸리는 단계)

축소 범위(합의된 스코프): **실제 에이전트(로컬 Ollama `gemma4:12b-mlx`) 호출은 수면
daily(30) + weekly(24, 롤링 7일 창) = 54건, 전력 24h(30) + 1w(24) + 1mo(1) = 55건, 총 109건만** 수행한다.
나머지(전력 1h 720건, 수면 30m 요약 494건)는 03 단계에서 스크립트가 템플릿 텍스트로 채우고
Ollama로 직접 임베딩한다(LLM 생성 호출 없음).

### 수면 weekly — 롤링 7일 창

달력 주(월~일)가 아니라 **매일 기준 최근 7박** 슬라이딩 창이다.

| 항목 | 값 |
| --- | --- |
| `period` | `weekly` |
| `period_start` | 창의 **첫날** (`YYYY-MM-DD`) |
| 생성 대상 | 6/7~6/30 각 창 첫날, **24건** (`sliding_week_starts` in `lib/timeutil.py`) |
| 데모 앵커 | `2026-06-30` → UI/API `weekStart=2026-06-24` (6/24~6/30, 7박) |

예: `period_start=2026-06-24`이면 `night_date`가 6/24, 6/25, …, 6/30인 세션 7개를 집계한다.
에이전트·백엔드·프론트 모두 **월요일 검증을 하지 않는다**(과거 달력 주 스펙과 다름).

**속도**: 이 PC에서 `gemma4:12b-mlx` 1건 생성에 약 50~270초가 걸린다(동시 호출 시 서로
경합해서 오히려 더 느려지므로 `MAX_WORKERS=1`로 직렬 실행 권장). 109건 전체를 돌리면
**대략 1.5~2시간** 소요된다. 그래서 이 단계는 백그라운드로 오래 켜두고(예: 밤새) 완료를
기다리는 것을 전제로 한다.

- 더 빠른 로컬 모델(`gemma2:2b`)로 바꾸면 건당 5~10초로 줄지만, 한국어 리포트 품질이
  눈에 띄게 떨어진다(어색한 문장, 숫자 오용). 필요하면 요청 Body에 `"model": "gemma2:2b"`를
  추가해 바꿀 수 있다(`docs/agent-api/sleep-analysis-api.md`·`power-analysis-api.md`의
  `model` 옵션 참고).

**재실행/이어하기**: `02_call_agent_reports.py`는 실행할 때마다
`demo/agent/power_reports.json`·`sleep_reports.json`을 읽어 **이미 끝난 대상은
건너뛴다**. 매 건 완료 후 즉시 파일에 체크포인트 저장하므로, 중간에 서버를 껐다 켜도
`python3 02_call_agent_reports.py`를 다시 실행하면 남은 것만 이어서 진행한다.

**주의 — 절대 동시에 2개 이상 실행하지 말 것**: 같은 `demo.db`·같은 에이전트 서버를
쓰기 때문에, 두 인스턴스를 동시에 켜면 같은 대상에 대해 `JOB_ALREADY_RUNNING` 오류가
나고, 체크포인트 파일에 경쟁 상태(race condition)가 생겨 진행 상황이 덮어써질 수 있다.
실행 전 `ps aux | grep 02_call_agent_reports`로 기존 프로세스가 없는지 꼭 확인한다.
같은 이유로 에이전트 서버(uvicorn)도 항상 하나만 띄운다(`curl :8502/health`로 확인).

**진행 상황 확인**:

```bash
cd demo/scripts
python3 -c "import json; print('power', len(json.load(open('../agent/power_reports.json'))))"
python3 -c "
import json
rows = json.load(open('../agent/sleep_reports.json'))
daily = sum(1 for r in rows if r['period']=='daily')
weekly = sum(1 for r in rows if r['period']=='weekly')
print('sleep', len(rows), f'(daily {daily} + weekly {weekly})')
"
# 목표: power 55건, sleep 54건 (daily 30 + weekly 24)
```

02가 아래처럼 끝나면 다음 단계로 넘어간다:

```
=== 02_call_agent_reports 완료 ===
power_reports: 55 -> .../demo/agent/power_reports.json
sleep_reports: 54 -> .../demo/agent/sleep_reports.json
```

### 주간 리포트만 다시 생성할 때

에이전트 스펙·프롬프트를 바꿨거나 weekly만 갱신하고 싶을 때:

```bash
cd demo/scripts
# sleep_reports.json 에서 period=="weekly" 항목만 제거(또는 파일 백업 후 weekly 삭제)
python3 -c "
import json
from pathlib import Path
p = Path('../agent/sleep_reports.json')
rows = json.load(open(p))
kept = [r for r in rows if r.get('period') != 'weekly']
print('before', len(rows), 'after', len(kept), 'weekly removed', len(rows)-len(kept))
p.write_text(json.dumps(kept, ensure_ascii=False, indent=2), encoding='utf-8')
"
python3 02_call_agent_reports.py   # weekly 24건만 재호출
python3 05_load_ai_json_to_db.py   # 03/04는 건너뛰어도 됨(weekly만 바뀐 경우)
```

에이전트 코드를 수정했다면 **uvicorn을 재시작**한 뒤 02를 돌린다.
`INVALID_WEEK_START`(월요일 오류)가 나오면 에이전트가 구버전인 것 — `wave-home-agent` 최신
`app/services/sleep_analysis.py`를 반영했는지 확인한다.

## 03~05 단계 — 02 완료 후 반영

| 단계 | 하는 일 | 02만 끝난 경우 |
| --- | --- | --- |
| `03_gen_manual_ai_texts.py` | insight·weekly_plan·1h전력·30m수면 템플릿 작성. `sleep_reports.json`의 weekly는 DB 메트릭으로 **본문만** 재병합(에이전트 `report_text`·`embedding`은 daily·weekly 모두 유지) | weekly만 갱신했으면 생략 가능 |
| `04_embed_manual_texts.py` | 03 산출물 Ollama 임베딩 | 03을 안 돌렸으면 생략 |
| `05_load_ai_json_to_db.py` | `agent`·`ai_manual` → `demo.db` | **필수** |
| `06_collect_chat_turns.py` | 채팅 시나리오 실호출 → `agent/chat.json` | 채팅 시드 |
| `07_load_chat_json_to_db.py` | `agent/chat.json` → `demo.db` `chat_history` 교체 | 채팅 DB 반영 |

```bash
cd demo/scripts
python3 03_gen_manual_ai_texts.py
python3 04_embed_manual_texts.py
python3 05_load_ai_json_to_db.py
# 채팅 시드(에이전트 :8512 + demo 백엔드 필요, 턴 단위)
python3 06_collect_chat_turns.py start S01
python3 06_collect_chat_turns.py turn S01 "다음 대사"
python3 06_collect_chat_turns.py done S01
# 수집 완료 후 DB 반영(기존 chat_history 삭제 후 교체)
python3 07_load_chat_json_to_db.py
```

`05` 정상 출력 예:

```
power_report: 775건
sleep_report: 54건
sleep_stat(30m summary): 494건
insight: 23건
weekly_plan_report: 60건
=== 05_load_ai_json_to_db 완료 ===
```

`sqlite-vec` 미설치 시 `vec_* 테이블 반영을 건너뜁니다` 경고가 나와도 위 본 테이블 건수는 동일하다.

## 검증 쿼리(로드 후)

```bash
sqlite3 bin/data/demo.db
```

```sql
-- 에이전트 실호출 분
SELECT COUNT(*) FROM power_report WHERE period IN ('24h','1w','1mo');  -- 55
SELECT COUNT(*) FROM sleep_report;                                     -- 54
SELECT COUNT(*) FROM sleep_report WHERE period='daily';                -- 30
SELECT COUNT(*) FROM sleep_report WHERE period='weekly';               -- 24

-- 03 템플릿 분
SELECT COUNT(*) FROM power_report WHERE period='1h';                   -- 720
SELECT COUNT(*) FROM sleep_stat WHERE granularity='30m' AND summary_text IS NOT NULL; -- 494
SELECT COUNT(*) FROM insight;                                          -- 23
SELECT COUNT(*) FROM weekly_plan_report;                               -- 60

-- 롤링 주간 범위·데모 창
SELECT MIN(period_start), MAX(period_start) FROM sleep_report WHERE period='weekly';
-- 2026-06-07 | 2026-06-30
SELECT report_text FROM sleep_report WHERE period='weekly' AND period_start='2026-06-24';
-- 데모 UI 주간 탭(앵커 6/30)에 쓰이는 AI 요약

-- vec_* (sqlite-vec 로드 성공 시)
SELECT COUNT(*) FROM vec_power_report;   -- 775 (55+720)
SELECT COUNT(*) FROM vec_sleep_report;   -- 54
SELECT COUNT(*) FROM vec_sleep_stat;     -- 494
```

데모 UI 확인: 수면 페이지 주간 탭 → 기간 `6/24(수)~6/30(화)`, 7일 트렌드·하단 카드·AI 요약 문단.

## 참고 — 자세(posture) 데이터는 생성하지 않음

`posture_stat`/`posture_report`/`vec_posture_report`는 스펙이 아직 확정되지 않아 스키마만
만들고 데이터는 채우지 않는다. `insight.surface='posture_report'`는 실데이터 없이 "준비 중"
안내 1건만 넣어 5개 surface 값을 모두 커버한다.
