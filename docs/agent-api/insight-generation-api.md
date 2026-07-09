# Insight Generation API

호출 방향: **백엔드(:8500) → 에이전트(:8501)** · Base URL: `/insight/v1`

백엔드가 리포트 생성·일일 갱신 시점에 에이전트에 인사이트 배치 생성을 **요청**한다.  
에이전트는 필요 시 `POST /internal/v1/db/query`·`POST /internal/v1/rag/search` 로 데이터를 보강하고, **생성 결과(인사이트 행 + 임베딩)를 반환한다.**

### 공통

- Base URL: `/insight/v1` (`http://<agent>:8501/insight/v1`)
- 비동기 잡 패턴은 [sleep-analysis-api.md](./sleep-analysis-api.md) 와 동일 (`POST` → `202` + `jobId`, `GET /jobs/{jobId}`).
- 동일 대상(`userId` + `surface` + `date`)에 `queued`/`running` 잡이 있으면 `409 JOB_ALREADY_RUNNING`.
- `embed`(기본 `true`): 각 인사이트에 `embedding` 포함. 백엔드가 `vec_insight_*` 에 upsert.



### POST `/insights`

특정 `surface`·`date` 의 인사이트를 생성(또는 당일 교체용 초안 반환).

**Request Body**

```json
{
  "userId": 1,
  "surface": "weekly_plan",
  "date": "2026-07-08",
  "context": {
    "reportPeriodStart": "2026-06-29"
  },
  "embed": true
}
```


| 필드        | 설명                                                                               |
| --------- | -------------------------------------------------------------------------------- |
| `surface` | `dashboard_banner` | `weekly_plan` | `sleep_report` | `posture_report` | `power` |
| `date`    | 발행일 `YYYY-MM-DD`                                                                 |
| `context` | surface 별 힌트(예: 수면 리포트 `periodStart`, 주간 계획 `reportPeriodStart`)                 |


에이전트는 `db/query`·`rag/search` 툴로 부족한 데이터를 스스로 조회한다.

**Response 202**

```json
{ "jobId": "job_insight_01J2Z...", "status": "queued" }
```

**완료 시** `result` — `items[]` 각 원소:

```ts
type GeneratedInsight = {
  surface: string;
  kind: 'banner' | 'action' | 'goal' | 'tip';
  date: string;
  label: string | null;
  title: string;
  text: string;
  actionable: boolean;
  actionType: 'schedule_task' | 'automation_rule' | 'reservation' | null;
  ruleJson: object | null;           // automation_rule | reservation
  scheduleTaskJson: object | null;   // schedule_task
  embedding?: number[];              // embed=true, 768차원
};
```

백엔드 저장 규칙:

- 당일 동일 `userId`+`surface`+`date` 기존 행은 **삭제 후 insert** (또는 upsert 정책은 백엔드 구현).
- `approved` 는 항상 `0` 으로 생성.



### GET `/jobs/{jobId}`

[sleep-analysis-api.md](./sleep-analysis-api.md) 와 동일 job 상태·결과 스키마.

### 전체 엔드포인트 요약

```http
POST /insight/v1/insights
GET  /insight/v1/jobs/{jobId}
```

