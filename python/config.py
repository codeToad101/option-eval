"""
config.py  —  Central config for the options collector pipeline.
Edit this file to change tickers, paths, email settings, etc.
"""

from pathlib import Path

# ── Bridge ────────────────────────────────────────────────────────────────────

BRIDGE_HOST = "127.0.0.1"
BRIDGE_PORT = 19999

# ── Tickers ───────────────────────────────────────────────────────────────────

TICKERS = [
    "SPY",    # S&P 500 ETF — baseline, deepest liquidity
    "QQQ",    # Nasdaq-100 — tech-heavy, slightly more explosive than SPY
    "NVDA",   # High-beta single stock, IV 50-80%, AI news sensitive
    "TSLA",   # Explosive retail/macro hybrid, IV often 60-100%+
    "AMZN",   # Mid-volatility single stock, earnings events
    "GLD",    # Gold ETF — macro/geopolitical vol, uncorrelated to equities
    "TLT",    # Long-duration Treasuries — rate-sensitive, inst. dominated
    "IWM",    # Russell 2000 small-cap — regime indicator, different term structure
    "MSTR",   # Leveraged BTC proxy, IV 100-200%+, extreme end of vol spectrum
    "UVXY",   # Leveraged VIX ETF — vol-on-vol dynamics, contango decay
]

# Tickers where ±35% strike range is used instead of default ±20%
# (high IV names that can move far between collection and expiry)
WIDE_STRIKE_TICKERS = {"NVDA", "TSLA", "MSTR", "UVXY"}

# ── Expiry schedule ───────────────────────────────────────────────────────────

WEEKLY_EXPIRIES  = 4   # next N Fridays
MONTHLY_EXPIRIES = 2   # next N 3rd-Friday monthlies beyond the weeklies

# ── Paths ─────────────────────────────────────────────────────────────────────

# Data lives outside the repo — adjust to your preferred location
_REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR   = _REPO_ROOT / "data" / "options"
LOG_DIR    = _REPO_ROOT / "logs"
DB_PATH    = _REPO_ROOT / "logs" / "runs.db"

# ── Email (weekly digest + hard-failure alerts) ───────────────────────────────

EMAIL_ENABLED   = True          # Set True once SMTP details are filled in
EMAIL_FROM      = "asiyakk123@gmail.com"
EMAIL_TO        = "asiyakk123@gmail.com"
SMTP_HOST       = "smtp.gmail.com"
SMTP_PORT       = 587
SMTP_USER       = "asiyakk123@gmail.com"
SMTP_PASSWORD   = "djra zpsm avcm xwvd"             # Use app password for Gmail; or load from .env

# Send an immediate alert if a ticker returns zero quotes
ALERT_ON_HARD_FAILURE = True

# Weekly digest fires on this weekday (0=Mon … 6=Sun) at the end of the run
DIGEST_WEEKDAY = 6  # Sunday