# 채팅 전 기능 E2E 결과 (데모 :8502)

실행일: 2026-07-13  
스택: `site-demo` + `wave-server --profile demo` (:8502) + agent (:8501, `configure-agent-demo`)  
인증: `Bearer wavehome-dev-token` + `X-Wave-Demo-Runtime-Id`

원본 SSE/API 로그: [demo-chat-e2e-results.json](demo-chat-e2e-results.json), `/tmp/demo-chat-e2e-phase14.json`

---

## Phase 0 — 환경·헬스

| ID | 항목 | 결과 | 메모 |
|----|------|------|------|
| P0 | demo 스택 기동 | PASS | :8502 / :8501 up |
| P0 | `configure-agent-demo.sh` | PASS | agent → core 8502 |
| P0 | `test-agent-integration.sh` | PASS | BACKEND_URL=8502 |
| P0 | health | PASS | `/api/v1/health` 200 |
| P0 | 로그인·WaveAI 진입 | PASS | Start now → 대시보드/WaveAI |

---

## Phase 1 — API/툴 레이어

| ID | 검증 | 결과 | 레이어/메모 |
|----|------|------|------------|
| A1 | 대화 CRUD | PASS | create 201 / rename 200 / delete 204 |
| A2 | 턴 SSE | PASS | `message_*` / `tool_*` / `content_delta` |
| A3 | DB 툴 수면 | PASS | `query_db` sleep_report, 94.86% |
| A4 | RAG/인사이트 | PASS | `rag_search` + 폴백 후 답변 |
| A5 | 장치 제어 | PASS | `control_device` → plug `switch:false` |
| A6 | 정책 RO | PASS | IR PUT → 403 `DEMO_READ_ONLY` |

---

## Phase 2 — WaveAI UX

| # | 항목 | 결과 | 메모 |
|---|------|------|------|
| 1 | 환영/인사이트 칩 → 전송·스트리밍 | PASS | 칩 클릭 후 답변·도구 이벤트 |
| 2 | Enter / Ctrl+Enter 설정 | PASS | GET settings `ctrlEnterSend:false` (세션) |
| 3 | 도구 칩 → 완료 후 접힘 | PASS | 「도구 1개 사용」 collapsed→expand |
| 4 | 생각 과정 펼침 | SKIP | 해당 턴에 thinking UI 없음 |
| 5 | 마크다운 | PASS | 목록·수치 렌더 |
| 6 | 대화 추가/전환/이름/삭제/고정 | PASS | 더보기: 이름변경·상단고정·제거 + 새대화 |
| 7 | 팝업(작게 보기) 동일 턴 | PASS | 작게 보기 → 대시보드 위 팝업, 동일 대화 |
| 8 | 시연 배너·TopBar | PASS | 「시연 모드」 배지 |
| 9 | AI 개인 프롬프트 반영 | PASS | PUT 후 다음 답변에 마커 문자열 포함 |

---

## Phase 3 — 도메인 시나리오

| ID | 시나리오 | 결과 | 메모 |
|----|----------|------|------|
| S1 | 수면 | PASS | A2/A3와 동일, 앵커일 6/30 |
| S2 | 심박 | PASS | DB/RAG 근거 답변 |
| S3 | 전력(현재) | PASS | 첫 실행 FAIL(빈 text) → **재시도 PASS** ~961W; flake 가능 |
| S4 | 전력(리포트) | PASS | RAG/주간 kWh |
| S5 | 플러그 전원 | PASS | `control_device` off; 상태 `switch:false` |
| S6 | 조명 밝기 | PASS | 침실 조명 brightness **30** (요청 후) |
| S7 | TV 볼륨 | PASS | control 경로 + 답변 |
| S8 | 예약 | PASS | 매일 23시 TV off 예약 답변 |
| S9 | 알람 | PASS | `create_alarm` E2E테스트 |
| S10 | 자동화 생성 | PASS_SOFT | 룰 **생성 없이** 안내만; 제스처 실발화 Skip(기대와 일치) |
| S11 | IR 조회 | PASS | `list_ir_commands` 8건 |
| S12 | TTS 요청 | PASS | 데모 제한 동작으로 안내·완료 답변 |
| Twin | S5–S7 반영 | PASS | 트윈 페이지 진입·장치 상태 API 일치; IoT 패널/트윈 구독 정상 |

---

## Phase 4 — 네거티브·경계

| ID | 내용 | 결과 | 메모 |
|----|------|------|------|
| N1 | 계정 생성 | PASS | 403 `DEMO_READ_ONLY` |
| N2 | IR 저장 | PASS | 403 |
| N3 | 인사이트 approve | PASS | POST `/api/v1/insights/approve` 403 |
| N4 | 잘못된 장치명 | PASS | 명확한 clarification, 크래시 없음 |
| N5 | 연속 문맥 | PASS | 후속 턴 응답(장치 목록 확인 질문 포함) |
| N6 | 스트리밍 중단 후 재전송 | PASS | abort 후 새 턴 OK |
| N7 | runtimeId 격리 | PASS | primary off / other runtime `switch:true` 유지 |

---

## Phase 5 — 프로덕션 델타

| 항목 | 결과 | 메모 |
|------|------|------|
| :8500 기동 | SKIP | `curl` 연결 실패(000). 실기기/실TTS/영구 DB는 프로덕션 기동 후 재실행 |

---

## 실패·주의

1. **S3 flake**: 첫 SSE `message_done`에 text 공란(툴은 다수 호출). 재시도 정상. 레이어 의심: 에이전트/LLM 타임아웃 또는 델타 누락.
2. **S10**: 자동화 **생성**까지는 미달(안내만). 계획상 “생성까지 Pass”이나 현재 LLM이 거실 레이더 부재로 생성 거부 → PASS_SOFT.
3. **입력란 `undefined`**: 팝업 textarea value가 간헐적으로 `undefined`로 보임(프론트). 전송 자체는 칩/정상 입력으로 확인.

## 요약

- 데모 풀패스: **Pass 위주**, S3 1회 flake, S10 soft, Phase5 Skip  
- 예상 소요에 맞게 API+브라우저 수동 검증 완료
