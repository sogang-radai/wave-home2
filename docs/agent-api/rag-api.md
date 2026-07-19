# RAG 검색 API

호출 방향: **에이전트(:8502) → 백엔드 agent-api(:8501)** · **POST** `/internal/v1/rag/search`

RAG 는 **백엔드 전담**이다. 에이전트가 직접 벡터 검색·DB 접근을 하지 않고, 백엔드에 HTTP 로
검색을 위임한다. 백엔드가 쿼리를 임베딩(`/llm/v1/embeddings`)하고 `sqlite-vec` 로 유사 문서를 찾아
스니펫을 돌려준다.

### 공통

- 쿼리 임베딩은 **1회**만 수행하고, `targets[]` 의 각 컬렉션 검색에 재사용한다.
- 컬렉션마다 필터 축이 다르다: 수면 계열은 `userId`, 전력은 `deviceId`(null=합산).
- `userId`만 주어진 전력 RAG 요청은 백엔드가 `device_user_map`으로 해당 사용자 소유 계측 장치를 해석한다.
  집 전체 합산은 `deviceId: null`, 특정 장치는 해당 `device.id`를 target에 넣는다.
- `vec_sleep_stat` 은 30m 고정, `vec_sleep_report`/`vec_power_report` 는 `period` 로 구분한다.
- 시간창 필터는 원본 테이블(`sleep_stat.time_start`, `sleep_report.period_start` 등)과 조인해 수행한다.
- 요청 `targets[i]` 와 응답 `results[i]` 가 1:1 대응한다.
- `score`: **코사인 유사도** 0~1 (1에 가까울수록 유사). sqlite-vec 검색·정렬 기준과 동일.
- 사전 검색 시 백엔드는 `hits[]` 를 `{ collection, refId, text }[]` 로 평탄화해 Chat API 의
  `context.retrieved` 에 넣을 수 있다(`score`는 Chat 컨텍스트에는 보통 생략).

### POST `/internal/v1/rag/search`

**Request Body**

```json
{
  "query": "요즘 잠을 잘 못 자는 것 같아",
  "targets": [
    {
      "collection": "sleep_report",
      "userId": 1,
      "period": "daily",
      "from": "2026-06-25",
      "to": "2026-07-04",
      "topK": 3
    },
    {
      "collection": "sleep_stat",
      "userId": 1,
      "from": "2026-07-01 00:00:00",
      "to": "2026-07-04 12:00:00",
      "topK": 5
    },
    {
      "collection": "power_report",
      "deviceId": null,
      "period": "24h",
      "from": "2026-06-27",
      "to": "2026-07-04",
      "topK": 3
    }
  ]
}
```

**Response 200**

```json
{
  "results": [
    {
      "collection": "sleep_report",
      "hits": [
        {
          "refId": 812,
          "score": 0.83,
          "text": "7월 1일 밤 수면은 총 5시간 36분으로 목표보다 30분 부족했습니다. ..."
        }
      ]
    },
    {
      "collection": "sleep_stat",
      "hits": [
        {
          "refId": 4123,
          "score": 0.79,
          "text": "02:00~02:30 구간은 깊은 수면이 지배적이었고 ..."
        }
      ]
    },
    {
      "collection": "power_report",
      "hits": []
    }
  ]
}
```



### 컬렉션 목록

| collection | vec 테이블 | 필터 축 | 비고 |
|------------|-----------|---------|------|
| `sleep_stat` | `vec_sleep_stat` | `userId`, `from`/`to`(`timeStart`) | 30m 고정 |
| `sleep_report` | `vec_sleep_report` | `userId`, `period`, `from`/`to`(`periodStart`) | |
| `power_report` | `vec_power_report` | `deviceId`, `period`, `from`/`to` | |
| `posture_report` | `vec_posture_report` | `userId`, `period`, `from`/`to` | 신규 |
| `weekly_plan_report` | `vec_weekly_plan_report` | `userId`, `from`/`to`(`periodStart`) | 신규, metrics 없음 |
| `insight_dashboard` | `vec_insight_dashboard` | `userId`, `date`, `from`/`to` | `surface=dashboard_banner` |
| `insight_weekly_plan` | `vec_insight_weekly_plan` | `userId`, `date`, `from`/`to` | |
| `insight_sleep` | `vec_insight_sleep` | `userId`, `date`, `from`/`to` | |
| `insight_posture` | `vec_insight_posture` | `userId`, `date`, `from`/`to` | |
| `insight_power` | `vec_insight_power` | `userId`, `date`, `from`/`to` | |

`insight_*` 컬렉션은 DB `insight.surface` 와 `insight_*` 접두로 대응한다.
인사이트 hit 의 `text` 스니펫은 `title + "\n" + text` 조합.

요청 예시 (주간 계획 배너 생성 보강용)

```json
{
  "query": "이번 주 수면과 할 일 진행은?",
  "targets": [
    { "collection": "weekly_plan_report", "userId": 1, "from": "2026-06-01", "to": "2026-07-08", "topK": 2 },
    { "collection": "insight_weekly_plan", "userId": 1, "date": "2026-07-08", "topK": 3 },
    { "collection": "sleep_report", "userId": 1, "period": "weekly", "from": "2026-06-29", "to": "2026-07-08", "topK": 1 }
  ]
}
```



### 전체 엔드포인트 요약

```http
POST /internal/v1/rag/search
```



### 백엔드 연동 지점

- 에이전트 tool 또는 챗 턴 **사전 검색** 시 호출한다.
- 백엔드가 `/llm/v1/embeddings`(에이전트 포워딩)로 쿼리 임베딩 1회 생성 후 `sqlite-vec` 검색.
- `userId`만 있는 전력 target 은 `device_user_map`으로 계측 장치·합산(`deviceId: null`)을 해석한다.
- Chat 사전 검색(`context.retrieved`)을 쓸 때만, 백엔드가 `POST /internal/v1/rag/search` 후 평탄화해 `POST /chat/v1/turns` Body 에 실어 보낸다.



### LangGraph RAG 예시

```python
import httpx
from langchain_core.tools import tool

BACKEND = "http://127.0.0.1:8501/internal/v1"

@tool
def search_memory(query: str, user_id: int) -> list[dict]:
    """사용자의 수면/전력 리포트·요약에서 관련 내용을 검색한다."""
    r = httpx.post(
        f"{BACKEND}/rag/search",
        json={
            "query": query,
            "targets": [
                {"collection": "sleep_report", "userId": user_id, "period": "daily",
                 "from": "2026-06-25", "to": "2026-07-04", "topK": 3},
                {"collection": "sleep_stat", "userId": user_id,
                 "from": "2026-07-01 00:00:00", "to": "2026-07-04 12:00:00", "topK": 5},
            ],
        },
        timeout=5.0,
    )
    r.raise_for_status()
    return r.json()["results"]   # [{collection, hits:[{refId, score, text}]}, ...]

# 사용: LLM 이 필요할 때 search_memory tool → 백엔드 /rag/search (기본 챗 흐름)
# (옵션) 턴 시작 전 사전검색만 context.retrieved 로 주입하는 방식도 가능
llm_with_tools = llm.bind_tools([search_memory, control_device])
```

- 즉, 에이전트의 "현재 방식" RAG = 백엔드 `POST /internal/v1/rag/search` 를 tool/사전검색으로 호출한 뒤 결과 스니펫을
프롬프트에 넣어 답변을 생성한다. 벡터 인덱스·DB 접근은 전부 백엔드가 소유한다.

