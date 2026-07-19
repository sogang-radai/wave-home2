# Power Analysis API

호출 방향: **백엔드(:8500) → 에이전트(:8502)** · Base URL: `/power/v1`


에이전트 서버가 제공하는 **전력 리포트 생성 API**다. 백엔드가 DB 에서 조회한 `metrics`·대상
`power_energy` 행(및 하위 구간)을 Body 로 넘기면, 에이전트는 자연어 요약(및 임베딩)을 생성해 반환한다.
추가 데이터가 필요하면 에이전트가 백엔드 DB 조회 API(`/internal/v1/db/query`)를 호출한다.
저장(`power_report.report_text`, `vec_power_report`)은 백엔드가 응답을 받은 뒤 수행한다.

호출 방향: **백엔드(:8500) → 에이전트(:8502)**

### 공통

- Base URL: `/power/v1` (에이전트 서버, `http://<agent>:8502/power/v1`)
- **SQLite 접근은 백엔드 전담**이다. 에이전트는 `dbUrl`·`energyId` 로 DB 를 열지 않는다.
- 백엔드는 생성 요청 시 `metrics`**(백엔드 계산) +** `target`**(대상 power_energy 행) +** `children`**(하위 구간,
옵션)** 을 Body 에 함께 실어 보낸다. 에이전트는 필요 없는 필드는 무시하고, 부족하면 DB 조회 API 로 보강한다.
- `embed`(기본 `true`)로 임베딩 동봉. **지표(metrics)는 백엔드가 계산**하고 에이전트는 텍스트/임베딩만 반환.
- 생성도 수면과 동일하게 **비동기 잡(job)** 이다. `POST /reports` 는 `202` + `jobId` 를 반환하고,
`GET /power/v1/jobs/{jobId}` 로 폴링해 결과를 받는다(SSE 미사용).
- **잡 운영 규칙**은 수면 API 와 동일하다. 중복 대상=`target.id`(동일 `energyId`).
- `model`·`embeddingModel`(옵션)도 수면 API 와 동일하다.
- `period` 종류: `1h` | `24h` | `1w` | `1mo`. `5m` 은 리포트 대상이 아니다.
- `deviceId = null` 이면 **계측 플러그 합산** 리포트, 특정 장치면 그 장치 리포트다.
- `1w`/`1mo` 리포트는 창 내 하위 `24h` 행을 `children` 에 담아 전달한다.
- 공통 에러 응답: `{ "error": { "code": "...", "message": "..." } }`

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "target.granularity 는 리포트 대상(1h/24h/1w/1mo)이어야 합니다.",
    "field": "target.granularity"
  }
}
```

**Response 409** — 동일 대상 잡 진행 중

```json
{
  "error": {
    "code": "JOB_ALREADY_RUNNING",
    "message": "동일 energyId(target.id)에 대한 job 이 이미 queued/running 상태입니다.",
    "jobId": "job_01J2ZSB6W9X3Y7Z2A3B4C5D6E7"
  }
}
```



### 타입

```ts
// db-schema.md power_energy 컬럼 (camelCase)
type PowerEnergyRow = {
  id: number; deviceId: number | null;   // null = 계측 플러그 합산
  granularity: '5m' | '1h' | '24h' | '1w' | '1mo';
  timeStart: string; energyWh: number; coverage: number; sampleCount: number;
};

type PowerReportRequest = {
  deviceId: number | null;
  period: '1h' | '24h' | '1w' | '1mo';
  periodStart: string;
  metrics: object;              // 백엔드가 계산한 구조화 지표
  target: PowerEnergyRow;       // 대상 power_energy 행(granularity = period)
  children?: PowerEnergyRow[]; // 하위 구간(24h→1h, 1w/1mo→24h). 옵션
  embed?: boolean;
  model?: string;
  embeddingModel?: string;
};

type PowerReportResponse = {
  energyId: number;             // target.id
  period: '1h' | '24h' | '1w' | '1mo';
  periodStart: string;
  deviceId: number | null;
  reportText: string;
  embedding: number[] | null;
  model: string;
  embeddingModel: string | null;
};

// 잡(job) 공통 (수면 API 와 동일)
type JobRef = { jobId: string; status: 'queued'; };
type JobStatus<T> = {
  jobId: string;
  status: 'queued' | 'running' | 'done' | 'failed';
  result?: T;
  error?: { code: string; message: string };
};
```

---



### POST `/reports`

전력 리포트를 생성한다. 백엔드가 계산한 `metrics` 와 대상 `target` 행(및 `children`)을 바탕으로
자연어 요약을 만든다.

**Request Body** — 24h 합산(`deviceId = null`)

```json
{
  "deviceId": null,
  "period": "24h",
  "periodStart": "2026-07-01",
  "metrics": {
    "energyWh": 3820.5, "energyKwh": 3.82, "peakW": 1180.4,
    "peakAt": "2026-07-01 22:05:00", "vsPrevPct": 12.3,
    "byDevice": [
      { "deviceId": 7714208883279181, "name": "거실 에어컨", "energyWh": 3120.0, "share": 0.82 }
    ]
  },
  "target": {
    "id": 20514, "deviceId": null, "granularity": "24h",
    "timeStart": "2026-07-01", "energyWh": 3820.5, "coverage": 0.98, "sampleCount": 288
  },
  "children": [
    {
      "id": 20401, "deviceId": null, "granularity": "1h",
      "timeStart": "2026-07-01 22:00:00", "energyWh": 1180.4, "coverage": 0.98, "sampleCount": 12
    }
  ],
  "embed": true,
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

**Response 202** — 잡 생성됨

```json
{ "jobId": "job_01J2ZSB6W9X3Y7Z2A3B4C5D6E7", "status": "queued" }
```

완료 시 `GET /power/v1/jobs/{jobId}` 의 `result` (PowerReportResponse, 24h 합산):

```json
{
  "energyId": 20514,
  "period": "24h",
  "periodStart": "2026-07-01",
  "deviceId": null,
  "reportText": "7월 1일 하루 전력 사용량은 3.82kWh 로 전일보다 12% 늘었습니다. 저녁 10시경 1.18kW 로 피크를 찍었고, 사용량의 82%가 거실 에어컨이었습니다. 낮 시간대에는 대기전력 위주로 거의 사용이 없었습니다.",
  "embedding": [0.0123, -0.0456, 0.0789],
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

완료 시 `result` (1mo 합산):

```json
{
  "energyId": 20988,
  "period": "1mo",
  "periodStart": "2026-06-03",
  "deviceId": null,
  "reportText": "최근 30일 총 전력 사용량은 약 115kWh, 일평균 3.8kWh 였습니다. 사용의 대부분(약 78%)이 에어컨이었고, 월 초 대기전력 위주에서 7월로 갈수록 냉방 사용이 크게 늘어난 추세가 뚜렷합니다.",
  "embedding": [0.0123, -0.0456, 0.0789],
  "model": "gemma4:12b-mlx",
  "embeddingModel": "nomic-embed-text"
}
```

---



### GET `/jobs/{jobId}`

`/reports` 로 생성한 잡의 상태와 결과를 조회한다. 완료(`done`)면 `result` 에 `PowerReportResponse`
가 담긴다. 수면 API 의 `/jobs` 와 동일한 규약·운영 규칙을 따른다.

**Response 200** — 완료

```json
{
  "jobId": "job_01J2ZSB6W9X3Y7Z2A3B4C5D6E7",
  "status": "done",
  "result": {
    "energyId": 20514,
    "period": "24h",
    "periodStart": "2026-07-01",
    "deviceId": null,
    "reportText": "7월 1일 하루 전력 사용량은 3.82kWh 로 ...",
    "embedding": [0.0123, -0.0456, 0.0789],
    "model": "gemma4:12b-mlx",
    "embeddingModel": "nomic-embed-text"
  }
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
POST /power/v1/reports
GET  /power/v1/jobs/{jobId}
```



### 백엔드 연동 지점

- `power_energy` 에 `1h`/`24h`/`1w`/`1mo` 행이 만들어진 뒤, DB 에서 `metrics`·`target`·`children` 을
조회해 `/reports` Body 를 구성하고 호출해 `jobId` 를 받는다. `/jobs/{jobId}` 폴링으로 완료를 기다린다.
- 완료되면 `result.reportText` 와 함께 `power_report` 에 upsert, `result.embedding` 을
`vec_power_report` 에 저장한다.
- 요금(cost)은 DB 에 저장하지 않으므로, 리포트 표시 시 백엔드가 `energy_wh` 로 요금표를 적용해 추정한다.

