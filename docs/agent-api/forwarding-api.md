# LLM 모델 포워딩 API

호출 방향: **백엔드(:8500) → 에이전트(:8502)** · Base URL: `/llm/v1`


에이전트 서버가 제공하는 **LLM/임베딩 서비스 API**다. 실제 모델 제공자(Gemini API, Ollama 등)와의 연결은 에이전트 서버가 소유하고, 백엔드는 이 API를 호출해 LLM을 사용한다.

호출 방향: **백엔드(:8500) → 에이전트(:8502)**

### 공통

- Base URL: `/llm/v1` (에이전트 서버, `http://<agent>:8502/llm/v1`)
- OpenAI Chat Completions / Embeddings 호환 스키마를 따른다.
- 백엔드는 OpenAI SDK 등 표준 클라이언트의 `base_url`을 `http://<agent>:8502/llm/v1`로 두고  
사용할 수 있다.
- 공통 에러 응답: `{ "error": { "code": "...", "message": "..." } }`
- 채팅 히스토리는 이 API로 저장하지 않는다. 대화 기록은 백엔드가 `chat_history`에 관리한다.

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "요청 본문이 올바르지 않습니다.",
    "field": "messages"
  }
}
```

**Response 404**

```json
{
  "error": {
    "code": "MODEL_NOT_FOUND",
    "message": "지원하지 않는 model 입니다. /models 로 사용 가능한 모델을 조회하세요."
  }
}
```

**Response 502**

```json
{
  "error": {
    "code": "LLM_PROVIDER_ERROR",
    "message": "LLM 제공자 응답을 처리하지 못했습니다."
  }
}
```

**Response 504**

```json
{
  "error": {
    "code": "LLM_TIMEOUT",
    "message": "LLM 응답 생성 시간이 초과되었습니다."
  }
}
```



### 타입

```ts
type LlmModel = {
  id: string;           // 실제 모델 이름 (예: 'gemma4:12b-mlx')
  object: 'model';
  role: 'chat' | 'embedding';
  provider: string;     // 제공/호스팅 출처 라벨. 예: 'ollama' | 'openai' | 'gemini'. 추후 문자열 추가 가능
  dimension?: number;   // embedding 만
};

type ChatMessage = {
  role: 'system' | 'user' | 'assistant';
  content: string;
};

type ChatCompletionRequest = {
  model: string;   // 실제 모델 이름
  messages: ChatMessage[];
  temperature?: number;
  top_p?: number;
  max_tokens?: number;
  stop?: string | string[];
  seed?: number;
  frequency_penalty?: number;
  presence_penalty?: number;
  stream?: boolean;
};

type ChatCompletionChoice = {
  index: number;
  message: {
    role: 'assistant';
    content: string;
    reasoning?: string;   // thinking 모델일 때 추론 구간
  };
  finish_reason: 'stop' | 'length' | null;
};

type ChatCompletionResponse = {
  id: string;
  object: 'chat.completion';
  model: string;   // 실제 모델 이름
  choices: ChatCompletionChoice[];
  usage: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
};

type ChatCompletionChunk = {
  id: string;
  object: 'chat.completion.chunk';
  model: string;   // 실제 모델 이름
  choices: Array<{
    index: number;
    delta: {
      role?: 'assistant';
      content?: string;
      reasoning?: string;   // thinking 모델의 추론 구간
    };
    finish_reason: 'stop' | 'length' | null;
  }>;
};

type EmbeddingRequest = {
  model: string;   // 실제 임베딩 모델 이름
  input: string | string[];
};

type EmbeddingResponse = {
  object: 'list';
  model: string;   // 실제 임베딩 모델 이름
  data: Array<{
    index: number;
    embedding: number[];
  }>;
  usage: {
    prompt_tokens: number;
    total_tokens: number;
  };
};
```

---



### GET `/models`

에이전트 서버가 제공하는 실제 모델 목록을 반환한다. 백엔드 기동 시 또는 모델 선택 전에 조회한다.
`provider` 는 해당 모델이 **어느 백엔드/런타임에서 서빙되는지**를 나타내는 자유 텍스트 라벨이다
(고정 enum 아님). 예: `ollama`, `openai`, `gemini`.

**Response 200**

```json
{
  "object": "list",
  "data": [
    {
      "id": "gemma4:12b-mlx",
      "object": "model",
      "role": "chat",
      "provider": "ollama"
    },
    {
      "id": "gemma2:2b",
      "object": "model",
      "role": "chat",
      "provider": "ollama"
    },
    {
      "id": "nomic-embed-text",
      "object": "model",
      "role": "embedding",
      "provider": "ollama",
      "dimension": 768
    }
  ]
}
```

---



### POST `/chat/completions`

채팅/생성 요청. `stream=false`이면 단일 JSON, `stream=true`이면 **SSE**로 토큰을 스트리밍한다.

**Request Body** — 비스트리밍

```json
{
  "model": "gemma4:12b-mlx",
  "messages": [
    { "role": "system", "content": "You are a smart home health assistant." },
    { "role": "user", "content": "어젯밤 수면을 3문장으로 요약해줘." }
  ],
  "temperature": 0.7,
  "max_tokens": 512,
  "stream": false
}
```

**Response 200**

```json
{
  "id": "chatcmpl_01J2ZQ8M6R9P4T7X3A5B2C1D0E",
  "object": "chat.completion",
  "model": "gemma4:12b-mlx",
  "choices": [
    {
      "index": 0,
      "message": {
        "role": "assistant",
        "content": "어젯밤 총 수면은 6시간 20분이었고, 입면까지 35분이 걸렸습니다. 새벽 3시경 뒤척임이 늘었습니다.",
        "reasoning": null
      },
      "finish_reason": "stop"
    }
  ],
  "usage": {
    "prompt_tokens": 42,
    "completion_tokens": 68,
    "total_tokens": 110
  }
}
```

**Request Body** — 스트리밍

```json
{
  "model": "gemma2:2b",
  "messages": [
    { "role": "user", "content": "오늘 전력 사용 패턴을 한 줄로 말해줘." }
  ],
  "stream": true
}
```

**Response 200 (SSE)**

헤더:

```http
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
```

본문 예시:

```text
data: {"id":"chatcmpl_01J2ZQ91BN4R8E5Y6W3T0P7M2C","object":"chat.completion.chunk","model":"gemma2:2b","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}

data: {"id":"chatcmpl_01J2ZQ91BN4R8E5Y6W3T0P7M2C","object":"chat.completion.chunk","model":"gemma2:2b","choices":[{"index":0,"delta":{"reasoning":"사용자가 전력 패턴을 물었다."},"finish_reason":null}]}

data: {"id":"chatcmpl_01J2ZQ91BN4R8E5Y6W3T0P7M2C","object":"chat.completion.chunk","model":"gemma2:2b","choices":[{"index":0,"delta":{"content":"저녁"},"finish_reason":null}]}

data: {"id":"chatcmpl_01J2ZQ91BN4R8E5Y6W3T0P7M2C","object":"chat.completion.chunk","model":"gemma2:2b","choices":[{"index":0,"delta":{"content":" 시간대에"},"finish_reason":null}]}

data: {"id":"chatcmpl_01J2ZQ91BN4R8E5Y6W3T0P7M2C","object":"chat.completion.chunk","model":"gemma2:2b","choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}

data: [DONE]
```

스트리밍 규칙:

- 각 이벤트는 `data: <json>\n\n` 형식이다.
- `choices[0].delta.content`를 누적하면 최종 답변 텍스트가 된다.
- thinking 모델은 `choices[0].delta.reasoning`으로 추론 구간을 별도 전송한다.
- 종료는 반드시 `data: [DONE]\n\n`으로 마무리한다.
- 스트림 시작 **전** 오류는 일반 HTTP 4xx/5xx + JSON 에러 본문으로 반환한다.
- 스트림 시작 **후** 오류는 `data: {"error":{"code":"LLM_PROVIDER_ERROR","message":"..."}}\n\n`을
보낸 뒤 연결을 닫는다.
- 백엔드(클라이언트)가 연결을 끊으면 에이전트는 상위 LLM 생성 요청을 즉시 취소한다.

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "messages 는 최소 1개 이상이어야 합니다.",
    "field": "messages"
  }
}
```

---



### POST `/embeddings`

텍스트 임베딩을 생성한다. RAG 인덱싱·검색에 사용한다. 스트리밍은 지원하지 않는다.

**Request Body** — 단일 입력

```json
{
  "model": "nomic-embed-text",
  "input": "어젯밤 수면 효율이 78%였고 새벽에 뒤척임이 늘었다."
}
```

**Request Body** — 배치 입력

```json
{
  "model": "nomic-embed-text",
  "input": [
    "어젯밤 수면 효율이 78%였다.",
    "거실 에어컨 사용량이 평소보다 30% 높았다."
  ]
}
```

**Response 200**

```json
{
  "object": "list",
  "model": "nomic-embed-text",
  "data": [
    {
      "index": 0,
      "embedding": [0.0123, -0.0456, 0.0789]
    }
  ],
  "usage": {
    "prompt_tokens": 18,
    "total_tokens": 18
  }
}
```

`embedding` 배열 길이는 에이전트 서버의 임베딩 모델 차원(예: 768)과 같다.

**Response 400**

```json
{
  "error": {
    "code": "INVALID_REQUEST",
    "message": "input 은 비어 있을 수 없습니다.",
    "field": "input"
  }
}
```

---



### 전체 엔드포인트 요약

```http
GET  /llm/v1/models
POST /llm/v1/chat/completions
POST /llm/v1/embeddings
```



### 백엔드 연동 지점

- 백엔드가 LLM이 필요한 작업(데일리 메시지·리포트 생성 등)에서 `/models`로 조회한 실제 모델 이름을
지정해 `/chat/completions`를 호출한다.
- 프론트로 실시간 응답을 흘려야 하면 `stream: true`로 SSE를 소비해 그대로 중계한다.
- RAG용 임베딩이 필요하면 `/embeddings`로 텍스트를 벡터화한다.
- 백엔드는 이 API를 통해서만 LLM에 접근한다. 제공자(Gemini/Ollama) URL을 직접 호출하지 않는다.
- **챗 턴**(`/chat/v1/turns`) LLM 은 에이전트가 직접 호출하므로 이 포워딩 API 와 무관하다.

---
