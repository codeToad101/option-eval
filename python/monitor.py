"""
monitor.py  —  Email alerts and weekly digest.

Called automatically by collector.py after each run.
Also callable standalone:
    python monitor.py --digest        # force-send this week's digest
    python monitor.py --test-email    # send a test message to verify SMTP
"""

import argparse
import logging
import smtplib
from datetime import date, timedelta
from email.mime.multipart import MIMEMultipart
from email.mime.text import MIMEText

from config import (
    ALERT_ON_HARD_FAILURE,
    DIGEST_WEEKDAY,
    EMAIL_ENABLED,
    EMAIL_FROM,
    EMAIL_TO,
    SMTP_HOST,
    SMTP_PASSWORD,
    SMTP_PORT,
    SMTP_USER,
)
from db import RunDB

log = logging.getLogger("monitor")


# ── Email send ────────────────────────────────────────────────────────────────

def _send_email(subject: str, body_text: str, body_html: str | None = None):
    if not EMAIL_ENABLED:
        log.info("Email disabled — would have sent: %s", subject)
        return

    msg = MIMEMultipart("alternative")
    msg["Subject"] = subject
    msg["From"]    = EMAIL_FROM
    msg["To"]      = EMAIL_TO

    msg.attach(MIMEText(body_text, "plain"))
    if body_html:
        msg.attach(MIMEText(body_html, "html"))

    try:
        with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as s:
            s.starttls()
            s.login(SMTP_USER, SMTP_PASSWORD)
            s.sendmail(EMAIL_FROM, EMAIL_TO, msg.as_string())
        log.info("Email sent: %s", subject)
    except Exception as e:
        log.error("Failed to send email '%s': %s", subject, e)


# ── Hard-failure alert ────────────────────────────────────────────────────────

def alert_hard_failure(run_date: str, failures: list[dict]):
    """Send an immediate alert if any ticker returned zero quotes."""
    if not ALERT_ON_HARD_FAILURE:
        return
    if not failures:
        return

    tickers = [f["ticker"] for f in failures]
    errors  = "\n".join(f"  {f['ticker']}: {f.get('error','unknown')}" for f in failures)

    subject = f"[options-collector] HARD FAILURE {run_date} — {', '.join(tickers)}"
    body = (
        f"Options collector hard failure on {run_date}.\n\n"
        f"The following tickers returned zero quotes:\n{errors}\n\n"
        f"Check that IB Gateway is running and ibkr_bridge.py is connected.\n"
        f"Log: logs/{run_date}.log\n"
    )
    _send_email(subject, body)


# ── Weekly digest ─────────────────────────────────────────────────────────────

def _week_dates() -> list[str]:
    """ISO dates for Mon–Sun of the current week."""
    today = date.today()
    monday = today - timedelta(days=today.weekday())
    return [(monday + timedelta(days=i)).isoformat() for i in range(7)]


def build_digest_text(rows: list, week_dates: list[str]) -> tuple[str, str]:
    """Build plain text + HTML digest from run rows."""
    from collections import defaultdict

    # Group: date → ticker → list of rows
    by_date: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))
    for r in rows:
        by_date[r["run_date"]][r["ticker"]].append(r)

    trading_days = [d for d in week_dates if d in by_date]

    lines = ["Options Collector — Weekly Digest", "=" * 40, ""]

    if not trading_days:
        lines.append("No data collected this week.")
        return "\n".join(lines), ""

    total_quotes  = 0
    total_ok      = 0
    total_warn    = 0
    total_fail    = 0

    for d in trading_days:
        lines.append(f"  {d}")
        for ticker, ticker_rows in sorted(by_date[d].items()):
            statuses = [r["status"] for r in ticker_rows]
            quotes   = sum(r["quotes_received"] for r in ticker_rows)
            total_quotes += quotes
            if "fail" in statuses:
                icon = "✗"
                total_fail += 1
            elif "warn" in statuses:
                icon = "⚠"
                total_warn += 1
            else:
                icon = "✓"
                total_ok += 1

            errors = [r["error_text"] for r in ticker_rows if r["error_text"]]
            error_str = f"  ({errors[0]})" if errors else ""
            lines.append(f"    {icon} {ticker:<6} {quotes:>6} quotes{error_str}")
        lines.append("")

    lines += [
        "─" * 40,
        f"Days collected : {len(trading_days)}",
        f"Ticker-days ok : {total_ok}",
        f"Ticker-days warn: {total_warn}",
        f"Ticker-days fail: {total_fail}",
        f"Total quotes   : {total_quotes:,}",
        "",
        "Manage: logs/runs.db  |  Data: data/options/",
    ]

    plain = "\n".join(lines)

    # Minimal HTML version
    rows_html = ""
    for d in trading_days:
        rows_html += f'<tr><td colspan="3" style="padding-top:12px;font-weight:500">{d}</td></tr>'
        for ticker, ticker_rows in sorted(by_date[d].items()):
            statuses = [r["status"] for r in ticker_rows]
            quotes   = sum(r["quotes_received"] for r in ticker_rows)
            color = "#c0392b" if "fail" in statuses else \
                    "#e67e22" if "warn" in statuses else "#27ae60"
            icon  = "✗" if "fail" in statuses else \
                    "⚠" if "warn" in statuses else "✓"
            errors = [r["error_text"] for r in ticker_rows if r["error_text"]]
            err_html = f'<br><small style="color:#888">{errors[0]}</small>' if errors else ""
            rows_html += (
                f'<tr>'
                f'<td style="padding:2px 8px;color:{color}">{icon}</td>'
                f'<td style="padding:2px 8px;font-family:monospace">{ticker}</td>'
                f'<td style="padding:2px 8px">{quotes:,} quotes{err_html}</td>'
                f'</tr>'
            )

    html = f"""
    <html><body style="font-family:sans-serif;font-size:14px;color:#222">
    <h2 style="margin-bottom:4px">Options Collector — Weekly Digest</h2>
    <p style="color:#555">Week of {week_dates[0]} to {week_dates[-1]}</p>
    <table style="border-collapse:collapse;margin-bottom:16px">
    {rows_html}
    </table>
    <table style="font-size:13px;color:#555">
      <tr><td>Days collected</td><td style="padding-left:12px">{len(trading_days)}</td></tr>
      <tr><td>Ticker-days ok</td><td style="padding-left:12px">{total_ok}</td></tr>
      <tr><td>Ticker-days warn</td><td style="padding-left:12px">{total_warn}</td></tr>
      <tr><td>Ticker-days fail</td><td style="padding-left:12px">{total_fail}</td></tr>
      <tr><td>Total quotes</td><td style="padding-left:12px">{total_quotes:,}</td></tr>
    </table>
    </body></html>
    """
    return plain, html


def maybe_send_weekly_digest(db: RunDB):
    """Send digest if today is DIGEST_WEEKDAY and it hasn't been sent yet."""
    today = date.today()
    if today.weekday() != DIGEST_WEEKDAY:
        return

    # Check if already sent today
    row = db.conn.execute(
        "SELECT digest_sent FROM daily_summary WHERE run_date = ?",
        (today.isoformat(),)
    ).fetchone()
    if row and row["digest_sent"]:
        log.info("Weekly digest already sent today, skipping.")
        return

    week_dates = _week_dates()
    rows = db.week_summary(week_dates)
    plain, html = build_digest_text(rows, week_dates)

    subject = f"[options-collector] Weekly digest {week_dates[0]} → {week_dates[-1]}"
    _send_email(subject, plain, html)

    db.conn.execute("""
        INSERT INTO daily_summary (run_date, digest_sent)
        VALUES (?, 1)
        ON CONFLICT(run_date) DO UPDATE SET digest_sent = 1
    """, (today.isoformat(),))
    db.conn.commit()


# ── CLI ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Options collector monitor")
    parser.add_argument("--digest",     action="store_true",
                        help="Force-send this week's digest now")
    parser.add_argument("--test-email", action="store_true",
                        help="Send a test email to verify SMTP config")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s  %(levelname)-7s  %(message)s")

    if args.test_email:
        _send_email(
            "[options-collector] Test email",
            "If you received this, SMTP is configured correctly."
        )
        return

    if args.digest:
        db = RunDB()
        week_dates = _week_dates()
        rows = db.week_summary(week_dates)
        plain, html = build_digest_text(rows, week_dates)
        print(plain)
        subject = f"[options-collector] Weekly digest {week_dates[0]} → {week_dates[-1]}"
        _send_email(subject, plain, html)
        return

    parser.print_help()


if __name__ == "__main__":
    main()