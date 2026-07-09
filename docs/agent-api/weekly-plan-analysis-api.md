# Weekly Plan Analysis API

호출 방향: **백엔드(:8500) → 에이전트(:8501)** · Base URL: `/weekly-plan/v1`

주간 계획 **상단 배너**(`weekly_plan_report`) narrative 생성.
에이전트가 `db/query`·`rag/search` 툴로 `schedule_task`·수면/자세 리포트 등을 **직접 조회**한다.
구조화 `metrics` 는 DB 에 저장하지 않는다.

### 공통

- Base URL: `/weekly-plan/v1`
- 비동기 잡: `POST /reports` → `202` + `jobId`, `GET /jobs/{jobId}` 폴링.
- 저장: 백엔드가 `weekly_plan_report` + `vec_weekly_plan_report` upsert.

### POST `/reports`

**Request Body**

```json
{
  "userId": 1,
  "periodStart": "2026-06-29",
  "embed": true
}
```

`periodStart` — 해당 주 월요일 `YYYY-MM-DD`.

에이전트는 Body 에 metrics 를 요구하지 않는다. 필요 데이터는 툴 호출로 수집.

**Response 202**

```json
{ "jobId": "job_wp_01J2Z...", "status": "queued" }
```

**완료 시 `result`**

```ts
type WeeklyPlanReportResult = {
  periodStart: string;
  headline: string | null;
  reportText: string;
  embedding?: number[];
};
```

프론트 매핑: `{ headline, body: reportText }`.

### GET `/jobs/{jobId}`

잡 상태·결과. [sleep-analysis-api.md](./sleep-analysis-api.md) 패턴 동일.

### 전체 엔드포인트 요약

```http
POST /weekly-plan/v1/reports
GET  /weekly-plan/v1/jobs/{jobId}
```
