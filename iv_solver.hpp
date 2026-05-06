#pragma once
#include "black_scholes.hpp"
#include <optional>
#include <cmath>
#include <algorithm>
#include <limits>
 
// ─────────────────────────────────────────────────────────────────────────────
// Implied Volatility solver
//
// Strategy: Newton-Raphson with vega as the Jacobian.
//   σ_{n+1} = σ_n − (BS(σ_n) − market_price) / vega(σ_n)
//
// We fall back to bisection only when Newton diverges (vega ~ 0 near ATM
// at very short tenors, or deep ITM/OTM where vega is essentially zero).
// ─────────────────────────────────────────────────────────────────────────────
 
struct IVResult {
    double iv;           // solved implied vol (annualised)
    int    iterations;   // number of Newton-Raphson steps taken
    bool   converged;    // false if we hit the iteration cap
};
 
struct IVSolverConfig {
    double tol           = 1e-8;   // price convergence tolerance
    int    max_iter_nr   = 50;     // Newton-Raphson iteration cap
    int    max_iter_bis  = 200;    // bisection fallback iteration cap
    double sigma_lo      = 1e-6;   // lower bracket  (≈ 0)
    double sigma_hi      = 10.0;   // upper bracket  (1000% vol)
    double min_vega      = 1e-10;  // below this we skip NR step
};
 
// Internal: model price for a given sigma, everything else fixed
inline double _bs_price_for_sigma(const OptionParams& base, double sigma) {
    OptionParams p = base;
    p.sigma = sigma;
    return bs_price(p).price;
}

// Intrinsic value floor — market price must exceed this for IV to exist
inline double intrinsic(const OptionParams& p) {
    const double disc = std::exp(-p.r * p.T);
    if (p.type == OptionType::Call)
        return std::max(0.0, p.S * std::exp(-p.q * p.T) - p.K * disc);
    else
        return std::max(0.0, p.K * disc - p.S * std::exp(-p.q * p.T));
}
 
// Main solver entry point
inline IVResult solve_iv(
    const OptionParams&  base_params,   // pass sigma = anything; it's overwritten --> safety ?
    double               market_price,
    const IVSolverConfig cfg = {})
{
    // Sanity: market price must be above intrinsic
    if (market_price < intrinsic(base_params) - cfg.tol)
        return { std::numeric_limits<double>::quiet_NaN(), 0, false };
 
    // ── Newton-Raphson ────────────────────────────────────────────────────
    // Seed: use Brenner-Subrahmanyam approximation for ATM as a warm start
    //   σ₀ ≈ sqrt(2π/T) · (market_price / S)
    double sigma = std::sqrt(2.0 * M_PI / base_params.T)
                 * (market_price / base_params.S);
    sigma = std::clamp(sigma, cfg.sigma_lo + 1e-4, cfg.sigma_hi - 1e-4);
 
    int iter = 0;
    for (; iter < cfg.max_iter_nr; ++iter) {
        OptionParams p = base_params;
        p.sigma = sigma;
        const BSResult res = bs_price(p);
 
        const double diff = res.price - market_price;
        if (std::abs(diff) < cfg.tol)
            return { sigma, iter + 1, true };
 
        // vega is returned per 1% move, so multiply back by 100 to get per unit
        const double vega_unit = res.vega * 100.0;
        if (std::abs(vega_unit) < cfg.min_vega)
            break; // degenerate vega → fall through to bisection
 
        const double step = diff / vega_unit;
        sigma -= step;
 
        // Clamp to valid range to prevent divergence on bad seeds
        sigma = std::clamp(sigma, cfg.sigma_lo, cfg.sigma_hi);
    }
 
    // ── Bisection fallback ──────> more research needed on security
    double lo = cfg.sigma_lo;
    double hi = cfg.sigma_hi;
 
    double f_lo = _bs_price_for_sigma(base_params, lo) - market_price;
    double f_hi = _bs_price_for_sigma(base_params, hi) - market_price;
 
    if (f_lo * f_hi > 0.0)
        return { std::numeric_limits<double>::quiet_NaN(), iter, false };
 
    for (int b = 0; b < cfg.max_iter_bis; ++b) {
        ++iter;
        double mid = 0.5 * (lo + hi);
        double f_mid = _bs_price_for_sigma(base_params, mid) - market_price;
 
        if (std::abs(f_mid) < cfg.tol)
            return { mid, iter, true };
 
        if (f_lo * f_mid < 0.0)
            hi = mid;
        else {
            lo  = mid;
            f_lo = f_mid;
        }
    }
 
    // Return best estimate even if not fully converged
    return { 0.5 * (lo + hi), iter, false };
}
 