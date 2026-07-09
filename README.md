# Volatility Arbitrage Research System

A modular C++20 research framework for studying equity options volatility
dynamics and model-implied mispricing. Implements a Black-Scholes-Merton
pricing engine, a hybrid Newton-Raphson/bisection implied volatility solver,
and a Levenberg-Marquardt SVI (Stochastic Volatility Inspired) surface
fitter, with an automated synthetic validation harness.

## What this is (and isn't)

This is a **pricing and surface-analytics engine**, validated end-to-end
against **synthetic data with known ground truth** — not a backtested
trading strategy. Every accuracy number below comes from generating option
chains with known parameters, running them through the real pipeline, and
measuring recovery error against those known values. No live or historical
market data was used to evaluate profitability. See [Validation](#validation)
for exactly what was and wasn't tested.

**Provider abstraction:** `IMarketData` is a single interface with
hot-swappable backends — synthetic data for research/CI, a cached wrapper
for any provider, and a live IBKR paper/live trading path. Everything above
the data layer (pricer, solver, surface fitter) is provider-agnostic.

## Pipeline

```
Option chain (IMarketData)
    ↓
IV solve per quote          (iv_solver.hpp: Newton-Raphson → bisection)
    ↓
Raw volatility surface       (svi_surface.hpp: build_raw_surface)
    ↓
SVI fit per expiry slice     (svi_surface.hpp: fit — Levenberg-Marquardt)
    ↓
Mispricing signals           (svi_surface.hpp: find_mispricing —
                               market IV vs. model IV)
```

## Build & run

```bash
mkdir -p build && cd build
cmake ..
cmake --build . --target bs_pricer         # live pipeline (synthetic by default)
cmake --build . --target validate_synthetic # validation harness
./bs_pricer
./validate_synthetic
```

`bs_pricer` runs against `SyntheticProvider` by default (no external
dependency, no API key). Set `VOL_PROVIDER=ibkr_paper` or `ibkr_live` to use
the IBKR path once it's implemented.

## Validation

`validate_synthetic` generates option chains from **known SVI parameters**,
runs them through the real pipeline (not a separate test-only code path),
and measures recovery error against ground truth. Five stages:

1. **IV solver recovery** — solved IV vs. true IV per contract, exercised
   through the production `build_raw_surface` call path.
2. **SVI fit recovery** — fitted `{a, b, ρ, m, σ}` vs. the true parameters
   used to generate the chain, across 4 distinct surface shapes (flat/calm,
   steep skew, high curvature, low-vol regime) and 4 expiry slices each.
3. **Zero-noise signal check** — confirms `find_mispricing` stays quiet when
   market IV equals model IV by construction.
4. **Degenerate-case handling** — deep ITM/OTM and short-tenor contracts,
   where price-based IV inversion is fundamentally ill-conditioned near
   zero vega.
5. **Injected mispricing sweep** — deliberately shifts known contracts by a
   known IV offset and measures true-positive/false-positive detection
   across offset magnitudes.

### Results

| Metric | Result |
|---|---|
| SVI parameter recovery (zero noise) | within 0.006 across all 4 regimes |
| Non-convergent fits | 0 / 8 scenarios |
| Mispricing detection | 100% detected, 0 false positives, at ≥3 vol-point offsets |
| Detection threshold | consistent under both clean and realistic bid-ask noise |

Full per-scenario output (including known solver limitations at near-zero
vega) is reproducible by running `validate_synthetic` directly.

## Roadmap

- [ ] IBKR live/paper data provider (currently stubbed)
- [ ] True-positive/false-positive testing under smooth (non-outlier)
      background noise, where the current detector under-detects
- [ ] Historical backtesting once a real market data source is integrated

## Disclaimer

Research and educational code. Not investment advice. Pricing and surface
analytics have been validated against synthetic ground truth only — no
claims are made about performance on live or historical market data.