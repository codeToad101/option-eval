#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/pricing/svi_surface.hpp
//
// SVI (Stochastic Volatility Inspired) volatility surface fitter.
//
// Overview
// ────────
// Gatheral's raw SVI parameterises total implied variance w(k) per expiry
// slice as a function of log-moneyness k = ln(K/F):
//
//   w(k) = a + b * ( ρ(k − m) + sqrt((k − m)² + σ²) )
//
// where:
//   a  — overall variance level (vertical shift)
//   b  — "wings" steepness (≥ 0)
//   ρ  — skew / correlation  ∈ (−1, 1)
//   m  — ATM shift
//   σ  — smile curvature / ATM smoothness (> 0)
//
// Implied vol is recovered as:  iv(k,T) = sqrt( w(k) / T )
//
// Fitting
// ───────
// We minimise sum-of-squared errors in IV space (not variance space) because
// IV errors are more uniform across strikes.  The optimiser is a simple
// Levenberg-Marquardt style gradient descent with analytic Jacobian.
//
// No external optimisation library is required — the implementation is
// self-contained.
//
// Pipeline
// ────────
//   OptionQuote chain
//       ↓  build_surface_points()   solve IV per quote, filter noise
//   vector<SurfacePoint>
//       ↓  fit_svi_slice()          fit one expiry slice
//   SVISlice
//       ↓  SVISurface               collection of slices
//
// Usage
// ─────
//   SVISurfaceBuilder builder(config);
//   VolatilitySurface raw   = builder.build_raw_surface(chain, spot, ts);
//   SVISurface        fitted = builder.fit(raw);
//
//   // Query fitted IV at any (k, T)
//   double iv = fitted.iv_at(log_moneyness, time_to_expiry);
//
//   // Find mispriced quotes
//   auto signals = builder.find_mispricing(raw, fitted, threshold_vol_pts);
//
// ─────────────────────────────────────────────────────────────────────────────

#include "../data/models/option_quote.hpp"
#include "../data/models/vol_snapshot.hpp"
#include "iv_solver.hpp"
#include "black_scholes.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <map>

namespace volarb::pricing {

using volarb::data::models::OptionQuote;
using volarb::data::models::OptionRight;
using volarb::data::models::SurfacePoint;
using volarb::data::models::SurfaceFilterConfig;
using volarb::data::models::VolatilitySurface;
using volarb::data::models::ExpirySlice;
using volarb::data::models::TermStructurePoint;


// ─────────────────────────────────────────────────────────────────────────────
// SVI parameters for one expiry slice
// ─────────────────────────────────────────────────────────────────────────────

struct SVIParams {
    double a = 0.04;   // variance level
    double b = 0.10;   // wing steepness
    double rho = -0.3; // skew
    double m = 0.0;    // ATM shift
    double sigma = 0.2;// smile curvature

    // Evaluate total implied variance w(k)
    [[nodiscard]]
    double w(double k) const {
        const double z = k - m;
        return a + b * (rho * z + std::sqrt(z * z + sigma * sigma));
    }

    // Implied vol at log-moneyness k, time T
    [[nodiscard]]
    double iv(double k, double T) const {
        if (T <= 0.0) return std::numeric_limits<double>::quiet_NaN();
        const double wk = w(k);
        if (wk <= 0.0) return std::numeric_limits<double>::quiet_NaN();
        return std::sqrt(wk / T);
    }

    // Butterfly arbitrage check: g(k) = (1 - k*rho/sigma_eff)^2 - (rho/sigma_eff)^2/4 + w''(k)/2
    // Simplified: check b*(1+|rho|) < 4/T for a given T
    [[nodiscard]]
    bool is_arbitrage_free(double T) const {
        if (b < 0.0 || sigma <= 0.0) return false;
        if (std::abs(rho) >= 1.0)    return false;
        if (a + b * sigma * std::sqrt(1.0 - rho * rho) < 0.0) return false;
        // Roger-Lee moment formula bound
        if (T > 0.0 && b * T > 4.0)  return false;
        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// One fitted expiry slice
// ─────────────────────────────────────────────────────────────────────────────

struct SVISlice {
    std::string expiration;
    double      time_to_expiry = 0.0;
    SVIParams   params;

    std::size_t n_points   = 0;     // points used in fit
    double      fit_rmse   = 0.0;   // root-mean-square IV error (vol points)
    bool        converged  = false;
    bool        arb_free   = false;

    [[nodiscard]]
    double iv_at(double log_moneyness) const {
        return params.iv(log_moneyness, time_to_expiry);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Full fitted SVI surface (collection of slices)
// ─────────────────────────────────────────────────────────────────────────────

struct SVISurface {
    std::string  underlying;
    std::int64_t snapshot_ts      = 0;
    double       underlying_price = 0.0;

    std::vector<SVISlice> slices;   // sorted by time_to_expiry ascending

    // Interpolated IV at (log_moneyness, T) — linear in T between fitted slices
    [[nodiscard]]
    double iv_at(double log_moneyness, double T) const {
        if (slices.empty()) return std::numeric_limits<double>::quiet_NaN();

        // Below shortest expiry: return front slice
        if (T <= slices.front().time_to_expiry)
            return slices.front().iv_at(log_moneyness);

        // Above longest expiry: return back slice
        if (T >= slices.back().time_to_expiry)
            return slices.back().iv_at(log_moneyness);

        // Find surrounding slices and interpolate total variance linearly in T
        for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
            const auto& lo = slices[i];
            const auto& hi = slices[i + 1];
            if (T >= lo.time_to_expiry && T <= hi.time_to_expiry) {
                const double alpha = (T - lo.time_to_expiry)
                                   / (hi.time_to_expiry - lo.time_to_expiry);
                const double w_lo = lo.params.w(log_moneyness);
                const double w_hi = hi.params.w(log_moneyness);
                // Interpolate total variance (w = iv² * T) — calendar-spread safe
                const double w_T_lo = w_lo; // w already is total variance
                const double w_T_hi = w_hi;
                const double w_interp = (1.0 - alpha) * w_T_lo + alpha * w_T_hi;
                if (w_interp <= 0.0 || T <= 0.0)
                    return std::numeric_limits<double>::quiet_NaN();
                return std::sqrt(w_interp / T);
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    [[nodiscard]] bool empty() const { return slices.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Mispricing signal
// ─────────────────────────────────────────────────────────────────────────────

struct MispricingSignal {
    SurfacePoint point;

    double model_iv     = 0.0;   // SVI surface IV at this strike/expiry
    double market_iv    = 0.0;   // solved IV from market mid
    double iv_diff      = 0.0;   // market_iv − model_iv  (+ = rich, − = cheap)
    double iv_diff_bps  = 0.0;   // abs(iv_diff) in vol basis points (×10000)

    bool is_rich  = false;  // market IV > model (sell candidate)
    bool is_cheap = false;  // market IV < model (buy candidate)
};

// ─────────────────────────────────────────────────────────────────────────────
// Builder config
// ─────────────────────────────────────────────────────────────────────────────

struct SVISurfaceConfig {
    // IV solver
    IVSolverConfig  iv_cfg;

    // Quote filtering (delegates to vol_snapshot.hpp SurfaceFilterConfig)
    SurfaceFilterConfig filter;

    // Fitting
    double  risk_free_rate  = 0.05;
    double  dividend_yield  = 0.013;  // ~SPY

    // LM optimiser
    int     max_iter        = 500;
    double  grad_tol        = 1e-8;
    double  lambda_init     = 1e-3;
    double  lambda_up       = 10.0;
    double  lambda_dn       = 0.1;

    // Mispricing threshold
    double  signal_threshold_vol = 0.02;  // 2 vol points
};

// ─────────────────────────────────────────────────────────────────────────────
// SVISurfaceBuilder
// ─────────────────────────────────────────────────────────────────────────────

class SVISurfaceBuilder {
public:

    explicit SVISurfaceBuilder(SVISurfaceConfig cfg = {})
        : cfg_(std::move(cfg)) {}

    // ── Stage 1: raw surface ──────────────────────────────────────────────
    //
    // Converts a flat OptionQuote chain into a VolatilitySurface by:
    //   1. Computing time-to-expiry from the quote's expiration string
    //   2. Solving IV for each quote (NR + bisection fallback)
    //   3. Computing Greeks from the solved IV
    //   4. Filtering noise according to SurfaceFilterConfig
    //
    VolatilitySurface build_raw_surface(
        const std::vector<OptionQuote>& chain,
        double                          spot,
        std::int64_t                    snapshot_ts,
        const std::string&              underlying = ""
    ) const {
        VolatilitySurface surf;
        surf.underlying       = underlying.empty()
                                ? (chain.empty() ? "" : chain.front().contract.underlying)
                                : underlying;
        surf.snapshot_ts      = snapshot_ts;
        surf.underlying_price = spot;

        for (const auto& q : chain) {
            if (!q.is_valid()) {
                ++surf.rejected_points;
                continue;
            }

            // ── Filter: spread, price floor, size ─────────────────────────
            if (q.spread_bps > cfg_.filter.max_spread_bps ||
                q.mid_price   < cfg_.filter.min_option_price ||
                q.bid_size    < cfg_.filter.min_bid_size ||
                q.ask_size    < cfg_.filter.min_ask_size)
            {
                ++surf.rejected_points;
                continue;
            }

            const double T = tte_from_expiry(q.contract.expiration, snapshot_ts);
            if (T < cfg_.filter.min_time_to_expiry) {
                ++surf.rejected_points;
                continue;
            }

            // ── Solve IV ───────────────────────────────────────────────────
            OptionParams params;
            params.S     = spot;
            params.K     = q.contract.strike;
            params.T     = T;
            params.r     = cfg_.risk_free_rate;
            params.q     = cfg_.dividend_yield;
            params.sigma = 0.0; // overwritten by solver
            params.right = params.right = q.contract.right;
            // params.type  = (q.contract.right == OptionRight::Call)
            //              ? OptionType::Call
            //              : OptionType::Put;

            const IVResult ivr = solve_iv(params, q.mid_price, cfg_.iv_cfg);

            if (!ivr.converged ||
                ivr.iv < cfg_.filter.min_iv ||
                ivr.iv > cfg_.filter.max_iv)
            {
                ++surf.rejected_points;
                continue;
            }

            // ── Build SurfacePoint ─────────────────────────────────────────
            params.sigma = ivr.iv;
            BSResult greeks;
            try { greeks = bs_price(params); }
            catch (...) { ++surf.rejected_points; continue; }

            SurfacePoint pt;
            pt.contract           = q.contract;
            pt.underlying_price   = spot;
            pt.bid_price          = q.bid_price;
            pt.ask_price          = q.ask_price;
            pt.mid_price          = q.mid_price;
            pt.time_to_expiry     = T;
            pt.risk_free_rate     = cfg_.risk_free_rate;
            pt.dividend_yield     = cfg_.dividend_yield;
            pt.implied_volatility = ivr.iv;
            pt.delta              = greeks.delta;
            pt.gamma              = greeks.gamma;
            pt.vega               = greeks.vega;
            pt.theta              = greeks.theta;
            pt.log_moneyness      = std::log(q.contract.strike / spot);
            pt.strike_distance_pct = (q.contract.strike - spot) / spot * 100.0;
            pt.spread             = q.spread;
            pt.spread_bps         = q.spread_bps;
            pt.iv_converged       = ivr.converged;
            pt.ts_event           = q.ts_event;
            pt.ts_recv            = q.ts_recv;

            surf.points.push_back(std::move(pt));
            ++surf.valid_points;
        }

        // ── Compute ATM IV ────────────────────────────────────────────────
        surf.atm_iv = compute_atm_iv(surf.points);

        return surf;
    }

    // ── Stage 2: SVI fit ──────────────────────────────────────────────────
    //
    // Groups points by expiry, fits one SVIParams per slice.
    //
    SVISurface fit(const VolatilitySurface& raw) const {
        SVISurface fitted;
        fitted.underlying       = raw.underlying;
        fitted.snapshot_ts      = raw.snapshot_ts;
        fitted.underlying_price = raw.underlying_price;

        // Group by expiry
        std::map<std::string, std::vector<const SurfacePoint*>> by_expiry;
        for (const auto& pt : raw.points)
            by_expiry[pt.contract.expiration].push_back(&pt);

        for (const auto& [expiry, pts] : by_expiry) {
            if (pts.size() < 4) continue; // not enough points for a stable fit

            SVISlice slice;
            slice.expiration    = expiry;
            slice.time_to_expiry = pts.front()->time_to_expiry;
            slice.n_points       = pts.size();

            fit_svi_slice(pts, slice);
            fitted.slices.push_back(std::move(slice));
        }

        // Sort by time_to_expiry ascending
        std::sort(fitted.slices.begin(), fitted.slices.end(),
                  [](const SVISlice& a, const SVISlice& b) {
                      return a.time_to_expiry < b.time_to_expiry;
                  });

        return fitted;
    }

    // ── Stage 3: mispricing signals ───────────────────────────────────────
    //
    // For each raw surface point, compare market IV to fitted SVI IV.
    // Returns points where |market_iv - model_iv| > threshold.
    //
    std::vector<MispricingSignal> find_mispricing(
        const VolatilitySurface& raw,
        const SVISurface&        fitted,
        double                   threshold_vol = -1.0  // -1 → use cfg default
    ) const {
        const double thresh = (threshold_vol > 0.0)
                            ? threshold_vol
                            : cfg_.signal_threshold_vol;

        std::vector<MispricingSignal> signals;

        for (const auto& pt : raw.points) {
            const double model = fitted.iv_at(pt.log_moneyness, pt.time_to_expiry);
            if (std::isnan(model)) continue;

            const double diff = pt.implied_volatility - model;
            if (std::abs(diff) < thresh) continue;

            MispricingSignal sig;
            sig.point        = pt;
            sig.model_iv     = model;
            sig.market_iv    = pt.implied_volatility;
            sig.iv_diff      = diff;
            sig.iv_diff_bps  = std::abs(diff) * 10000.0;
            sig.is_rich      = diff > 0.0;
            sig.is_cheap     = diff < 0.0;
            signals.push_back(std::move(sig));
        }

        // Sort by |iv_diff| descending — biggest mispricings first
        std::sort(signals.begin(), signals.end(),
                  [](const MispricingSignal& a, const MispricingSignal& b) {
                      return std::abs(a.iv_diff) > std::abs(b.iv_diff);
                  });

        return signals;
    }

    // ── Convenience: one-shot ─────────────────────────────────────────────
    //
    // build_raw_surface → fit → find_mispricing
    //
    struct SurfaceResult {
        VolatilitySurface          raw;
        SVISurface                 fitted;
        std::vector<MispricingSignal> signals;
    };

    SurfaceResult run(
        const std::vector<OptionQuote>& chain,
        double                          spot,
        std::int64_t                    snapshot_ts,
        const std::string&              underlying = ""
    ) const {
        SurfaceResult r;
        r.raw     = build_raw_surface(chain, spot, snapshot_ts, underlying);
        r.fitted  = fit(r.raw);
        r.signals = find_mispricing(r.raw, r.fitted);
        return r;
    }

    [[nodiscard]] const SVISurfaceConfig& config() const { return cfg_; }

private:

    SVISurfaceConfig cfg_;

    // ── Time to expiry ────────────────────────────────────────────────────
    // "YYYY-MM-DD" → years from snapshot_ts (ms epoch)
    static double tte_from_expiry(const std::string& expiry,
                                   std::int64_t       snapshot_ts_ms) {
        std::tm exp_tm{};
        exp_tm.tm_year = std::stoi(expiry.substr(0, 4)) - 1900;
        exp_tm.tm_mon  = std::stoi(expiry.substr(5, 2)) - 1;
        exp_tm.tm_mday = std::stoi(expiry.substr(8, 2));
        exp_tm.tm_hour = 21; // ~market close EST in UTC
        const std::time_t exp_t = timegm(&exp_tm);
        const double now_s = static_cast<double>(snapshot_ts_ms) / 1000.0;
        return std::max(0.0, (static_cast<double>(exp_t) - now_s) / (365.25 * 86400.0));
    }

    // ── ATM IV ────────────────────────────────────────────────────────────
    static std::optional<double> compute_atm_iv(
        const std::vector<SurfacePoint>& pts
    ) {
        // Find the point with smallest |log_moneyness| in each expiry,
        // average those across front two expiries
        if (pts.empty()) return std::nullopt;

        std::map<std::string, const SurfacePoint*> atm_by_expiry;
        for (const auto& pt : pts) {
            auto it = atm_by_expiry.find(pt.contract.expiration);
            if (it == atm_by_expiry.end() ||
                std::abs(pt.log_moneyness) < std::abs(it->second->log_moneyness))
                atm_by_expiry[pt.contract.expiration] = &pt;
        }

        double sum = 0.0;
        int    cnt = 0;
        for (const auto& [_, pt] : atm_by_expiry) {
            sum += pt->implied_volatility;
            if (++cnt >= 2) break;
        }
        return cnt > 0 ? std::optional<double>(sum / cnt) : std::nullopt;
    }

    // ─────────────────────────────────────────────────────────────────────
    // SVI slice fitter — Levenberg-Marquardt
    //
    // Minimises  E(θ) = Σ (iv_model(k_i; θ) − iv_market_i)²
    // over θ = {a, b, ρ, m, σ}
    //
    // Constraints enforced by clamping after each step:
    //   b ≥ 0,  |ρ| < 1,  σ > 0
    //   a + b·σ·√(1−ρ²) ≥ 0   (no negative total variance)
    // ─────────────────────────────────────────────────────────────────────
    void fit_svi_slice(
        const std::vector<const SurfacePoint*>& pts,
        SVISlice&                                slice
    ) const {
        const double T = slice.time_to_expiry;
        const int    N = static_cast<int>(pts.size());

        // Build observation vectors
        std::vector<double> k(N), iv_mkt(N);
        for (int i = 0; i < N; ++i) {
            k[i]      = pts[i]->log_moneyness;
            iv_mkt[i] = pts[i]->implied_volatility;
        }

        // Initialise params from data
        SVIParams p = init_params(k, iv_mkt, T);

        double lambda = cfg_.lambda_init;

        auto residuals = [&](const SVIParams& q) {
            std::vector<double> r(N);
            for (int i = 0; i < N; ++i)
                r[i] = q.iv(k[i], T) - iv_mkt[i];
            return r;
        };

        auto cost = [&](const std::vector<double>& r) {
            double s = 0.0;
            for (double ri : r) s += ri * ri;
            return s;
        };

        // Analytic Jacobian: d(iv)/d(param)
        // iv = sqrt(w/T),  d(iv)/d(w) = 1/(2·iv·T)
        // d(w)/d(a) = 1
        // d(w)/d(b) = ρ(k−m) + sqrt((k−m)²+σ²)
        // d(w)/d(ρ) = b(k−m)
        // d(w)/d(m) = b(−ρ − (k−m)/sqrt((k−m)²+σ²))
        // d(w)/d(σ) = b·σ / sqrt((k−m)²+σ²)
        auto jacobian = [&](const SVIParams& q) {
            // J is N×5
            std::vector<std::array<double, 5>> J(N);
            for (int i = 0; i < N; ++i) {
                const double z     = k[i] - q.m;
                const double disc  = std::sqrt(z * z + q.sigma * q.sigma);
                const double w_val = q.w(k[i]);
                const double iv_val = q.iv(k[i], T);
                if (iv_val <= 0.0 || disc <= 0.0) {
                    J[i] = {0, 0, 0, 0, 0};
                    continue;
                }
                const double div_dw = 1.0 / (2.0 * iv_val * T);
                J[i][0] = div_dw * 1.0;
                J[i][1] = div_dw * (q.rho * z + disc);
                J[i][2] = div_dw * q.b * z;
                J[i][3] = div_dw * q.b * (-q.rho - z / disc);
                J[i][4] = div_dw * q.b * q.sigma / disc;
            }
            return J;
        };

        std::vector<double> res = residuals(p);
        double E = cost(res);

        for (int iter = 0; iter < cfg_.max_iter; ++iter) {
            const auto J = jacobian(p);

            // Build JᵀJ and Jᵀr  (5×5 normal equations)
            double JtJ[5][5] = {};
            double Jtr[5]    = {};
            for (int i = 0; i < N; ++i) {
                for (int a = 0; a < 5; ++a) {
                    Jtr[a] += J[i][a] * res[i];
                    for (int b = 0; b < 5; ++b)
                        JtJ[a][b] += J[i][a] * J[i][b];
                }
            }

            // Add LM damping: (JᵀJ + λ·diag(JᵀJ)) Δθ = −Jᵀr
            double A[5][5];
            for (int a = 0; a < 5; ++a)
                for (int b2 = 0; b2 < 5; ++b2)
                    A[a][b2] = JtJ[a][b2] + (a == b2 ? lambda * JtJ[a][a] : 0.0);

            double rhs[5];
            for (int a = 0; a < 5; ++a)
                rhs[a] = -Jtr[a];

            // Solve 5×5 system via Gaussian elimination
            double delta[5];
            if (!solve5x5(A, rhs, delta)) {
                lambda *= cfg_.lambda_up;
                continue;
            }

            // Trial step
            SVIParams p_new = p;
            p_new.a   += delta[0];
            p_new.b   += delta[1];
            p_new.rho += delta[2];
            p_new.m   += delta[3];
            p_new.sigma += delta[4];
            clamp_params(p_new);

            const auto res_new = residuals(p_new);
            const double E_new = cost(res_new);

            if (E_new < E) {
                p   = p_new;
                res = res_new;
                E   = E_new;
                lambda *= cfg_.lambda_dn;
            } else {
                lambda *= cfg_.lambda_up;
            }

            // Convergence check on gradient norm
            double gnorm = 0.0;
            for (int a = 0; a < 5; ++a) gnorm += Jtr[a] * Jtr[a];
            if (std::sqrt(gnorm) < cfg_.grad_tol) {
                slice.converged = true;
                break;
            }
        }

        slice.params   = p;
        slice.fit_rmse = std::sqrt(E / N) * 100.0;  // expressed in vol points (%)
        slice.arb_free = p.is_arbitrage_free(T);
        if (!slice.converged && E < 1e-6) slice.converged = true;
    }

    // ── Parameter initialisation from data ───────────────────────────────
    static SVIParams init_params(
        const std::vector<double>& k,
        const std::vector<double>& iv,
        double T
    ) {
        SVIParams p;

        // ATM IV estimate: minimum |k| point
        double atm_iv = iv[0];
        double min_k  = std::abs(k[0]);
        for (std::size_t i = 1; i < k.size(); ++i) {
            if (std::abs(k[i]) < min_k) {
                min_k  = std::abs(k[i]);
                atm_iv = iv[i];
            }
        }

        p.a     = atm_iv * atm_iv * T * 0.9;
        p.b     = 0.1;
        p.rho   = -0.3;
        p.m     = 0.0;
        p.sigma = atm_iv * std::sqrt(T) * 0.5;
        p.sigma = std::max(p.sigma, 0.01);
        clamp_params(p);
        return p;
    }

    // ── Enforce SVI constraints ───────────────────────────────────────────
    static void clamp_params(SVIParams& p) {
        p.b     = std::max(p.b,     0.0);
        p.rho   = std::clamp(p.rho, -0.999, 0.999);
        p.sigma = std::max(p.sigma, 1e-4);
        // Ensure a + b*sigma*sqrt(1-rho^2) >= 0
        const double floor = -p.b * p.sigma * std::sqrt(1.0 - p.rho * p.rho);
        if (p.a < floor) p.a = floor + 1e-8;
    }

    // ── 5×5 Gaussian elimination with partial pivoting ───────────────────
    static bool solve5x5(double A[5][5], double rhs[5], double x[5]) {
        // Augmented matrix [A | rhs]
        double M[5][6];
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 5; ++j) M[i][j] = A[i][j];
            M[i][5] = rhs[i];
        }

        for (int col = 0; col < 5; ++col) {
            // Partial pivot
            int pivot = col;
            for (int row = col + 1; row < 5; ++row)
                if (std::abs(M[row][col]) > std::abs(M[pivot][col]))
                    pivot = row;
            if (std::abs(M[pivot][col]) < 1e-14) return false;
            std::swap(M[col], M[pivot]);

            const double inv = 1.0 / M[col][col];
            for (int row = col + 1; row < 5; ++row) {
                const double f = M[row][col] * inv;
                for (int j = col; j <= 5; ++j)
                    M[row][j] -= f * M[col][j];
            }
        }

        // Back substitution
        for (int i = 4; i >= 0; --i) {
            x[i] = M[i][5];
            for (int j = i + 1; j < 5; ++j)
                x[i] -= M[i][j] * x[j];
            x[i] /= M[i][i];
        }
        return true;
    }
};

} // namespace volarb::pricing