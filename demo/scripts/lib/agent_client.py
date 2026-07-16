"""에이전트 서버(:8501) sleep/v1, power/v1 job API 클라이언트.

docs/agent-api/sleep-analysis-api.md, power-analysis-api.md 의 비동기 job 패턴을 그대로
따른다: POST -> 202 + jobId, GET /jobs/{jobId} 폴링(1~3초 시작, 30초 경과 후 5~10초 백오프).
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request

DEFAULT_BASE_URL = "http://127.0.0.1:8501"
POLL_INITIAL_S = 2.0
POLL_BACKOFF_AFTER_S = 30.0
POLL_BACKOFF_S = 7.0
POLL_TIMEOUT_S = 600.0


class AgentJobError(RuntimeError):
    def __init__(self, code: str, message: str, job_id: str | None = None):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.job_id = job_id


def _request(method: str, url: str, body: dict | None = None, timeout: float = 30.0) -> tuple[int, dict]:
    data = json.dumps(body).encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        payload = json.loads(exc.read().decode("utf-8")) if exc.fp else {}
        return exc.code, payload


def _post(base_url: str, path: str, body: dict) -> str:
    status, payload = _request("POST", f"{base_url}{path}", body)
    if status == 202:
        return payload["jobId"]
    if status == 409:
        err = payload.get("error", {})
        job_id = err.get("jobId")
        if job_id:
            print(f"[agent] {path}: 이미 진행 중인 job 재사용 {job_id}")
            return job_id
        raise AgentJobError(err.get("code", "CONFLICT"), err.get("message", str(payload)))
    err = payload.get("error", {})
    raise AgentJobError(err.get("code", f"HTTP_{status}"), err.get("message", str(payload)))


def _poll(base_url: str, jobs_path: str, job_id: str, timeout: float = POLL_TIMEOUT_S) -> dict:
    start = time.monotonic()
    wait = POLL_INITIAL_S
    while True:
        status, payload = _request("GET", f"{base_url}{jobs_path}/{job_id}")
        if status == 404:
            raise AgentJobError("JOB_NOT_FOUND", f"job {job_id} not found")
        job_status = payload.get("status")
        if job_status == "done":
            return payload["result"]
        if job_status == "failed":
            err = payload.get("error", {})
            raise AgentJobError(err.get("code", "GENERATION_FAILED"), err.get("message", "job failed"), job_id)
        if time.monotonic() - start > timeout:
            raise AgentJobError("JOB_TIMEOUT", f"job {job_id} exceeded {timeout}s")
        time.sleep(wait)
        if time.monotonic() - start > POLL_BACKOFF_AFTER_S:
            wait = POLL_BACKOFF_S


def create_sleep_report(body: dict, base_url: str = DEFAULT_BASE_URL) -> dict:
    job_id = _post(base_url, "/sleep/v1/reports", body)
    return _poll(base_url, "/sleep/v1/jobs", job_id)


def create_power_report(body: dict, base_url: str = DEFAULT_BASE_URL) -> dict:
    job_id = _post(base_url, "/power/v1/reports", body)
    return _poll(base_url, "/power/v1/jobs", job_id)


def health_check(base_url: str = DEFAULT_BASE_URL, timeout: float = 3.0) -> bool:
    try:
        status, _ = _request("GET", f"{base_url}/health", timeout=timeout)
        return status == 200
    except Exception:  # noqa: BLE001 - health probe, any failure means "not reachable"
        return False
