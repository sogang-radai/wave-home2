#!/usr/bin/env python3
"""03의 산출물(demo/ai_manual/*.json)을 읽어 Ollama(nomic-embed-text)로 직접 임베딩한다.

이미 embedding 필드가 있어도 재생성한다(항상 최신 텍스트 기준으로 재임베딩).
"""

from __future__ import annotations

import json
from pathlib import Path

from _lib import ollama_client

REPO_ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = REPO_ROOT / "demo" / "ai_manual"

BATCH_SIZE = 16


def embed_file(path: Path, text_field: str) -> None:
    items = json.loads(path.read_text(encoding="utf-8"))
    texts = [it[text_field] for it in items]
    embeddings: list[list[float]] = []
    for i in range(0, len(texts), BATCH_SIZE):
        chunk = texts[i : i + BATCH_SIZE]
        embeddings.extend(ollama_client.embed_texts(chunk))
        print(f"  {path.name}: {min(i + BATCH_SIZE, len(texts))}/{len(texts)}")
    for it, emb in zip(items, embeddings):
        it["embedding"] = emb
    path.write_text(json.dumps(items, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--only",
        action="append",
        metavar="FILE",
        help="특정 파일만 임베딩(예: --only power_report_1h.json). 여러 번 지정 가능",
    )
    args = parser.parse_args()

    targets = [
        ("insight.json", "text"),
        ("weekly_plan_report.json", "report_text"),
        ("power_report_1h.json", "report_text"),
        ("sleep_stat_30m_summary.json", "summary_text"),
    ]
    if args.only:
        wanted = set(args.only)
        targets = [(f, field) for f, field in targets if f in wanted]
    print(f"ollama={ollama_client.DEFAULT_BASE_URL}")
    for filename, field in targets:
        path = OUT_DIR / filename
        if not path.exists():
            print(f"  (없음, 스킵) {filename}")
            continue
        print(f"임베딩 중: {filename}")
        embed_file(path, field)

    print("=== 04_embed_manual_texts 완료 ===")


if __name__ == "__main__":
    main()
