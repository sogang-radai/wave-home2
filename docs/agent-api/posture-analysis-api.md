# Posture Analysis API (경로 예약)

호출 방향: **백엔드(:8500) → 에이전트(:8501)** · Base URL: `/posture/v1`

자세 통계·리포트·인사이트 생성 API **경로만 예약**한다.
내부 메트릭(`posture_stat` 컬럼)이 확정되기 전까지 Request/Response 본문은 확정하지 않는다.

### 예약 엔드포인트

```http
POST /posture/v1/summaries      # (추후) 구간 요약
POST /posture/v1/reports        # (추후) daily | weekly 리포트
POST /posture/v1/insights       # (추후) surface=posture_report 인사이트 — 또는 insight/v1 로 통합
GET  /posture/v1/jobs/{jobId}
```

수면 API([sleep-analysis-api.md](./sleep-analysis-api.md))와 동일한 잡·저장 패턴을 따를 예정이다.

- 에이전트 → 백엔드: `db/query`(`posture_stat`, `posture_report`), `rag/search`(`posture_report`, `insight_posture`)
- 저장: `posture_report`, `insight`, `vec_*` — **백엔드**

### 현재 상태

- `posture_stat` 스키마: **초안** (`db-schema.md`)
- wave-server / 에이전트 구현: **미착수**
