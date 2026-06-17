# Options Collector — Setup & Operation

## Files

```
python/
├── collector.py            # main orchestrator — run this nightly
├── config.py               # all settings: tickers, paths, email
├── db.py                   # SQLite run log
├── validator.py            # data quality checks
├── writer.py               # Parquet writer
├── monitor.py              # email alerts + weekly digest
├── check.py                # CLI data health inspector
├── catchup_runner.sh       # shell wrapper called by launchd
├── wake_trigger.swift      # macOS wake listener (compile once)
├── com.options.collector.plist  # launchd job definition
└── requirements.txt
```

Data lives outside the repo:
```
data/options/{TICKER}/{YYYY-MM-DD}.parquet
logs/{YYYY-MM-DD}.log
logs/runs.db
```

---

## 1. Python environment

```bash
cd /path/to/your/repo
python3 -m venv .venv
source .venv/bin/activate
pip install -r python/requirements.txt
```

---

## 2. IB Gateway

- Download IB Gateway: https://www.interactivebrokers.com/en/trading/ibgateway.php
- Configure → API → Settings:
  - ✓ Enable ActiveX and Socket Clients
  - ✓ Socket port: 7497 (paper)
  - ✓ Allow connections from localhost only
- Leave IB Gateway running before 16:30 ET on collection days
- ibkr_bridge.py must be running before collector.py starts:
  ```bash
  source .venv/bin/activate
  python python/ibkr_bridge.py --paper
  ```

---

## 3. Email (optional but recommended)

Edit `python/config.py`:
```python
EMAIL_ENABLED = True
EMAIL_FROM    = "you@gmail.com"
EMAIL_TO      = "you@gmail.com"
SMTP_USER     = "you@gmail.com"
SMTP_PASSWORD = "your-gmail-app-password"  # not your login password
```

For Gmail: Settings → Security → 2FA → App Passwords → create one for "Mail".

Test it:
```bash
python python/monitor.py --test-email
```

---

## 4. macOS automation (wake-triggered catch-up)

### Step A — edit paths in the shell script

Open `python/catchup_runner.sh` and confirm `SCRIPT_DIR` resolves correctly,
or hardcode the absolute path to your repo's `python/` directory.

### Step B — install the launchd plist

```bash
# Replace EDIT_ME with your repo's absolute path in the plist first:
sed -i '' 's|EDIT_ME|/absolute/path/to/your/repo|g' \
    python/com.options.collector.plist

cp python/com.options.collector.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.options.collector.plist

# Verify it loaded:
launchctl list | grep options
```

This fires catchup_runner.sh at 16:35 ET on weekdays (if the Mac is awake).

### Step C — wake-from-sleep trigger

**Option 1 — sleepwatcher (simpler, Homebrew):**
```bash
brew install sleepwatcher
# Create ~/.wakeup script:
echo '#!/bin/bash' > ~/.wakeup
echo '/absolute/path/to/repo/python/catchup_runner.sh &' >> ~/.wakeup
chmod +x ~/.wakeup
brew services start sleepwatcher
```

**Option 2 — compile the Swift wake listener:**
```bash
swiftc python/wake_trigger.swift -o python/wake_trigger \
    -framework Cocoa -framework Foundation

# Edit EDIT_ME path inside wake_trigger.swift first ^^^
# Then add wake_trigger binary to Login Items:
# System Settings → General → Login Items → + → select wake_trigger binary
```

Either option makes the collector run automatically when you open your laptop
after market close, even if you missed the 16:35 ET calendar trigger.

---

## 5. Verify the setup

**Manual test run (doesn't write files):**
```bash
source .venv/bin/activate
python python/collector.py --dry-run
```

**Single ticker:**
```bash
python python/collector.py --ticker SPY
```

**Check collected data:**
```bash
python python/check.py                       # summary of all dates
python python/check.py --ticker NVDA         # NVDA run history
python python/check.py --gaps                # missing trading days
python python/check.py --read SPY 2025-06-16 # peek at a parquet file
```

**Force weekly digest email now:**
```bash
python python/monitor.py --digest
```

---

## 6. Normal operation checklist

Before 16:30 ET each weekday:
- [ ] IB Gateway is open and logged in (paper account)
- [ ] ibkr_bridge.py is running (`python python/ibkr_bridge.py --paper`)
- [ ] Mac lid open (or wake-trigger will handle it later)

The collector runs automatically at 16:35 ET or on next wake.
Sunday evening: check your email for the weekly digest.
Use `python check.py --gaps` to see any missed days.

---

## 7. Data schema reference

Each `.parquet` file: one ticker, one collection date, all expiries.

| Column           | Type    | Notes                              |
|------------------|---------|------------------------------------|
| raw_symbol       | string  | OCC symbol (SPY250620C00530000)    |
| underlying       | string  | ticker                             |
| expiration       | string  | YYYY-MM-DD                         |
| right            | string  | C / P                              |
| strike           | float32 |                                    |
| bid              | float32 |                                    |
| ask              | float32 |                                    |
| mid              | float32 |                                    |
| bid_size         | int32   |                                    |
| ask_size         | int32   |                                    |
| underlying_price | float32 | spot at collection time            |
| dte              | int16   | days to expiry                     |
| collection_date  | string  | YYYY-MM-DD                         |
| ts_ms            | int64   | epoch milliseconds                 |

Read in Python:
```python
import pandas as pd
df = pd.read_parquet("data/options/SPY/2025-06-16.parquet")
```

Read across all dates for one ticker:
```python
import pandas as pd
from pathlib import Path
df = pd.read_parquet("data/options/SPY/")   # reads entire directory
```