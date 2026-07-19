# 전력 관리

프로덕션 전력 파이프라인의 **계측·집계·스케줄·리포트/인사이트·metering** 을 한곳에 정리한다.

## 1. 개요

```
Tuya EP2H (1s sample)
        │
        ▼
PowerManager ──► power_energy (5m, per-plug + device_id NULL 합산)
        │
        ├─ 시 경계 → AgentJobQueue ← PowerReport 1h
        ├─ 자정    → 24h / 1w / 1mo / Insight(power)
        └─ 일요일  → 1yr
                │
                ▼
        wave-home-agent /power/v1 · /insight/v1
                │
                ▼
        power_report · vec_power_report · insight
```

핵심 파일:

| 파일 | 역할 |
|------|------|
| `service/power_manager.cpp` | 1초 샘플, 5m 적분, 스케줄 enqueue, metering 가드 |
| `web/http/v1/power_store.cpp` | 조회·리포트 생성·큐 잡 실행 |
| `service/agent/agent_job_queue.cpp` | 수면·전력·인사이트 단일 워커 우선순위 큐 |
| `device.settings.metering` | UI 비활성 시 집계 중단 (기본 true) |

## 2. 계측 (5분)

1. `PowerManager`가 `tuya_ep2h` 플러그마다 약 1초 간격으로 `readStatus` 호출.
2. 사다리꼴 적분으로 5분 버킷 `energy_wh` 누적.
3. 버킷 경계에서 `power_energy` upsert:
   - 플러그별: `device_id = DB id`
   - 가구 합: `device_id IS NULL` (metering=true 플러그만 합산)

장치 `enabled=false` / non-Running 이면 샘플은 0으로 두고 적산하지 않는 기존 동작과 별도로, **`settings.metering=false`는 실시간 UI 값은 유지하되 적산·합산 기여를 중단**한다.

## 3. Metering (UI 비활성)

- 저장: `device.settings_json.metering` (기본 생략=true).
- 프론트 [`PowerPage`](../wave-home-front/src/pages/power/PowerPage.js) 토글 → `PATCH` settings merge → `PowerManager::setMeteringEnabled`.
- 플러그 목록 API `metering` 필드로 표시 상태 동기화 (localStorage 미사용).

## 4. 스케줄

| 트리거 | enqueue |
|--------|---------|
| 시(時) 경계 | 직전 시각 `1h` 리포트 (가구 합) |
| 자정 (새 날짜 첫 틱) | 어제 `24h` → 롤링 `1w`(어제−6) → 롤링 `1mo`(어제−29) → `Insight(surface=power)` |
| 일요일 자정 배치 | 롤링 `1yr`(어제−364) |

스케줄러는 **enqueue만** 한다. 에이전트 폴링은 샘플링 스레드를 막지 않는다.

## 5. AgentJobQueue

단일 워커. 동시 실행 1개.

우선순위 (작을수록 먼저):

1. **처리중** — in-flight 선점 없음
2. **period_rank** — `30m/1h=0` → `24h·daily·insight=1` → `1w=2` → `1mo=3` → `1yr=4`
3. **domain_rank** — 수면(0) → 전력(1); `sleep_report` 인사이트는 수면 도메인

동일 `targetKey`는 queued/running이면 재enqueue 스킵.

HTTP 온디맨드(`ensure_hourly` / `ensure_daily`)는 `enqueueAndWait`(약 90초). 타임아웃 시 UI는 “리포트 준비 중”.

## 6. 리포트 페이로드

`generate_report`가 `metrics`(totalEnergyWh, coverage, byDevice, vsPrevPct, peakW/peakAt, days/avgDailyWh)와 `children`(하위 granularity 행)을 채워 에이전트에 전달한다.

## 7. 프론트 인사이트

`INSIGHT_CARD_LIMIT=4`, 최신 `date` 코호트 (변경 없음).

## 8. 관련 문서

- [`docs/db-schema.md`](db-schema.md) — `power_energy` / `power_report`
- [`docs/agent-api/power-analysis-api.md`](agent-api/power-analysis-api.md)
- [`docs/sleep_management.md`](sleep_management.md) — 동일 잡 큐 공유
