# 수면 관리 (Sleep Management)

프로덕션 수면 파이프라인의 **신호 처리·세션·집계·리포트/인사이트·장치 선택**을 한곳에 정리한다.  
구현 위치: `src/wave-server/service/sleep/*`, 모델 `bin/models/sleep0-*`, 연구용 DSP 참고 `src/test/test-iq/`.

관련: [수면 단계 심사 대응](ppt/sleep-stage-defense.md) (`docs/ppt/` — 로컬 전용일 수 있음), [Sleep Analysis API](agent-api/sleep-analysis-api.md).

---

## 1. 전체 흐름

```text
srs_r4sn (point cloud + IQ)
        │
        ▼
 SleepManager::tickRuntime
   ├─ SleepPipeline (PointNet+LSTM) → absent / awake / asleep + toss
   ├─ VitalTargetPicker → atan2 + 칼만 → az/el/distance
   ├─ VitalSignsProcessor → 5 range bin IQ → HR/BR
   ├─ Wave Station mic/env (settings.sleep=true 인 경우)
   └─ 초→분→(세션 기준)30분 집계 + SessionFsm
        │
        ├─ 세션 onset 후 30분마다 → Summary30m 잡 → agent /sleep/v1/summaries
        └─ 세션 close → DailyReport + WeeklyReport(롤링 7일) 잡
                              → insights (surface=sleep_report)
```

런타임은 **방(`roomId`)당 하나**. 레이더가 없으면 런타임을 만들지 않는다. Wave Station은 선택(마이크·조도).

---

## 2. 장치 선택

### 2.1 설정

- 레이더 `srs_r4sn`: `settings.sleep: true` (예: 침실 하방). 책상 레이더는 `false`.
- Wave Station: `settings.sleep: true` 이면 수면용 스테이션 후보.
- SSOT 예: `bin/device/device_list.json`, `bin/data/device_list.json`.

### 2.2 선택 규칙 (`SleepManager::loadRoomConfigs`)

- `ORDER BY room_id, user_id, device.id` 순으로 스캔.
- sleep=true 레이더 / Wave Station 각각 **처음 등장한 것만** 채택 (덮어쓰지 않음).
- 레이더 없음 → 해당 (room,user) 스킵.
- Wave Station 없음 → `stationExternalId` 비움, mic/env 스킵, 세션 `station_id` NULL 가능.

프론트에서 수면 레이더·스테이션을 고르는 UI는 추후. 지금은 플래그 + first-wins.

---

## 3. SleepNet (재실 / 수면 / 뒤척임)

| 항목 | 내용 |
|------|------|
| 입력 | 포인트 클라우드 프레임 (`SleepPipeline::pushFrame`) |
| 출력 | `absent` / `awake` / `asleep`, toss (`calm` / `slight` / `moderate`) |
| 모델 | `bin/models/sleep0-2` 등 (`AppConfig.sleep_model_path`) |
| 단계(light/deep/REM) | **모델 출력 아님** → §6 휴리스틱 |

초·분 집계기는 `sleep_aggregator`가 SleepNet 샘플을 비율·toss 통계로 묶는다.

---

## 4. 타겟 추적 → IQ 빔 (range / 각도)

파일: `sleep_vitals.cpp` — `VitalTargetPicker`.

1. `targetId ≤ 254` 포인트의 **power-weighted centroid** (x,y,z).
2. 구면 변환:
   - `distance = ‖xyz‖`
   - `azimuth = atan2(x, y)`
   - `elevation = atan2(z, hypot(x,y))`
3. **1D 상수속도 칼만**으로 az / el / distance 각각 스무딩 (EMA 대체).
4. `tickVitals`: 중심 range bin ±2 → **거리 5개**에 동일 az/el로 `RadarIQRequest` 전송.  
   bin 간격 ≈ `0.071565 m` (`VitalTargetPicker::kRangeBinSizeM`).

Sleeping 페이즈에서만 vitals 틱. 약 20 워커 틱마다 IQ 1회.

---

## 5. I/Q → 심박 / 호흡

파일: `VitalSignsProcessor` (test-iq `vital_signs.py` 이식). FFT는 **kissfft** (`kiss_fftr` / `kiss_fft`), O(N²) DFT 미사용.

### 5.1 빈별 전처리

1. `atan2(imag, real)` 위상.
2. TI식 **위상 언래핑** (`MmwDemo_computePhaseUnwrap` 포팅) → Δψ.
3. 평균 제거 → Hao식 이동평균(window 5, 8-pass) → 변위 `Δx = λ/(4π)·Δψ` (λ≈3.89 mm @ 77 GHz).
4. 버퍼 최대 400 프레임 (~20 s @ 20 Hz).

### 5.2 분리·추정 (빈마다)

| 단계 | BR | HR |
|------|----|----|
| Bandpass FFT | 0.10–0.70 Hz | 0.80–3.33 Hz |
| 피크 탐색 | 동일 대역 | 0.80–2.50 Hz |
| Comb | — | 호흡 기본파·고조파 시간/스펙트럼 노치 |
| 추정기 | FFT top-k + autocorr + peak-count (가중 0.45/0.35/0.20) | pre-comb 3개 + post-comb 3개 (각 0.20/0.15/0.15) |

FFT 크기 2048. Warmup 약 7회 estimate는 수치 억제.

### 5.3 거리 빈 신뢰도 (선택 기준)

**질문: 거리 빈 신뢰도는 어떻게 측정하나?**

빈마다 독립 추정한 뒤 아래 점수가 가장 큰 빈의 HR/BR를 채택한다.

1. **대역 피크 우세도** `band_peak_dominance(spectrum, lo, hi)`  
   \[
   \mathrm{conf} = \frac{\max_{k \in [\mathrm{lo},\mathrm{hi})} P[k]}{\sum_{k \in [\mathrm{lo},\mathrm{hi})} P[k]}
   \]
   - BR conf: 호흡 밴드패스 후 파워 스펙트럼의 BR 대역  
   - HR conf: comb 후 HR 탐색 대역  
2. **결합 점수**  
   `score = 0.55 * br_conf + 0.45 * hr_conf`  
3. 추가 게이트: BR 6–42 rpm & `br_conf ≥ 0.12`, HR 48–150 bpm & `hr_conf ≥ 0.18` 일 때만 optional 값 채움.  
4. 게시용 `hrConfidence`는 `hr_conf * 0.8` (레이더 심박이 약함을 반영).

즉 신뢰도는 “대역 안 에너지가 한 피크에 얼마나 모였는가”이며, SNR·다중경로를 직접 추정하지는 않는다.

---

## 6. 수면 단계 휴리스틱

파일: `sleep_stage_synth.cpp`. SleepNet이 asleep인 분에 대해 light/deep/REM을 합성.

- lux ≥ 30 → awake  
- asleep 비율 낮음 → absent/awake  
- toss↑ → light  
- 그 외 **~90분 주기 가중**(초반 deep↑, 후반 rem↑) + BR 안정 시 deep 가산  

임상 PSG 동급이 아님. 개선·MESA/wav2sleep 로드맵은 `docs/ppt/sleep-stage-defense.md` 참고.

세션 경계: `sleep_session_fsm` (onset/기상/부재 임계).

---

## 7. 집계 · DB

| 단위 | 트리거 | 테이블/필드 |
|------|--------|-------------|
| 1초 | 프레임 집계 flush | (내부 → 분으로) |
| 1분 | 분 경계 | `sleep_stat` granularity=`1m` |
| 30분 | **세션 onset 이후** 경과 30분마다 (+ close 시 잔여) | `sleep_stat` `30m` |
| 세션 | FSM close | `sleep_session` |

시계 `:00/:30` 고정 창이 아니라 **세션 시작 시각 기준** 30분 창이다.

코골이: Wave Station mic opus 구독 → `SnoreAudioAnalyzer`. 없으면 toss/온도 fallback.

---

## 8. 리포트 · 인사이트 잡

`SleepManager` 전용 잡 큐 (`m_jobs`, 단일 `m_jobWorker`) — 세션/집계 워커와 분리.

| 잡 | 언제 | Agent | 후속 |
|----|------|-------|------|
| `Summary30m` | 30m persist 직후 | `POST /sleep/v1/summaries` | embed |
| `DailyReport` | 세션 close | `POST /sleep/v1/reports` period=daily | `generateAndPersistInsights(..., sleep_report, nightDate)` |
| `WeeklyReport` | 세션 close마다 | period=weekly, `periodStart = nightDate−6` | 동일 surface (날짜=periodStart 또는 nightDate 정책은 생성기) |

- 하루 여러 세션 → close마다 daily 잡 (같은 `night_date`면 DB upsert 정책에 따름).  
- 주간: 롤링 7일 `[nightDate−6, nightDate]`, 프론트 `getRollingWeekStart`와 동일.  
- 허브만 켜져 있고 세션이 안 닫히면 daily/weekly는 안 쌓임 (자정 cron 없음).

---

## 9. 프론트 / API

- 일·주 리포트: `GET /api/v1/sleep/...` (`sleep_store`).  
- 인사이트: `surface=sleep_report`, 기간 필터 + 최신 `date` 코호트, 카드 상한 `INSIGHT_CARD_LIMIT`(4).  
- 데모 데이터는 상당 부분 시나리오 합성 (`demo/sleep.md`).

---

## 10. 운영 시 주의

- IQ `enabled`·포트 미설정 시 vitals 조용히 비움.  
- 에이전트 다운 시 잡은 WARN 후 스킵 — 리포트 공백 가능.  
- 방당 런타임 1개 · 다중 유저 같은 방은 충돌 가능.  
- 프로덕션 vitals ≠ test-iq GUI의 모든 TI 히스토그램 융합(빈 선택은 **신뢰도 max**).

---

## 11. 주요 소스 맵

| 파일 | 역할 |
|------|------|
| `sleep_manager.cpp` | 틱, 장치 선택, 집계, 잡 |
| `sleep_vitals.*` | 타겟·칼만·IQ DSP |
| `sleep_pipeline` / `nn/*` | SleepNet |
| `sleep_session_fsm.*` | 세션 |
| `sleep_stage_synth.*` | light/deep/REM |
| `sleep_aggregator.*` | 초·분·30분 |
| `sleep_audio.*` | 코골이 |
| `sleep_store.cpp` | HTTP 조회 |
| `src/test/test-iq/vital_signs.py` | DSP 레퍼런스 |
