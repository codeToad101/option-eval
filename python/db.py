"""
db.py  —  SQLite run log.

Schema
------
runs  : one row per (run_date, ticker, expiry) attempt
        status: 'ok' | 'warn' | 'fail'

Used by:
  - collector.py  to write results
  - monitor.py    to read results for weekly digest
  - check.py      for CLI data-health queries
"""

import sqlite3
from datetime import date
from pathlib import Path

from config import DB_PATH


class RunDB:
    def __init__(self, path: Path = DB_PATH):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(str(path))
        self.conn.row_factory = sqlite3.Row
        self._init_schema()

    def _init_schema(self):
        self.conn.executescript("""
            CREATE TABLE IF NOT EXISTS runs (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                run_date        TEXT    NOT NULL,
                ticker          TEXT    NOT NULL,
                expiry          TEXT,
                quotes_received INTEGER NOT NULL DEFAULT 0,
                validation_flags INTEGER NOT NULL DEFAULT 0,
                status          TEXT    NOT NULL,   -- 'ok' | 'warn' | 'fail'
                duration_ms     INTEGER NOT NULL DEFAULT 0,
                error_text      TEXT,
                created_at      TEXT    DEFAULT (datetime('now'))
            );
            CREATE INDEX IF NOT EXISTS idx_runs_date   ON runs(run_date);
            CREATE INDEX IF NOT EXISTS idx_runs_ticker ON runs(ticker);

            CREATE TABLE IF NOT EXISTS daily_summary (
                run_date        TEXT    PRIMARY KEY,
                tickers_ok      INTEGER DEFAULT 0,
                tickers_warn    INTEGER DEFAULT 0,
                tickers_fail    INTEGER DEFAULT 0,
                total_quotes    INTEGER DEFAULT 0,
                digest_sent     INTEGER DEFAULT 0,   -- bool
                created_at      TEXT    DEFAULT (datetime('now'))
            );
        """)
        self.conn.commit()

    def log_run(self, run_date: str, ticker: str, expiry: str | None,
                quotes: int, flags: int, status: str,
                duration_ms: int, error: str | None = None):
        self.conn.execute("""
            INSERT INTO runs
                (run_date, ticker, expiry, quotes_received, validation_flags,
                 status, duration_ms, error_text)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (run_date, ticker, expiry, quotes, flags, status, duration_ms, error))
        self.conn.commit()

    def upsert_daily_summary(self, run_date: str, ok: int, warn: int,
                             fail: int, total_quotes: int):
        self.conn.execute("""
            INSERT INTO daily_summary
                (run_date, tickers_ok, tickers_warn, tickers_fail, total_quotes)
            VALUES (?, ?, ?, ?, ?)
            ON CONFLICT(run_date) DO UPDATE SET
                tickers_ok   = excluded.tickers_ok,
                tickers_warn = excluded.tickers_warn,
                tickers_fail = excluded.tickers_fail,
                total_quotes = excluded.total_quotes
        """, (run_date, ok, warn, fail, total_quotes))
        self.conn.commit()

    def already_ran_today(self, run_date: str, tickers: list[str]) -> list[str]:
        """Return subset of tickers that already have 'ok' or 'warn' rows today."""
        rows = self.conn.execute("""
            SELECT DISTINCT ticker FROM runs
            WHERE run_date = ? AND status IN ('ok', 'warn')
        """, (run_date,)).fetchall()
        ran = {r["ticker"] for r in rows}
        return [t for t in tickers if t in ran]

    def week_summary(self, iso_week_dates: list[str]) -> list[sqlite3.Row]:
        placeholders = ",".join("?" * len(iso_week_dates))
        return self.conn.execute(f"""
            SELECT run_date, ticker, expiry,
                   quotes_received, validation_flags, status, error_text
            FROM runs
            WHERE run_date IN ({placeholders})
            ORDER BY run_date, ticker, expiry
        """, iso_week_dates).fetchall()

    def dates_with_data(self) -> list[str]:
        rows = self.conn.execute("""
            SELECT DISTINCT run_date FROM runs
            WHERE status IN ('ok','warn')
            ORDER BY run_date
        """).fetchall()
        return [r["run_date"] for r in rows]

    def ticker_summary(self, ticker: str, limit: int = 30) -> list[sqlite3.Row]:
        return self.conn.execute("""
            SELECT run_date, expiry, quotes_received, validation_flags,
                   status, duration_ms, error_text
            FROM runs
            WHERE ticker = ?
            ORDER BY run_date DESC, expiry
            LIMIT ?
        """, (ticker, limit)).fetchall()

    def close(self):
        self.conn.close()