이 문서는 SQLite3 데이터베이스의 스키마를 정의하는 명세입니다.

## 변경 내역

- **2026-07-07** — v1 API·프론트 mock 정렬
  - `routine_task.source_insight_id` 추가
  - `insight.label` 추가
  - `chat_history.title`, `chat_history.updated_at` 추가
  - `user_session`, 사용자 설정(`user_sleep_config`, `user_general_settings`, `user_ai_agent_settings`) 추가
  - 홈 자동화 `automation_rule`, `home_event` 추가

## 준수 사항

- id는 INTEGER 사용
- 타임스탬프는 VARCHAR(50) 사용, 포맷은 'YYYY-MM-DD HH:MM:SS'(ISO8601 에서 T 를 공백으로 대체)

## 목차

- 계정
- 세션
- 방
- 장치
- 사용자 설정
- 통계
    - 수면
    - 전력
- 제스처
- 홈 자동화
- 헬스 루틴/일정/TODO
- 알림
- 에이전트
    - 채팅
    - 인사이트

---------------------------------------------------------------------------------------------------


## 계정
### 스키마

```sql
CREATE TABLE user (
    id         INTEGER     NOT NULL,
    name       TEXT        NOT NULL,   -- 사용자 표시 이름
    created_at VARCHAR(50),            -- 생성 시각 'YYYY-MM-DD HH:MM:SS'

    PRIMARY KEY (id)
);
```

---------------------------------------------------------------------------------------------------

## 세션

활성 구성원과 인증 토큰. 단일 가구·단일 로그인을 가정한다.

### 스키마

```sql
CREATE TABLE user_session (
    id                INTEGER     PRIMARY KEY,
    active_user_id    INTEGER,                 -- nullable: 미선택
    access_token_hash TEXT        NOT NULL,    -- Bearer 토큰 해시
    created_at        VARCHAR(50) NOT NULL,
    updated_at        VARCHAR(50) NOT NULL,

    FOREIGN KEY (active_user_id) REFERENCES user(id)
);
```

---------------------------------------------------------------------------------------------------

## 방
### 스키마

```sql
CREATE TABLE room (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,    -- 방 이름
    description TEXT    NOT NULL,    -- 방 설명

    PRIMARY KEY (id)
);

CREATE TABLE room_user_map (
    room_id INTEGER NOT NULL,
    user_id INTEGER NOT NULL,

    PRIMARY KEY (room_id, user_id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
```

---------------------------------------------------------------------------------------------------

## 장치
- 장치 목록은 json으로 관리함, 테이블은 FK참조 목적으로만(자세한 세팅값은 json에 저장)
- 서버 기동시 json을 읽어서 db와 대조 후 없던 장치는 추가 => id를 다시 할당하고 json에 반영
- 만약 json에서 장치가 사라지면 db의 archived를 1로 => 다시 생기면 0으로
- 디바이스의 성격에 따라 사용되는 방이 여러개 일수도 있음(N:M)
- 디바이스를 여러명이 공유할 수도 있음(N:M)

### 지원 장치 목록
개발 대상
    - Wave Station
        - ESP32
        - 마이크(I2S INMP441)
        - 스피커
        - 조도센서
        - 온도센서
        - 습도센서
레이더
    - retina-r4sn

카메라
    - droid-cam
    - Reolink E1 Pro

스마트 플러그
    - 텐플 ep2-h

스마트 전구
    - 필립스 위즈 컬러 e29
    - 필립스 위즈 화이트 e29

TV
    - 삼성 32인치 스마트 TV - tizen os

### 스키마

```sql
CREATE TABLE device (
    id          INTEGER NOT NULL,
    name        TEXT    NOT NULL,   -- 장치 표시 이름
    description TEXT    NOT NULL,   -- 장치 설명
    class       TEXT    NOT NULL,   -- 장치 클래스(srs_r4sn, tuya_ep2h 등). json 과 동일
    archived    INTEGER NOT NULL,   -- 1 = json에서 사라짐(보관), 0 = 활성

    PRIMARY KEY (id)
);

CREATE TABLE device_user_map (
    device_id INTEGER NOT NULL,
    user_id   INTEGER NOT NULL,

    PRIMARY KEY (device_id, user_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE device_room_map (
    device_id INTEGER NOT NULL,
    room_id   INTEGER NOT NULL,

    PRIMARY KEY (device_id, room_id),
    FOREIGN KEY (device_id) REFERENCES device(id),
    FOREIGN KEY (room_id) REFERENCES room(id)
);
```

---------------------------------------------------------------------------------------------------

## 사용자 설정

구성원별 JSON 설정. 카탈로그(사운드·TTS 스피커·AI 모델 목록)는 정적 시드 파일로 두고 DB에는 저장하지 않는다.

### 스키마

```sql
CREATE TABLE user_sleep_config (
    user_id    INTEGER     PRIMARY KEY,
    config     TEXT        NOT NULL,    -- json: bedtime, wakeTime, acAuto, …
    updated_at VARCHAR(50) NOT NULL,

    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE user_general_settings (
    user_id    INTEGER     PRIMARY KEY,
    settings   TEXT        NOT NULL,    -- json: theme, language, notificationSound, ttsSpeakerId, …
    updated_at VARCHAR(50) NOT NULL,

    FOREIGN KEY (user_id) REFERENCES user(id)
);

CREATE TABLE user_ai_agent_settings (
    user_id           INTEGER     PRIMARY KEY,
    personal_prompt   TEXT        NOT NULL DEFAULT '',
    selected_model_id TEXT        NOT NULL,    -- 카탈로그 id (gemini-flash2.5 등)
    ctrl_enter_send   INTEGER     NOT NULL DEFAULT 0,
    wave_ai_sound     INTEGER     NOT NULL DEFAULT 1,
    updated_at        VARCHAR(50) NOT NULL,

    FOREIGN KEY (user_id) REFERENCES user(id)
);
```

`sleep_config` json 예시

```json
{
  "bedtime": "23:30",
  "wakeTime": "07:00",
  "wakeUpSound": "love-yourself",
  "acAuto": true,
  "acTemp": 24,
  "lightAuto": true,
  "dimStartMinutes": 30,
  "finalBrightness": 10,
  "wakeLightRamp": true,
  "wakeMusic": true,
  "wakeTvOrAlarm": false
}
```

---------------------------------------------------------------------------------------------------

## 통계
### 수면
- 프레임 원본은 저장하지 않고, 1분을 최소 영구 단위로 축약한다.
- granularity 로 해상도를 구분한다: 1m(최소 단위), 30m(에이전트 입력/RAG).
- 30m 행에는 환경 스냅샷과 자연어 요약을 함께 두어 RAG 에 사용한다.
- 세션(하룻밤)과 리포트(일/주)는 성격이 달라 별도 테이블로 둔다. 수면 시작과 끝은 에이전트가 판단한다.
- 장치는 세션 내에서 안 바뀌므로 세션에 저장한다(레이더=radar_id, Wave Station=station_id). 통계 행은 user_id 기준으로 유일하다.
- 레이더(SleepNet)가 직접 주는 출력은 status(absent/awake/asleep)와 뒤척임(toss)뿐이다. 이는 status_ratio / toss_* 에 담는다.
- 수면단계(light/deep/rem), 심박(HR)/호흡(BR), 코골이, 환경은 별도 센서(레이더 IQ, Wave Station 오디오 등) 몫이며 없으면 NULL 이다. 목업에서는 합성해 채운다.

스키마

```sql
CREATE TABLE sleep_session (
    id             INTEGER     PRIMARY KEY,
    user_id        INTEGER     NOT NULL,
    room_id        INTEGER     NOT NULL,
    radar_id       INTEGER     NOT NULL,    -- 레이더 장치 ID
    station_id     INTEGER,                 -- Wave Station 장치 ID(옵션)
    night_date     VARCHAR(50) NOT NULL,    -- 온셋 기준 날짜 'YYYY-MM-DD'
    onset          VARCHAR(50),             -- 첫 수면 시각 'YYYY-MM-DD HH:MM:SS'
    final_wake     VARCHAR(50),             -- 최종 기상 시각 'YYYY-MM-DD HH:MM:SS'
    time_in_bed_s  INTEGER,                 -- 재실 시간(초)
    asleep_total_s INTEGER,                 -- 총 수면 시간(초)
    efficiency     REAL,                    -- 수면 효율 = asleep_total_s / time_in_bed_s
    stage_totals   TEXT,                    -- json: 단계별 총 초
    toss_events    INTEGER,                 -- 뒤척임(움직임) 이벤트 수
    hr_mean        REAL,                    -- 평균 심박(bpm)
    br_mean        REAL,                    -- 평균 호흡(rpm)
    snore_ratio    REAL,                    -- 코골이 비율 0~1(수면 중 코골이 감지 시간 비율)

    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (station_id) REFERENCES device(id)
);

CREATE TABLE sleep_stat (
    id               INTEGER     PRIMARY KEY,
    user_id          INTEGER     NOT NULL,
    room_id          INTEGER     NOT NULL,
    session_id       INTEGER,                 -- 에이전트가 경계 판정 후 연결(주로 30m, 1m 은 NULL 가능)
    granularity      VARCHAR(3)  NOT NULL,    -- '1m' | '30m'
    time_start       VARCHAR(50) NOT NULL,    -- 구간 시작 'YYYY-MM-DD HH:MM:00'
    time_end         VARCHAR(50),             -- 구간 끝 'YYYY-MM-DD HH:MM:00'
    coverage         REAL        NOT NULL,    -- 실제 추론 출력 비율 0~1
    stage_label      VARCHAR(10),             -- 대표 수면단계(awake/light/deep/rem/absent)
    stage_ratio      TEXT,                    -- json: 단계별 시간 비율(합=1)
    stage_confidence REAL,                    -- 대표 단계 신뢰도 0~1
    status_ratio     TEXT,                    -- json: absent/awake/asleep 시간 비율(합=1, 레이더 status)
    toss_mean        REAL,                    -- 뒤척임 강도(toss_index 0~1) 평균
    toss_max         REAL,                    -- 뒤척임 강도 최대
    toss_p90         REAL,                    -- 뒤척임 강도 p90
    toss_events      INTEGER,                 -- 움직임 이벤트 수(index>=0.5 상승 교차)
    toss_ratio       TEXT,                    -- json: calm/slight/moderate 시간 비율(합=1)
    hr_mean          REAL,                    -- 평균 심박(bpm)
    hr_min           REAL,                    -- 최소 심박
    hr_max           REAL,                    -- 최대 심박
    hr_std           REAL,                    -- 심박 표준편차
    hr_confidence    REAL,                    -- 심박 신뢰도 0~1
    br_mean          REAL,                    -- 평균 호흡(rpm)
    br_min           REAL,                    -- 최소 호흡
    br_max           REAL,                    -- 최대 호흡
    br_std           REAL,                    -- 호흡 표준편차
    snore_ratio      REAL,                    -- 코골이 비율 0~1(구간 중 코골이가 감지된 시간 비율)
    env_temp         REAL,                    -- 온도(℃)
    env_lux          REAL,                    -- 조도(lux)
    env_noise        REAL,                    -- 소음(dB)
    summary_text     TEXT,                    -- 30m RAG용 자연어 요약

    CHECK (granularity IN ('1m', '30m')),
    UNIQUE (user_id, granularity, time_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (room_id) REFERENCES room(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
);

CREATE TABLE sleep_report (
    id           INTEGER     PRIMARY KEY,
    user_id      INTEGER     NOT NULL,
    period       VARCHAR(10) NOT NULL,    -- 'daily' | 'weekly'
    period_start VARCHAR(50) NOT NULL,    -- daily: 'YYYY-MM-DD', weekly: 주 시작일(월요일) 'YYYY-MM-DD'
    session_id   INTEGER,                 -- daily 한정
    metrics      TEXT,                    -- json: 구조화 지표
    report_text  TEXT,                    -- LLM 생성 리포트

    CHECK (period IN ('daily', 'weekly')),
    UNIQUE (user_id, period, period_start),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (session_id) REFERENCES sleep_session(id)
);
```

sleep_stat 비율 필드 json 예시 (30m 구간)

```json
{
    "stage_ratio":  { "light": 0.60, "deep": 0.20, "rem": 0.15, "awake": 0.05 },
    "status_ratio": { "asleep": 0.95, "awake": 0.05, "absent": 0.0 },
    "toss_ratio":   { "calm": 0.70, "slight": 0.27, "moderate": 0.03 }
}
```

- `status_ratio` 는 레이더가 실제로 주는 3-클래스(absent/awake/asleep) 비율이고, `stage_ratio` 는 그중 asleep 을 light/deep/rem 으로 세분한 (합성/미래) 값이다.
- 1m 행은 한 단계로 수렴하므로 각 비율이 사실상 {대표값: 1.0} 이다. 세분 통계는 30m 에서 의미가 있다.

RAG 임베딩 (sqlite-vec 확장 필요, nomic-embed-text 768차원)

```sql
CREATE VIRTUAL TABLE vec_sleep_stat USING vec0 (
    stat_id   INTEGER PRIMARY KEY,   -- sleep_stat.id (granularity='30m')
    embedding float[768]
);

CREATE VIRTUAL TABLE vec_sleep_report USING vec0 (
    report_id INTEGER PRIMARY KEY,   -- sleep_report.id
    embedding float[768]
);
```

### 전력

스마트 플러그의 전력을 받아 장치별 사용량(에너지)을 산출
값이 꺾이는 지점(breakpoint)만 남겨 파형을 저장하고, 정확한 에너지는 샘플에서 적분해 따로 저장

breakpoint 추출

- 선을 계속 이어가며 근사하다가, 새 값이 허용 오차를 꽤 길게 벗어날 때만 break 한다. 중간에 잠깐 튀는 값은 평탄화한다.
  1. 예외 밴드(노이즈/스파이크 제거): 새 값이 직전 기록값 근처면 무시한다. 단발 스파이크는 짧은 중앙값 필터(3~5샘플)로 제거한다. 중앙값 필터는 계단(진짜 변화)은 보존하고 튀는 값만 없앤다.
  2. 압축 밴드: 허용 오차 안에서 직선으로 이어지는 동안은 점을 버리고, 더 못 이으면 직전 점을 breakpoint 로 확정하고 새 구간을 시작한다.
- 고정 허용치(예: 5W)는 저전력 장치(3W)에서 안 잡힌다. 허용치를 값에 비례시켜 해결한다: 허용치 = max(절대값, 비율 x 현재전력). 예: max(0.3W, 3%).
- 대안: SlideFilter/SwingFilter(오차 보장 PLA), SWAB(노이즈에 강함). 실시간이면 위 SDT 조합을 기본으로 한다.

연결 해제 / 재연결

- 연결이 끊긴 구간은 값을 모르므로 절대 보간하지 않는다.
- 끊길 때 마지막 값으로 구간을 닫는 breakpoint(event=disconnect)를 남기고, 다시 붙으면 새 구간을 여는 breakpoint(event=reconnect)를 남긴다.
- 장치가 0W 를 보고하는 off 와, 데이터 자체가 없는 disconnect 는 다른 이벤트로 구분한다.

요금 계산

- 요금(cost)은 DB 에 저장하지 않는다. 백엔드가 요청 시점에 energy_wh 를 요금표(누진/시간대)에 대입해 추정한다.
- 요금표가 바뀌어도 저장된 에너지는 그대로이므로 재계산만 하면 된다. 요금표 스키마는 추후.
- 에너지(Wh) = 원시 샘플 적분(전력 x 시간의 합). 보간값이 아닌 실제 샘플만 사용한다. 5m -> 1h -> 24h 로 롤업해 저장한다.

전력 테이블은 단순 통계라 user_id 를 두지 않고 장치에 종속시킨다.
device_id = NULL 은 계측 플러그(스마트 플러그)들만 합산한 통계를 담는다. 계측이 불가능한 장치는 합산 대상이 아니다.

- SQLite 는 UNIQUE 에서 NULL 을 서로 다른 값으로 취급하므로, 합산 행 중복 방지는 COALESCE(device_id, -1) 유니크 인덱스로 처리한다.
- FK 는 device_id 가 NULL 이면 검사하지 않으므로, 합산용 더미 device 행을 따로 둘 필요가 없다.
장치와 사용자/방의 관계가 필요하면 별도 맵 테이블(device_user_map, device_room_map)로 조인한다.

스키마

```sql
CREATE TABLE power_energy (
    id           INTEGER     PRIMARY KEY,
    device_id    INTEGER,                 -- NULL = 전체 합산
    granularity  VARCHAR(3)  NOT NULL,    -- '5m' | '1h' | '24h' | '1w' | '1mo'
    time_start   VARCHAR(50) NOT NULL,    -- 5m: 'YYYY-MM-DD HH:MM:00', 1h: 'YYYY-MM-DD HH:00:00', 24h/1w/1mo: 'YYYY-MM-DD'(구간 첫날)
    energy_wh    REAL        NOT NULL,    -- 정확한 적분값(하위 granularity 를 합산)
    coverage     REAL        NOT NULL,    -- 존재하는 하위 구간의 데이터 완성도 0~1(off 로 생략된 구간은 미포함)
    sample_count INTEGER     NOT NULL,    -- 적분에 사용된 원시 샘플 수

    CHECK (granularity IN ('5m', '1h', '24h', '1w', '1mo')),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE UNIQUE INDEX uq_power_energy ON power_energy (COALESCE(device_id, -1), granularity, time_start);
```

리포트

- 수면과 동일하게 에너지 통계(power_energy)와 리포트를 성격이 달라 별도 테이블로 둔다.
- 1h / 24h / 1w / 1mo 구간을 대상으로 구조화 지표(metrics)와 LLM 자연어 요약(report_text)을 저장한다.
- report 의 period 는 항상 원본 power_energy 의 granularity 와 같고(period_start = time_start), energy_id 로 1:1 연결한다.
- report_text 는 LLM 으로 채운다. 요약 프롬프트는 추후 작성 예정이므로 지금은 NULL로 둔다.
- device_id = NULL 은 power_energy 와 동일하게 계측 플러그 합산 리포트를 뜻한다.
- 30m 수면과 마찬가지로 RAG 용 임베딩 테이블(vec_power_report)을 둔다.

집계 단위(granularity/period)

- 5m / 1h / 24h: 겹치지 않는 달력 버킷(하위를 그대로 합산).
- 1w: **슬라이딩 7일 창**. 매일 최근 7일을 집계하며, 데이터가 7일 미만이라 못 만드는 창은 건너뛴다. time_start = 창의 첫날.
- 1mo: **슬라이딩 30일 창**. 데이터가 30일이면 완전한 창은 하나뿐이다. time_start = 창의 첫날.
- 1w/1mo 는 24h 행을 창 단위로 합산해 만든다. 전주 대비(vs_prev)는 한 주기(7일/30일) 이전 창과 비교한다.

```sql
CREATE TABLE power_report (
    id           INTEGER     PRIMARY KEY,
    energy_id    INTEGER     NOT NULL,    -- 원본 power_energy 행 id(granularity=period, 같은 device_id/time_start)
    device_id    INTEGER,                 -- NULL = 계측 플러그 합산
    period       VARCHAR(10) NOT NULL,    -- '1h' | '24h' | '1w' | '1mo' | '1yr'
    period_start VARCHAR(50) NOT NULL,    -- 1h: 'YYYY-MM-DD HH:00:00', 24h/1w/1mo: 'YYYY-MM-DD'(구간 첫날) 
    metrics      TEXT,                    -- json: 구조화 지표(에너지/피크/전일대비 등)
    report_text  TEXT,                    -- LLM 생성 요약(추후 작성, 지금은 NULL)
    created_at   VARCHAR(50),             -- 생성 시각 'YYYY-MM-DD HH:MM:SS'

    CHECK (period IN ('1h', '24h', '1w', '1mo', '1yr')),
    FOREIGN KEY (energy_id) REFERENCES power_energy(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE UNIQUE INDEX uq_power_report ON power_report (COALESCE(device_id, -1), period, period_start);
```

metrics json 예시 (24h 합산 리포트, device_id = NULL)

```json
{
    "energy_wh": 3820.5,
    "energy_kwh": 3.82,
    "peak_w": 1180.4,
    "peak_at": "2026-07-01 22:05:00",
    "on_ratio": 0.34,
    "coverage": 0.98,
    "prev_energy_wh": 3401.2,
    "vs_prev_pct": 12.3,
    "by_device": [
        { "device_id": 7714208883279181, "name": "거실 에어컨", "energy_wh": 3120.0, "share": 0.82 },
        { "device_id": 5341068403714904, "name": "침실 컴퓨터", "energy_wh": 520.5, "share": 0.14 }
    ]
}
```

- 장치별 리포트(device_id 지정)는 단일 장치 지표만 담는다.
- 1h 리포트는 `peak_w`/`peak_at`/`energy_wh`/`coverage` 중심으로 축약하고 `by_device`/`vs_prev_pct`는 생략 가능하다.
- 1w / 1mo 리포트는 위 필드에 더해 창 정보(`days`: 창 안에서 데이터가 있는 날 수, `avg_daily_wh`: 일평균)를 담는다. `peak_w`/`peak_at`는 창 내 일별 피크 중 최대다.

RAG 임베딩 테이블

```sql
CREATE VIRTUAL TABLE vec_power_report USING vec0 (
    report_id INTEGER PRIMARY KEY,   -- power_report.id
    embedding float[768]
);
```

---------------------------------------------------------------------------------------------------

## 제스처
- 제스처 세트/클래스 정의는 json으로 관리함(gestures/gesture_sets.json + 각 set.json).
- 세트 테이블은 FK 참조용 id 만 둔다(장치와 동일한 방식). 로그가 어떤 세트에서 나왔는지 가리키는 용도.
- 세트 내 개별 제스처는 set.json 의 class_id 로 식별하며 별도 테이블을 두지 않는다.
- 로그는 json 이 바뀌어도 남아야 하므로 이름/동작을 스냅샷으로 저장한다.

스키마
```sql
CREATE TABLE gesture_set (
    id       INTEGER NOT NULL,
    name     TEXT    NOT NULL,   -- 세트 이름(json 과 동일, 표시/스냅샷용)
    archived INTEGER NOT NULL,   -- 1 = json에서 사라짐(보관), 0 = 활성

    PRIMARY KEY (id)
);

CREATE TABLE gesture_log (
    id             INTEGER     PRIMARY KEY,
    gesture_set_id INTEGER     NOT NULL,   -- 감지된 제스처가 속한 세트
    class_id       INTEGER     NOT NULL,   -- 세트 내 제스처 class_id(set.json)
    timestamp      VARCHAR(50) NOT NULL,   -- 감지 시각 'YYYY-MM-DD HH:MM:SS'
    gesture_name   VARCHAR(50) NOT NULL,   -- 제스처 이름 스냅샷(json 변경 대비)
    radar_id       INTEGER     NOT NULL,   -- 제스처를 감지한 레이더 장치
    device_id      INTEGER,                -- 제스처로 제어된 장치(대상 없으면 NULL)
    action         VARCHAR(100),           -- 실행된 동작 스냅샷
    confidence     REAL,                   -- 신뢰도 0~1

    FOREIGN KEY (gesture_set_id) REFERENCES gesture_set(id),
    FOREIGN KEY (radar_id) REFERENCES device(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE INDEX idx_gesture_log_occurred ON gesture_log (timestamp);
```

---------------------------------------------------------------------------------------------------

## 홈 자동화

트리거 룰과 이벤트 로그. 장치 마스터는 `device` + json 설정을 따른다.

### 트리거 룰

```sql
CREATE TABLE automation_rule (
    id            INTEGER      PRIMARY KEY,
    user_id       INTEGER      NOT NULL,
    name          VARCHAR(100) NOT NULL,
    enabled       INTEGER      NOT NULL DEFAULT 1,
    cooldown_ms   INTEGER      NOT NULL DEFAULT 0,
    trigger_json  TEXT,                           -- nullable: 디바이스 쿼리 트리거
    schedule_json TEXT,                           -- nullable: cron/시간 스케줄
    actions_json  TEXT         NOT NULL,          -- 실행 액션 배열
    created_at    VARCHAR(50)  NOT NULL,
    updated_at    VARCHAR(50)  NOT NULL,

    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_automation_rule_user ON automation_rule (user_id);
```

### 홈 이벤트 로그

`gesture_log`와 별도로 규칙 실행·스케줄 등 UI 이벤트 탭용 통합 로그.

```sql
CREATE TABLE home_event (
    id           INTEGER      PRIMARY KEY,
    user_id      INTEGER      NOT NULL,
    type         VARCHAR(20)  NOT NULL,          -- 'gesture' | 'execution' | 'schedule' | 'connection' | …
    occurred_at  VARCHAR(50)  NOT NULL,
    device_id    INTEGER,
    device_name  VARCHAR(100),
    message      VARCHAR(300) NOT NULL,
    triggered_by VARCHAR(100),                   -- rule:3, schedule:5, manual 등
    detail_json  TEXT,

    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (device_id) REFERENCES device(id)
);
CREATE INDEX idx_home_event_user_time ON home_event (user_id, occurred_at);
```

- 제스처 감지 원본은 `gesture_log`에 남기고, UI용 이벤트는 `home_event`에 복제·요약해 넣을 수 있다.

---------------------------------------------------------------------------------------------------

## 헬스 루틴/일정/TODO
- 프론트 헬스 루틴(주간 계획) 페이지의 요일별 할 일 목록에 대응한다.
- 하나의 할 일은 요일 + 카테고리 + (선택)시간대를 가진다. 시간대는 자정 기준 '분'으로 저장한다(프론트 startMin/endMin 과 동일).
- 에이전트가 인사이트 기반으로 할 일을 제안/추가할 수 있으므로 생성 주체(created_by)를 남긴다.

스키마
```sql
CREATE TABLE routine_task (
    id           INTEGER      PRIMARY KEY,
    user_id      INTEGER      NOT NULL,
    title        VARCHAR(100) NOT NULL,   -- 할 일 제목
    created_at   VARCHAR(50),             -- 생성 시각 'YYYY-MM-DD HH:MM:SS'
    created_by   VARCHAR(10)  NOT NULL,   -- 'user' | 'agent' 생성 주체
    category     VARCHAR(10)  NOT NULL,   -- 예: 'posture' | 'sleep' | 'diet' | 'mental' | ...
    day_of_week  VARCHAR(3)   NOT NULL,   -- 'mon' | 'tue' | 'wed' | 'thu' | 'fri' | 'sat' | 'sun'
    start_minute INTEGER,                 -- 자정 기준 분(0~1440). 시간 미지정이면 NULL
    end_minute   INTEGER,                 -- 자정 기준 분(0~1440)
    done         INTEGER      NOT NULL,   -- 0 = 미완료, 1 = 완료
    source_insight_id INTEGER,            -- 인사이트에서 파생 시 (nullable)

    CHECK (day_of_week IN ('mon', 'tue', 'wed', 'thu', 'fri', 'sat', 'sun')),
    CHECK (created_by IN ('user', 'agent')),
    CHECK ((start_minute IS NULL AND end_minute IS NULL)
           OR (start_minute >= 0 AND start_minute < end_minute AND end_minute <= 1440)),
    FOREIGN KEY (user_id) REFERENCES user(id),
    FOREIGN KEY (source_insight_id) REFERENCES insight(id)
);
CREATE INDEX idx_routine_task_user_day ON routine_task (user_id, day_of_week);
CREATE INDEX idx_routine_task_insight ON routine_task (source_insight_id);
```

---------------------------------------------------------------------------------------------------

## 알림
- 프론트 알림 패널에 표시되는 알림. 자동화/에이전트/센서 이벤트가 생성한다.
- type 은 늘어날 수 있어 CHECK 를 걸지 않는다(값은 주석으로 관리). 읽음 여부만 상태로 둔다.

스키마
```sql
CREATE TABLE notification (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    type       VARCHAR(20)  NOT NULL,   -- 'timer' | 'sleep' | 'posture' | 'temperature' | ...
    message    VARCHAR(200) NOT NULL,   -- 알림 내용
    read       INTEGER      NOT NULL,   -- 0 = 안읽음, 1 = 읽음
    created_at VARCHAR(50)  NOT NULL,   -- 생성 시각 'YYYY-MM-DD HH:MM:SS'

    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_notification_user_created ON notification (user_id, created_at);
```

---------------------------------------------------------------------------------------------------

## 에이전트
### 채팅
- 한 행 = 한 대화 세션. message 컬럼에 대화 전체(메시지 배열)를 json 으로 저장한다.
- gemma 를 ollama 로 돌리며 OpenAI chat protocol 을 사용한다. 아래는 그 포맷 예시.
- content 는 문자열 또는 멀티모달 파트 배열(text/image_url)이 될 수 있다.

대화 내용 json 예시 (OpenAI protocol)
```json
{
    "messages": [
        { "role": "system", "content": "너는 WaveHome 라이프스타일 에이전트야." },
        { "role": "user", "content": "어젯밤 수면 점수 알려줘" },
        { "role": "assistant", "content": "어젯밤 수면 점수는 82점이에요. 뒤척임이 03:05~03:40에 많았어요." },
        {
            "role": "user",
            "content": [
                { "type": "text", "text": "이 그래프 해석해줘" },
                { "type": "image_url", "image_url": { "url": "data:image/png;base64,..." } }
            ]
        }
    ]
}
```

스키마
```sql
CREATE TABLE chat_history (
    id         INTEGER      NOT NULL,
    user_id    INTEGER      NOT NULL,
    title      VARCHAR(100) NOT NULL DEFAULT '새 대화',
    created_at VARCHAR(50)  NOT NULL,   -- 생성 시각 'YYYY-MM-DD HH:MM:SS'
    updated_at VARCHAR(50)  NOT NULL,   -- 마지막 메시지 시각
    message    TEXT         NOT NULL,   -- 대화 내용 json(messages 배열)

    PRIMARY KEY (id),
    FOREIGN KEY (user_id) REFERENCES user(id)
);
```

### 인사이트
- 에이전트가 생성하는 권장 액션/제안. 프론트 인사이트 카드에 대응한다.
- 사용자가 승인하면(approved=1) 헬스 루틴(routine_task) 등으로 반영될 수 있다.

스키마
```sql
CREATE TABLE insight (
    id         INTEGER      PRIMARY KEY,
    user_id    INTEGER      NOT NULL,
    domain     VARCHAR(20)  NOT NULL,   -- 'sleep' | 'posture' | 'weekly-plan' | ...
    period     VARCHAR(10),             -- 'daily' | 'weekly' (해당 없으면 NULL)
    label      VARCHAR(50),             -- UI 그룹 (예: '오늘의 권장 액션', '다음 주 목표')
    title      VARCHAR(100) NOT NULL,   -- 인사이트 제목
    text       VARCHAR(300) NOT NULL,   -- 인사이트 본문(권장 액션 등)
    approved   INTEGER      NOT NULL,   -- 0 = 미승인, 1 = 사용자 승인
    created_at VARCHAR(50)  NOT NULL,   -- 생성 시각 'YYYY-MM-DD HH:MM:SS'

    FOREIGN KEY (user_id) REFERENCES user(id)
);
CREATE INDEX idx_insight_user_domain ON insight (user_id, domain);
```
