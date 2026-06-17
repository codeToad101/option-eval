#!/bin/bash
# catchup_runner.sh
# -----------------
# Called by launchd on wake-from-sleep AND at 16:35 ET daily.
# Decides whether a collection run is needed and fires collector.py if so.
#
# Logic:
#   1. It must be a weekday (Mon–Fri)
#   2. Current ET time must be >= 16:30 (market close)
#   3. Today's collection must not have already run successfully
#
# launchd does NOT retroactively fire missed calendar triggers after sleep,
# so the wake-from-sleep trigger (OnDemand via sleepwatcher, or
# NSWorkspaceDidWakeNotification via a small Swift helper) calls this script,
# which then checks all conditions itself.

set -euo pipefail

# ── Config ────────────────────────────────────────────────────────────────────

# Absolute path to the python/ directory inside your repo
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="$SCRIPT_DIR/../.venv/bin/python"
COLLECTOR="$SCRIPT_DIR/collector.py"
LOG_DIR="$SCRIPT_DIR/../logs"
LOCKFILE="/tmp/options_collector.lock"

# ── Guards ────────────────────────────────────────────────────────────────────

# Prevent concurrent runs
if [ -f "$LOCKFILE" ]; then
    echo "[catchup] Lock file exists — another run in progress, exiting."
    exit 0
fi
trap 'rm -f "$LOCKFILE"' EXIT
touch "$LOCKFILE"

mkdir -p "$LOG_DIR"

# Check weekday in ET
DOW=$(TZ="America/New_York" date +%u)   # 1=Mon … 7=Sun
if [ "$DOW" -ge 6 ]; then
    echo "[catchup] Weekend — no collection needed."
    exit 0
fi

# Check time in ET: must be >= 16:30 (1630)
HOUR_MIN=$(TZ="America/New_York" date +%H%M)
if [ "$HOUR_MIN" -lt 1630 ]; then
    echo "[catchup] It is $HOUR_MIN ET — market not yet closed, exiting."
    exit 0
fi

TODAY=$(TZ="America/New_York" date +%Y-%m-%d)
DAILY_LOG="$LOG_DIR/$TODAY.log"

# Check if already ran today by inspecting the SQLite DB
# (collector.py's --force flag bypasses this check)
ALREADY_RAN=$("$PYTHON" - <<'PYEOF'
import sys
sys.path.insert(0, ''"$SCRIPT_DIR"'')
from db import RunDB
from config import TICKERS
from datetime import date
db = RunDB()
ran = db.already_ran_today(date.today().isoformat(), TICKERS)
print("yes" if len(ran) >= len(TICKERS) * 0.8 else "no")
PYEOF
)

if [ "$ALREADY_RAN" = "yes" ]; then
    echo "[catchup] Collection already completed today ($TODAY), exiting."
    exit 0
fi

# ── Run ───────────────────────────────────────────────────────────────────────

echo "[catchup] Starting collection for $TODAY at $(date)" | tee -a "$DAILY_LOG"
"$PYTHON" "$COLLECTOR" >> "$DAILY_LOG" 2>&1
EXIT_CODE=$?

if [ $EXIT_CODE -eq 0 ]; then
    echo "[catchup] Collection finished successfully." | tee -a "$DAILY_LOG"
else
    echo "[catchup] Collection finished with errors (exit $EXIT_CODE)." | tee -a "$DAILY_LOG"
fi

exit $EXIT_CODE