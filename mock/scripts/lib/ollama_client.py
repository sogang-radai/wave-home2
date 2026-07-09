"""로컬 Ollama 서버에 직접 붙어 nomic-embed-text 임베딩을 생성한다.

에이전트 서버(/llm/v1/embeddings)를 거치지 않고 스크립트가 Ollama를 직접 호출한다
(작업 지시: "임베딩 벡터는 이 PC에 내장된 ollama 서버에 nomic_text_embed를 생성 코드에서
직접 호출할 예정"). 배치 엔드포인트(`/api/embed`)를 우선 쓰고, 실패하면 단건
(`/api/embeddings`)으로 폴백한다.
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request

DEFAULT_BASE_URL = "http://127.0.0.1:11434"
DEFAULT_MODEL = "nomic-embed-text"
EMBED_DIM = 768


def _post_json(url: str, payload: dict, timeout: float = 60.0) -> dict:
    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, headers={"Content-Type": "application/json"}, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def embed_texts(
    texts: list[str],
    model: str = DEFAULT_MODEL,
    base_url: str = DEFAULT_BASE_URL,
) -> list[list[float]]:
    """텍스트 목록을 임베딩한다. 빈 문자열은 그대로 빈 벡터 요청을 보내지 않도록 주의."""
    if not texts:
        return []

    try:
        resp = _post_json(f"{base_url}/api/embed", {"model": model, "input": texts})
        embeddings = resp.get("embeddings")
        if embeddings and len(embeddings) == len(texts):
            return embeddings
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        print(f"[ollama] /api/embed 배치 호출 실패({exc}), 단건 폴백으로 재시도")

    # 폴백: 단건 /api/embeddings
    out: list[list[float]] = []
    for t in texts:
        resp = _post_json(f"{base_url}/api/embeddings", {"model": model, "prompt": t})
        out.append(resp["embedding"])
    return out


def embed_text(text: str, model: str = DEFAULT_MODEL, base_url: str = DEFAULT_BASE_URL) -> list[float]:
    return embed_texts([text], model=model, base_url=base_url)[0]


def to_vec_blob(embedding: list[float]) -> bytes:
    """sqlite-vec 가상 테이블에 넣을 float32 리틀엔디안 바이트열로 변환."""
    import struct

    return struct.pack(f"<{len(embedding)}f", *embedding)
