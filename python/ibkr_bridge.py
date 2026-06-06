#!/usr/bin/env python3
"""
ibkr_bridge.py  —  IB Gateway / TWS bridge for volarb C++ backend
═══════════════════════════════════════════════════════════════════

Architecture
────────────
  C++ IBKRProvider  ──TCP──►  this process  ──TWS API──►  IB Gateway / TWS

This process listens on localhost:19999 (configurable via --port).
C++ sends newline-delimited JSON requests; this process replies with
newline-delimited JSON responses.  One request → one response per line.

Protocol
────────
Request (C++ → Python):
  { "id": <int>, "method": "option_chain",
    "underlying": "SPY", "date": "2025-01-15", "underlying_price": 0.0 }

  { "id": <int>, "method": "underlying_trade", "symbol": "SPY" }

Response (Python → C++):
  Success:
    { "id": <int>, "ok": true,  "data": <payload> }
  Error:
    { "id": <int>, "ok": false, "error": "<message>" }

option_chain payload: list of OptionQuote-compatible dicts
  { "raw_symbol": "SPY250115C00500000",
    "underlying": "SPY",  "expiration": "2025-01-15",
    "right": "C",         "strike": 500.0,
    "bid": 3.40,  "ask": 3.50,  "mid": 3.45,
    "bid_size": 10, "ask_size": 10,
    "ts_ms": 1736985600000 }

underlying_trade payload:
  { "symbol": "SPY", "price": 587.32, "size": 100, "ts_ms": 1736985600000 }

Installation
────────────
  pip install ib_insync   # https://github.com/erdewit/ib_insync

TWS / IB Gateway setup
───────────────────────
  1. Download IB Gateway (lighter than full TWS):
       https://www.interactivebrokers.com/en/trading/ibgateway.php
  2. In IB Gateway: Configure → API → Settings
       ✓ Enable ActiveX and Socket Clients
       ✓ Socket port: 7497  (paper)  or  7496  (live)
       ✓ Allow connections from localhost only
  3. Run this bridge:
       python ibkr_bridge.py --paper          # paper account
       python ibkr_bridge.py --live           # live account  (be careful)
       python ibkr_bridge.py --paper --port 19999  # custom port

Usage from C++ side
───────────────────
  See IBKRProvider in market_data.hpp — it connects to localhost:19999
  and sends/receives the JSON protocol described above.
"""

import argparse
import asyncio
import json
import logging
import math
import sys
import time
from datetime import datetime, timezone

try:
    from ib_insync import IB, Stock, Option, util
except ImportError:
    sys.exit(
        "ERROR: ib_insync not installed.\n"
        "Run:  pip install ib_insync\n"
        "Docs: https://github.com/erdewit/ib_insync"
    )

# ── Logging ──────────────────────────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s  %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("ibkr_bridge")
util.logToConsole(logging.WARNING)   # silence ib_insync's own noise


# ── IB session (singleton) ────────────────────────────────────────────────────

class IBSession:
    def __init__(self, host: str, tws_port: int, client_id: int):
        self.ib = IB()
        self.host = host
        self.tws_port = tws_port
        self.client_id = client_id

    def connect(self):
        log.info("Connecting to TWS/Gateway at %s:%d (clientId=%d)…",
                 self.host, self.tws_port, self.client_id)
        self.ib.connect(self.host, self.tws_port, clientId=self.client_id)
        log.info("Connected — account: %s", self.ib.managedAccounts())

    def disconnect(self):
        self.ib.disconnect()

    # ── Market data helpers ───────────────────────────────────────────────

    def get_underlying_trade(self, symbol: str) -> dict:
        contract = Stock(symbol, "SMART", "USD")
        self.ib.qualifyContracts(contract)
        [ticker] = self.ib.reqTickers(contract)
        price = ticker.last or ticker.close or float("nan")
        size  = int(ticker.lastSize or 0)
        ts_ms = int(time.time() * 1000)
        return {"symbol": symbol, "price": price, "size": size, "ts_ms": ts_ms}

    def get_option_chain(self, underlying: str, date: str,
                         underlying_price: float = 0.0) -> list[dict]:
        """
        Fetch a full option chain for `underlying` expiring on `date`
        (format: "YYYY-MM-DD").

        Strategy
        ────────
        1. reqSecDefOptParams → exchange + strikes + expiries
        2. Filter to the requested expiry
        3. Build Option contracts for all strikes (call + put)
        4. reqTickers in bulk → bid/ask/last
        5. Return as list of dicts matching the bridge protocol
        """
        # Resolve spot if not supplied
        if underlying_price <= 0.0:
            trade = self.get_underlying_trade(underlying)
            underlying_price = trade["price"]

        # TWS uses YYYYMMDD expiry format
        expiry_tws = date.replace("-", "")  # "2025-01-15" → "20250115"

        # 1. Get option parameters (available strikes / expiries / exchange)
        stock = Stock(underlying, "SMART", "USD")
        self.ib.qualifyContracts(stock)
        chains = self.ib.reqSecDefOptParams(
            stock.symbol, "", stock.secType, stock.conId
        )

        # Pick the SMART / CBOE chain; prefer CBOE for US equity options
        chain = None
        for c in chains:
            if c.exchange in ("SMART", "CBOE"):
                chain = c
                break
        if chain is None and chains:
            chain = chains[0]
        if chain is None:
            raise RuntimeError(f"No option chain found for {underlying}")

        if expiry_tws not in chain.expirations:
            available = sorted(chain.expirations)[:8]
            raise RuntimeError(
                f"Expiry {date} not available for {underlying}. "
                f"Nearest: {available}"
            )

        # 2. Filter strikes to ±20% around spot (avoids requesting 1000+ contracts)
        lo = underlying_price * 0.80
        hi = underlying_price * 1.20
        strikes = [s for s in chain.strikes if lo <= s <= hi]

        if not strikes:
            strikes = list(chain.strikes)   # fallback: take all

        # 3. Build Contract objects
        contracts = []
        for strike in strikes:
            for right in ("C", "P"):
                contracts.append(
                    Option(underlying, expiry_tws, strike, right, "SMART")
                )

        # Qualify in batches (TWS throttles large batches)
        qualified = []
        batch = 50
        for i in range(0, len(contracts), batch):
            qualified.extend(self.ib.qualifyContracts(*contracts[i:i+batch]))

        if not qualified:
            return []

        # 4. Request tickers (snapshot)
        tickers = self.ib.reqTickers(*qualified)

        ts_ms = int(time.time() * 1000)
        quotes = []
        for t in tickers:
            c = t.contract
            bid   = t.bid   if not math.isnan(t.bid   or math.nan) else 0.0
            ask   = t.ask   if not math.isnan(t.ask   or math.nan) else 0.0
            last  = t.last  if not math.isnan(t.last  or math.nan) else 0.0
            mid   = (bid + ask) / 2.0 if bid and ask else last

            # OCC symbol: SPY + YYMMDD + C/P + 8-digit strike×1000
            yr   = c.lastTradeDateOrContractMonth[2:6]   # YYMMDD
            k8   = f"{int(round(c.strike * 1000)):08d}"
            occ  = f"{c.symbol}{yr}{c.right}{k8}"

            quotes.append({
                "raw_symbol":  occ,
                "underlying":  c.symbol,
                "expiration":  date,          # back to YYYY-MM-DD
                "right":       c.right,       # "C" or "P"
                "strike":      c.strike,
                "bid":         round(bid,  4),
                "ask":         round(ask,  4),
                "mid":         round(mid,  4),
                "bid_size":    int(t.bidSize  or 0),
                "ask_size":    int(t.askSize  or 0),
                "ts_ms":       ts_ms,
            })

        return quotes
    
    async def get_option_chain_async(self, underlying: str, date: str,
                                    underlying_price: float = 0.0) -> list[dict]:
        import math, time
        if underlying_price <= 0.0:
            stock = Stock(underlying, "SMART", "USD")
            qualified = await self.ib.qualifyContractsAsync(stock)
            contract = qualified[0] if qualified else stock
            [ticker] = await self.ib.reqTickersAsync(contract)
            underlying_price = ticker.last or ticker.close or 500.0

        stock = Stock(underlying, "SMART", "USD")
        qualified = await self.ib.qualifyContractsAsync(stock)
        stock = qualified[0] if qualified else stock

        chains = await self.ib.reqSecDefOptParamsAsync(
            stock.symbol, "", stock.secType, stock.conId
        )
        chain = next((c for c in chains if c.exchange in ("SMART", "CBOE")), None)
        if chain is None and chains:
            chain = chains[0]
        if chain is None:
            raise RuntimeError(f"No option chain found for {underlying}")

        expiry_tws = date.replace("-", "")
        if expiry_tws not in chain.expirations:
            available = sorted(chain.expirations)[:8]
            raise RuntimeError(f"Expiry {date} not available. Nearest: {available}")

        lo, hi = underlying_price * 0.80, underlying_price * 1.20
        strikes = [s for s in chain.strikes if lo <= s <= hi] or list(chain.strikes)

        contracts = [
            Option(underlying, expiry_tws, strike, right, "SMART")
            for strike in strikes
            for right in ("C", "P")
        ]

        qualified = []
        for i in range(0, len(contracts), 50):
            qualified.extend(await self.ib.qualifyContractsAsync(*contracts[i:i+50]))

        if not qualified:
            return []

        tickers = await self.ib.reqTickersAsync(*qualified)
        ts_ms = int(time.time() * 1000)
        quotes = []
        for t in tickers:
            c = t.contract
            bid = t.bid if t.bid and not math.isnan(t.bid) else 0.0
            ask = t.ask if t.ask and not math.isnan(t.ask) else 0.0
            mid = (bid + ask) / 2.0 if bid and ask else 0.0
            yr  = c.lastTradeDateOrContractMonth[2:6]
            occ = f"{c.symbol}{yr}{c.right}{int(round(c.strike*1000)):08d}"
            quotes.append({
                "raw_symbol": occ, "underlying": c.symbol,
                "expiration": date, "right": c.right, "strike": c.strike,
                "bid": round(bid,4), "ask": round(ask,4), "mid": round(mid,4),
                "bid_size": int(t.bidSize or 0), "ask_size": int(t.askSize or 0),
                "ts_ms": ts_ms,
            })
        return quotes


# ── Request dispatcher ────────────────────────────────────────────────────────

async def dispatch_async(session: IBSession, req: dict) -> dict:
    rid = req.get("id", 0)
    try:
        method = req["method"]
        if method == "underlying_trade":
            data = await session.ib.qualifyContractsAsync(
                Stock(req["symbol"], "SMART", "USD")
            )
            contract = data[0] if data else Stock(req["symbol"], "SMART", "USD")
            [ticker] = await session.ib.reqTickersAsync(contract)
            import math, time
            price = ticker.last or ticker.close or float("nan")
            return {"id": rid, "ok": True, "data": {
                "symbol": req["symbol"],
                "price": price,
                "size": int(ticker.lastSize or 0),
                "ts_ms": int(time.time() * 1000)
            }}

        elif method == "option_chain":
            data = await session.get_option_chain_async(
                underlying       = req["underlying"],
                date             = req["date"],
                underlying_price = float(req.get("underlying_price", 0.0)),
            )
            return {"id": rid, "ok": True, "data": data}

        else:
            return {"id": rid, "ok": False, "error": f"Unknown method: {method}"}

    except Exception as exc:
        log.exception("Error handling request %s", req)
        return {"id": rid, "ok": False, "error": str(exc)}

# def dispatch(session: IBSession, req: dict) -> dict:
#     rid = req.get("id", 0)
#     try:
#         method = req["method"]

#         if method == "underlying_trade":
#             data = session.get_underlying_trade(req["symbol"])
#             return {"id": rid, "ok": True, "data": data}

#         elif method == "option_chain":
#             data = session.get_option_chain(
#                 underlying       = req["underlying"],
#                 date             = req["date"],
#                 underlying_price = float(req.get("underlying_price", 0.0)),
#             )
#             return {"id": rid, "ok": True, "data": data}

#         else:
#             return {"id": rid, "ok": False, "error": f"Unknown method: {method}"}

#     except Exception as exc:
#         log.exception("Error handling request %s", req)
#         return {"id": rid, "ok": False, "error": str(exc)}


# ── TCP server ────────────────────────────────────────────────────────────────

async def handle_client(reader: asyncio.StreamReader,
                         writer: asyncio.StreamWriter,
                         session: IBSession):
    peer = writer.get_extra_info("peername")
    log.info("C++ client connected from %s", peer)
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                req = json.loads(line.decode())
            except json.JSONDecodeError as e:
                resp = {"id": 0, "ok": False, "error": f"JSON parse error: {e}"}
                writer.write((json.dumps(resp) + "\n").encode())
                await writer.drain()
                continue

            log.info("→ %s", req.get("method", "?"))
            # Run blocking IB calls in a thread pool so the event loop stays alive
            resp = await dispatch_async(session, req)
            log.info("← ok=%s  id=%d", resp.get("ok"), resp.get("id", 0))

            writer.write((json.dumps(resp) + "\n").encode())
            await writer.drain()

    except (asyncio.IncompleteReadError, ConnectionResetError):
        pass
    finally:
        log.info("C++ client disconnected from %s", peer)
        writer.close()


async def run_server(session: IBSession, host: str, port: int):
    server = await asyncio.start_server(
        lambda r, w: handle_client(r, w, session),
        host, port
    )
    addr = server.sockets[0].getsockname()
    log.info("Bridge listening on %s:%d  (waiting for C++ connections…)", *addr)
    async with server:
        await server.serve_forever()


# ── Entry point ───────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="IBKR ↔ C++ bridge")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--paper", action="store_true", default=True,
                      help="Paper trading (TWS port 7497) [default]")
    mode.add_argument("--live",  action="store_true",
                      help="Live trading (TWS port 7496) — be careful!")
    parser.add_argument("--tws-host",  default="127.0.0.1")
    parser.add_argument("--tws-port",  type=int, default=0,
                        help="Override TWS port (default: 7497 paper / 7496 live)")
    parser.add_argument("--client-id", type=int, default=10,
                        help="TWS client ID (must be unique per connection)")
    parser.add_argument("--port",      type=int, default=19999,
                        help="Bridge TCP port that C++ connects to [19999]")
    parser.add_argument("--host",      default="127.0.0.1",
                        help="Bridge listen address [127.0.0.1]")
    args = parser.parse_args()

    paper    = not args.live
    tws_port = args.tws_port or (7497 if paper else 7496)

    log.info("Mode: %s  |  TWS: %s:%d  |  Bridge: %s:%d",
             "PAPER" if paper else "LIVE",
             args.tws_host, tws_port,
             args.host, args.port)

    session = IBSession(args.tws_host, tws_port, args.client_id)
    session.connect()

    try:
        asyncio.run(run_server(session, args.host, args.port))
    except KeyboardInterrupt:
        log.info("Shutting down…")
    finally:
        session.disconnect()


if __name__ == "__main__":
    main()