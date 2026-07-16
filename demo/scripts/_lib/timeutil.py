"""공용 날짜/시간 헬퍼. 목업 기간은 2026-06-01 ~ 2026-06-30(30일) 고정."""

from __future__ import annotations

from datetime import date, datetime, timedelta

MONTH_START = date(2026, 6, 1)
MONTH_END = date(2026, 6, 30)
DAYS_IN_MONTH = (MONTH_END - MONTH_START).days + 1  # 30

DOW_KO = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]


def fmt_date(d: date) -> str:
    return d.strftime("%Y-%m-%d")


def fmt_dt(dt: datetime) -> str:
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def parse_dt(s: str) -> datetime:
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S")


def june_dates() -> list[date]:
    return [MONTH_START + timedelta(days=i) for i in range(DAYS_IN_MONTH)]


def day_of_week(d: date) -> str:
    return DOW_KO[d.weekday()]


def is_weekend(d: date) -> bool:
    return d.weekday() >= 5


def monday_of_week(d: date) -> date:
    return d - timedelta(days=d.weekday())


def sliding_week_starts(days: list[date]) -> list[date]:
    """매일 '최근 7일' 슬라이딩 창의 첫날 목록(7일 미만인 앞부분은 제외)."""
    return [d for d in days if (d - days[0]).days >= 6]


def clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, x))
