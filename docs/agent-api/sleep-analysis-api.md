# Sleep Analysis API

호출 방향: **백엔드(:8500) → 에이전트(:8501)** · Base URL: `/sleep/v1`


에이전트 서버가 제공하는 **수면 분석·리포트 생성 API**다. 백엔드가 DB 에서 조회한 데이터를 Body 로
넘기면, 에이전트는 자연어 요약·리포트 본문(및 임베딩)을 생성해 반환한다. 추가 데이터가 필요하면
에이전트가 백엔드 DB 조회 API(`/internal/v1/db/query`)를 호출한다. 저장(`sleep_stat.summary_text`, `sleep_report`,
`vec_*`)은 백엔드가 응답을 받은 뒤 수행한다.

호출 방향: **백엔드(:8500) → 에이전트(:8501)**

### 공통

- Base URL: `/sleep/v1` (에이전트 서버, `http://<agent>:8501/sleep/v1`)
- 백엔드가 요청 시에만 에이전트를 호출한다. 에이전트는 자체 스케줄로 분석하지 않는다.
- **SQLite 접근은 백엔드 전담**이다. 에이전트는 `dbUrl`·row id 로 DB 를 열지 않는다.
- 백엔드는 생성 요청 시 **인라인 데이터 + metrics**(리포트)를 Body 에 함께 실어 보낸다. 에이전트는
필요 없는 필드는 무시하고, 부족하면 DB 조회 API 로 보강한다.
- 요약(`/summaries`): 대상 **30m** `sleep_stat` **윈도우 1개**(`window`)를 인라인으로 전달한다.
- 리포트(`/reports`): 해당 기간의 `metrics`**(백엔드 계산) +** `sessions` **+** `stats30m` 를 인라인으로 전달한다.
daily 도 하루에 여러 세션이 있으면 `sessions` 배열에 모두 담는다.
- 리포트 종류는 경로가 아니라 Body 의 `period` 로 구분한다.
- `embed`(기본 `true`): 응답에 임베딩 벡터를 함께 생성해 담는다. 백엔드는 이를 그대로 `vec_*` 에
넣으면 되고, 별도 임베딩 호출이 필요 없다. `false` 면 텍스트만 반환한다.
  - 임베딩 모델/차원은 스키마의 `vec_*`(nomic-embed-text, 768)과 일치한다.
- `model`(옵션): 생성에 쓸 모델 이름. `embeddingModel`(옵션): 임베딩에 쓸 모델 이름. 둘 다 생략 가능.
  - 에이전트는 요청된 모델을 우선 고려하되, 상황(부하/컨텍스트 길이 등)에 따라 유연하게 다른 모델을
  선택할 수 있다. 실제 사용한 모델 이름은 응답의 `model`·`embeddingModel` 로 돌려준다.
  - `embeddingModel` 을 바꿔 임베딩 차원이 달라지면 `vec_*` 스키마와 어긋날 수 있으니 백엔드가 확인한다.
- **지표(metrics) 계산은 백엔드가 수행**한다. 에이전트는 metrics 와 인라인 통계를 바탕으로 **자연어
텍스트와 임베딩만** 반환한다(`sleep_report.metrics` 저장은 백엔드 몫).
- 요약/리포트 생성은 실시간이 아니므로 **비동기 잡(job)** 으로 처리한다. `POST` 는 즉시 `202` + `jobId`
를 반환하고, `GET /sleep/v1/jobs/{jobId}` 로 폴링해 완료 시 결과를 받는다(SSE 미사용).
- **잡 운영 규칙**(전력 API 동일):
  - **중복 요청**: 동일 대상(요약=`window.id`, 리포트=`userId`+`period`+`periodStart`)에 `queued`/`running`
    job 이 있으면 `POST` 는 **409** `JOB_ALREADY_RUNNING`.
  - **jobId 보존**: `done`/`failed` 후 **24시간** 동안 `GET /jobs/{jobId}` 조회 가능. 이후 `404`.
  - **폴링**: 1~3초 간격 시작, 30초 경과 후 5~10초 백오프.
  - **재시도**: `failed` job 은 재개하지 않는다. 새 `POST` 로 잡을 다시 만든다.
  - **POST vs job 실패**: 입력 검증(빈 `sessions`, 잘못된 `window` 등)은 **POST 400**. LLM 생성 실패·타임아웃은
    job `failed`(`GENERATION_FAILED`, `GENERATION_TIMEOUT`).
- 날짜·시각 포맷은 DB 스키마와 동일: `YYYY-MM-DD`, `YYYY-MM-DD HH:MM:SS`.
- 공통 에러 응답: `{ "error": { "code": "...", "message": "..." } }`

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "period는 daily 또는 weekly 여야 합니다.",
    "field": "period"
  }
}
```

**Response 400** — 윈도우 검증 실패

```json
{
  "error": {
    "code": "INVALID_WINDOW",
    "message": "window.granularity 는 30m 이어야 합니다.",
    "field": "window.granularity"
  }
}
```

**Response 400** — 리포트 데이터 부족

```json
{
  "error": {
    "code": "NO_SLEEP_DATA",
    "message": "sessions 가 비어 있습니다. 해당 기간 수면 데이터를 먼저 조회해 Body 를 구성하세요.",
    "field": "sessions"
  }
}
```

**Response 409** — 동일 대상 잡 진행 중

```json
{
  "error": {
    "code": "JOB_ALREADY_RUNNING",
    "message": "동일 대상에 대한 job 이 이미 queued/running 상태입니다.",
    "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E"
  }
}
```

**Response 502** — 잡 큐 등록 실패

```json
{
  "error": {
    "code": "GENERATION_FAILED",
    "message": "job 을 큐에 등록하지 못했습니다."
  }
}
```



### 타입

```ts
// db-schema.md sleep_stat / sleep_session 컬럼 (camelCase)
type SleepStatRow = {
  id: number; userId: number; roomId: number; sessionId: number | null;
  granularity: '1m' | '30m'; timeStart: string; timeEnd: string | null;
  coverage: number; stageLabel: string | null; stageRatio: object | null;
  stageConfidence: number | null; statusRatio: object | null;
  tossMean: number | null; tossMax: number | null; tossP90: number | null;
  tossEvents: number | null; tossRatio: object | null;
  hrMean: number | null; hrMin: number | null; hrMax: number | null; hrStd: number | null;
  brMean: number | null; brMin: number | null; brMax: number | null; brStd: number | null;
  snoreRatio: number | null; envTemp: number | null; envLux: number | null; envNoise: number | null;
};

type SleepSessionRow = {
  id: number; userId: number; roomId: number; radarId: number; stationId: number | null;
  nightDate: string; onset: string | null; finalWake: string | null;
  timeInBedS: number | null; asleepTotalS: number | null; efficiency: number | null;
  stageTotals: object | null; tossEvents: number | null;
  hrMean: number | null; brMean: number | null; snoreRatio: number | null;
};

type SummaryRequest = {
  window: SleepStatRow;       // granularity='30m' 대상 윈도우
  minutes?: SleepStatRow[];   // 옵션: 같은 구간 1m 행들
  embed?: boolean;            // 기본 true
  model?: string;
  embeddingModel?: string;
};

type SummaryResponse = {
  statId: number;             // window.id
  summaryText: string;        // sleep_stat.summary_text 에 저장
  embedding: number[] | null;
  model: string;
  embeddingModel: string | null;
};

type ReportRequest = {
  userId: number;
  period: 'daily' | 'weekly';
  periodStart: string;        // daily: date, weekly: weekStart
  metrics: object;              // 백엔드가 계산한 구조화 지표
  sessions: SleepSessionRow[];  // 해당 기간 세션( daily 도 여러 개 가능)
  stats30m: SleepStatRow[];     // 해당 기간 30m 통계
  embed?: boolean;
  model?: string;
  embeddingModel?: string;
};

type ReportResponse = {
  period: 'daily' | 'weekly';
  periodStart: string;
  reportText: string;
  embedding: number[] | null;
  model: string;
  embeddingModel: string | null;
};

// 잡(job) 공통
type JobRef = {
  jobId: string;
  status: 'queued';
};

type JobStatus<T> = {
  jobId: string;
  status: 'queued' | 'running' | 'done' | 'failed';
  result?: T;
  error?: { code: string; message: string };
};
```

---



### POST `/summaries`

30분(`sleep_stat.granularity='30m'`) 구간의 자연어 요약을 생성한다. RAG·에이전트 입력용
`summary_text`(및 `vec_sleep_stat` 임베딩)를 만들 때 사용한다.

백엔드는 대상 30m 윈도우를 `window` 로 인라인 전달한다. 더 정밀한 서술이 필요하면 `minutes`(1m 행)를
함께 실을 수 있다.

**Request Body**

```json
{
  "window": {
    "id": 4123, "userId": 1, "roomId": 1, "sessionId": 88,
    "granularity": "30m", "timeStart": "2026-07-01 02:00:00", "timeEnd": "2026-07-01 02:30:00",
    "coverage": 0.98, "stageLabel": "deep",
    "stageRatio": { "deep": 0.62, "light": 0.30, "rem": 0.08 },
    "statusRatio": { "asleep": 0.95, "awake": 0.05, "absent": 0.0 },
    "tossMean": 0.12, "tossEvents": 2, "hrMean": 58.1, "brMean": 14.2,
    "snoreRatio": 0.03, "envTemp": 24.2, "envLux": 0.0, "envNoise": 32.1
  },
  "embed": true,
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

**Response 202** — 잡 생성됨

```json
{ "jobId": "job_01J2ZS5K8M4P7R2X9A0B1C2D3E", "status": "queued" }
```

완료 시 `GET /sleep/v1/jobs/{jobId}` 의 `result` (SummaryResponse):

```json
{
  "statId": 4123,
  "summaryText": "02:00~02:30 구간은 깊은 수면이 지배적이었고 심박은 58bpm 전후로 안정적이었습니다. 코골이는 거의 감지되지 않았고 실내 온도는 24.2℃를 유지했습니다.",
  "embedding": [0.0123, -0.0456, 0.0789],
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

---



### POST `/reports`

일일/주간 리포트를 생성한다. `period`·`periodStart` 로 종류와 기간을 지정하고, 백엔드가 계산한
`metrics` 와 해당 기간의 `sessions`·`stats30m` 을 인라인으로 전달한다.
`sleep_report`(및 `vec_sleep_report`)에 저장할 `reportText`·`embedding` 을 반환한다.

- `period="daily"` — `periodStart`=night_date. 그날 `sleep_session`·30m 통계를 `sessions`/`stats30m` 에 담는다.
- `period="weekly"` — `periodStart`=weekStart(월요일). 그 주 7일치를 `sessions`/`stats30m` 에 담는다.
- 추가 세부가 필요하면 에이전트가 DB 조회 API 로 보강한다.

**Request Body** — daily

```json
{
  "userId": 1,
  "period": "daily",
  "periodStart": "2026-07-01",
  "metrics": {
    "asleepTotalS": 20160, "timeInBedS": 26880, "efficiency": 0.75,
    "latencyS": 2100, "tossEvents": 18, "snoreRatio": 0.12
  },
  "sessions": [
    {
      "id": 88, "userId": 1, "roomId": 1, "radarId": 7714208883279181, "stationId": null,
      "nightDate": "2026-07-01", "onset": "2026-07-01 00:35:00", "finalWake": "2026-07-01 07:55:00",
      "timeInBedS": 26880, "asleepTotalS": 20160, "efficiency": 0.75,
      "tossEvents": 18, "hrMean": 59.2, "brMean": 14.5, "snoreRatio": 0.12
    }
  ],
  "stats30m": [
    {
      "id": 4120, "userId": 1, "roomId": 1, "sessionId": 88,
      "granularity": "30m", "timeStart": "2026-07-01 03:00:00", "timeEnd": "2026-07-01 03:30:00",
      "coverage": 0.97, "stageLabel": "light", "tossMean": 0.28, "tossEvents": 6,
      "hrMean": 62.0, "snoreRatio": 0.18, "envTemp": 24.5
    }
  ],
  "embed": true
}
```

**Response 202** — 잡 생성됨

```json
{ "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E", "status": "queued" }
```

완료 시 `GET /sleep/v1/jobs/{jobId}` 의 `result` (ReportResponse, daily):

```json
{
  "period": "daily",
  "periodStart": "2026-07-01",
  "reportText": "7월 1일 밤 수면은 총 5시간 36분으로 목표보다 30분 부족했습니다. 입면까지 35분이 걸렸고 새벽 3시 이후 뒤척임이 늘었습니다. 코골이 비율은 전일보다 소폭 증가했으나 수면 효율은 75%로 양호한 편입니다.",
  "embedding": [0.0123, -0.0456, 0.0789],
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

**Request Body** — weekly

> 아래 `sessions`/`stats30m` 빈 배열은 축약 예시다. 실제 호출 시 백엔드가 DB 에서 해당 주 7일치
> 세션·30m 통계를 조회해 채운다.

```json
{
  "userId": 1,
  "period": "weekly",
  "periodStart": "2026-06-29",
  "metrics": {
    "avgAsleepS": 20400, "avgEfficiency": 0.73, "bedtimeDriftMin": 30
  },
  "sessions": [],
  "stats30m": [],
  "embed": true
}
```

**Response 202** — 잡 생성됨

```json
{ "jobId": "job_01J2ZS9P4S8T2V6X3A4B5C6D7E", "status": "queued" }
```

완료 시 `GET /sleep/v1/jobs/{jobId}` 의 `result` (ReportResponse, weekly):

```json
{
  "period": "weekly",
  "periodStart": "2026-06-29",
  "reportText": "6월 29일~7월 5일 주간 평균 수면은 5시간 40분 수준이었습니다. 주 초반보다 후반으로 갈수록 입면 시간이 짧아지고 깊은 수면 비율이 개선되었습니다. 주말 취침 시각이 30분 늦어진 패턴이 보입니다.",
  "embedding": [0.0123, -0.0456, 0.0789],
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

**Response 400**

```json
{
  "error": {
    "code": "INVALID_WEEK_START",
    "message": "weekStart는 해당 주의 월요일 날짜여야 합니다.",
    "field": "periodStart"
  }
}
```

---



### GET `/jobs/{jobId}`

`/summaries`·`/reports` 로 생성한 잡의 상태와 결과를 조회한다. 완료(`done`)면 `result` 에
해당 응답 타입(`SummaryResponse` 또는 `ReportResponse`)이 담긴다.

**Response 200** — 진행 중

```json
{ "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E", "status": "running" }
```

**Response 200** — 완료

```json
{
  "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E",
  "status": "done",
  "result": {
    "period": "daily",
    "periodStart": "2026-07-01",
    "reportText": "7월 1일 밤 수면은 총 5시간 36분으로 목표보다 30분 부족했습니다. ...",
    "embedding": [0.0123, -0.0456, 0.0789],
    "model": "gemma4:12b-mlx",
    "embeddingModel": "nomic-embed-text"
  }
}
```

**Response 200** — 실패 (생성 타임아웃)

```json
{
  "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E",
  "status": "failed",
  "error": { "code": "GENERATION_TIMEOUT", "message": "수면 분석 생성 시간이 초과되었습니다." }
}
```

**Response 200** — 실패 (생성 오류)

```json
{
  "jobId": "job_01J2ZS7N2Q6R9T4X1A2B3C4D5E",
  "status": "failed",
  "error": { "code": "GENERATION_FAILED", "message": "수면 리포트를 생성하지 못했습니다." }
}
```

**Response 404**

```json
{
  "error": {
    "code": "JOB_NOT_FOUND",
    "message": "jobId 에 해당하는 작업이 없습니다."
  }
}
```

---



### 전체 엔드포인트 요약

```http
POST /sleep/v1/summaries
POST /sleep/v1/reports
GET  /sleep/v1/jobs/{jobId}
```



### 백엔드 연동 지점

- 30m `sleep_stat` 행이 생긴 뒤(또는 백필 시) 해당 윈도우를 `window` 로 `/summaries` 에 실어 호출해
`jobId` 를 받고, `/jobs/{jobId}` 폴링으로 완료되면 `result.summaryText` 를 `sleep_stat.summary_text` 에,
`result.embedding` 을 `vec_sleep_stat` 에 저장한다.
- 프론트의 `GET /api/v1/sleep/reports/daily`·`weekly` 요청 전, 캐시가 없으면 DB 에서 `metrics`·`sessions`·
`stats30m` 을 조회해 `/reports` Body 를 구성하고 잡을 만든다. 완료 후 `result.reportText` 와 함께
`sleep_report` 에 upsert, `result.embedding` 을 `vec_sleep_report` 에 저장한다.
- 리포트는 즉시 필요하지 않으므로, 프론트에는 "생성 중" 을 먼저 응답하고 완료되면 캐시로 제공해도 된다.
- `embed=true` 로 임베딩을 함께 받으므로 별도 `/llm/v1/embeddings` 호출은 필요 없다(원하면 `false`).
- 인사이트 생성은 [insight-generation-api.md](../insight-generation-api.md) (`POST /insight/v1/insights`) 로 분리. 일일/주간 리포트 job 과 별도.

