#!/usr/bin/env python3
"""Simple speed + quality bench for local Ollama models."""
from __future__ import annotations

import json
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:11434"
MODELS = sys.argv[2:] or ["qwen3.5:9b", "gemma4:e4b", "gemma4:12b-mlx"]

SPEED_PROMPT = "Explain photosynthesis in about 100 words."
QUALITY_PROMPTS = [
    (
        "math",
        "A store sells apples for $2 each and oranges for $3 each. "
        "You buy 4 apples and 5 oranges, then get a $2 discount. "
        "How much do you pay? Reply with only the final number and a one-line calculation.",
    ),
    (
        "ko_reason",
        "다음을 한 문장으로 요약하세요: 스마트홈에서 수면 센서가 움직임을 감지하면 "
        "야간 조명을 켜고, 아침이 되면 알람과 함께 커튼을 연다.",
    ),
    (
        "code",
        "Write a Python function is_palindrome(s: str) -> bool that ignores spaces "
        "and letter case. Return only the function, no explanation.",
    ),
    (
        "factual",
        "What is the capital of Australia? One short sentence.",
    ),
]


def post(path: str, payload: dict, timeout: float = 300.0) -> dict:
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        f"{HOST}{path}",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def chat(model: str, prompt: str, *, think: bool, num_predict: int) -> dict:
    return post(
        "/api/chat",
        {
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "think": think,
            "stream": False,
            "options": {"num_predict": num_predict, "temperature": 0.2},
        },
    )


def tok_s(d: dict) -> float:
    ec = d.get("eval_count") or 0
    td = d.get("eval_duration") or 0
    return ec / (td / 1e9) if td else 0.0


def stop(model: str) -> None:
    try:
        post("/api/generate", {"model": model, "keep_alive": 0}, timeout=30)
    except Exception:
        pass


def speed_bench(model: str) -> dict:
    stop(model)
    time.sleep(0.5)
    # cold-ish load + discard
    chat(model, "hi", think=False, num_predict=8)
    rates = []
    sample = ""
    for _ in range(3):
        d = chat(model, SPEED_PROMPT, think=False, num_predict=128)
        rates.append(tok_s(d))
        m = d.get("message") or {}
        sample = (m.get("content") or "")[:200]
    return {
        "avg_tok_s": sum(rates) / len(rates),
        "runs": rates,
        "sample": sample,
    }


def quality_bench(model: str) -> list[dict]:
    out = []
    for name, prompt in QUALITY_PROMPTS:
        d = chat(model, prompt, think=False, num_predict=256)
        m = d.get("message") or {}
        content = (m.get("content") or "").strip()
        thinking = (m.get("thinking") or "").strip()
        out.append(
            {
                "task": name,
                "content": content,
                "thinking_len": len(thinking),
                "tok_s": tok_s(d),
                "eval_count": d.get("eval_count"),
                "wall_s": (d.get("total_duration") or 0) / 1e9,
            }
        )
    return out


def main() -> None:
    print(f"host={HOST}")
    print(f"models={MODELS}")
    results = {}
    for model in MODELS:
        print(f"\n=== SPEED {model} ===", flush=True)
        try:
            sp = speed_bench(model)
            print(
                f"avg={sp['avg_tok_s']:.2f} tok/s  runs={[round(x,2) for x in sp['runs']]}",
                flush=True,
            )
            print(f"sample: {sp['sample']!r}", flush=True)
            print(f"\n=== QUALITY {model} ===", flush=True)
            q = quality_bench(model)
            for item in q:
                print(
                    f"[{item['task']}] {item['tok_s']:.1f} tok/s wall={item['wall_s']:.2f}s "
                    f"eval={item['eval_count']} think_len={item['thinking_len']}",
                    flush=True,
                )
                print(item["content"][:500], flush=True)
                print("---", flush=True)
            results[model] = {"speed": sp, "quality": q}
            stop(model)
        except Exception as exc:
            print(f"FAILED {model}: {exc}", flush=True)
            results[model] = {"error": str(exc)}

    out_path = "/tmp/ollama_model_bench.json"
    with open(out_path, "w") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    print(f"\nWrote {out_path}")


if __name__ == "__main__":
    main()
