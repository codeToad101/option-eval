// ─────────────────────────────────────────────────────────────────────────────
// validate_synthetic.cpp
//
// Synthetic validation harness. NOT backtesting — this tests whether the
// IV solver and SVI fitter correctly recover KNOWN ground-truth parameters,
// which is a prerequisite for trusting them on real data.
//
// Stage 1: IV solver recovery      — generate prices from known sigma,
//                                     confirm solve_iv() recovers it.
// Stage 2: SVI fit recovery        — generate a full chain from a known
//                                     SVIParams set, confirm fit_svi_slice()
//                                     (via SVISurfaceBuilder::fit) recovers
//                                     the same params.
// Stage 3: Zero-noise mispricing   — with no injected noise, find_mispricing
//                                     should return ~no signals (market IV
//                                     == model IV by construction).
// Stage 4: Controlled-noise stress — inject known bid/ask noise, confirm
//                                     signals scale with it and degenerate
//                                     cases (deep ITM/OTM, short tenor)
//                                     degrade gracefully (converged=false,
//                                     not garbage).
//
// Build this as a separate target from main.cpp — it doesn't touch any
// live/paper data provider.
// ─────────────────────────────────────────────────────────────────────────────

#include "src/data/models/option_quote.hpp"
#include "src/data/models/vol_snapshot.hpp"
#include "src/pricing/black_scholes.hpp"
#include "src/pricing/iv_solver.hpp"
#include "src/pricing/svi_surface.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace volarb::data::models;
using namespace volarb::pricing;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Unix-ms for midnight UTC of a "YYYY-MM-DD" string. Used as the snapshot
// timestamp so time-to-expiry is computed relative to the FICTITIOUS
// synthetic date, not real wall-clock time (using now_ms() here was the
// bug: it made every synthetic 2024 expiry look like it was in the past
// relative to actual today, so every point failed the min_time_to_expiry
// filter and got rejected).
static std::int64_t date_to_ms(const std::string& s) {
    std::tm tm{};
    tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(s.substr(8, 2));
    return static_cast<std::int64_t>(timegm(&tm)) * 1000LL;
}

static std::string advance_date(const std::string& s, int days) {
    std::tm tm{};
    tm.tm_year = std::stoi(s.substr(0, 4)) - 1900;
    tm.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(s.substr(8, 2)) + days;
    std::time_t t = timegm(&tm);
    gmtime_r(&t, &tm);
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return {buf};
}

static std::string make_occ_symbol(const std::string& root, const std::string& expiry,
                                    OptionRight right, double strike) {
    char buf[32];
    const char cp = (right == OptionRight::Call) ? 'C' : 'P';
    const int  k  = static_cast<int>(std::round(strike * 1000.0));
    std::snprintf(buf, sizeof(buf), "%s%s%s%c%08d",
                  root.c_str(), expiry.substr(2, 2).c_str(),
                  expiry.substr(5, 2).c_str(), cp, k);
    return {buf};
}

// ─────────────────────────────────────────────────────────────────────────────
// Ground-truth generator: builds an OptionQuote chain FROM a known SVIParams
// set (not the linear-skew approximation SyntheticProvider uses — that
// distinction matters, since only SVI-consistent data lets us check whether
// fit_svi_slice recovers the exact input params).
// ─────────────────────────────────────────────────────────────────────────────

struct GroundTruthPoint {
    OptionQuote quote;
    double      true_iv;
    double      time_to_expiry;
};

static std::vector<GroundTruthPoint> generate_svi_chain(
    const std::string&              underlying,
    double                           spot,
    const std::string&               snapshot_date,
    const std::vector<int>&          dtes,        // days to expiry per slice
    const SVIParams&                 true_params,
    double                            r,
    double                            q,
    double                            half_spread_frac,  // e.g. 0.0 for zero-noise
    unsigned                          seed = 7
) {
    std::vector<GroundTruthPoint> out;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> noise(-1.0, 1.0);

    const std::int64_t ts = date_to_ms(snapshot_date);

    for (int dte : dtes) {
        const std::string expiry = advance_date(snapshot_date, dte);
        const double T = static_cast<double>(dte) / 365.0;

        // Strike grid: spot * {0.80 .. 1.20} in 2.5% steps, matching your
        // existing SyntheticProvider grid so results are comparable.
        for (int i = -8; i <= 8; ++i) {
            const double K  = spot * (1.0 + i * 0.025);
            const double k  = std::log(K / spot);
            const double true_iv = true_params.iv(k, T);
            if (!(true_iv > 0.0) || std::isnan(true_iv)) continue;

            for (auto right : { OptionRight::Call, OptionRight::Put }) {
                OptionParams p;
                p.S = spot; p.K = K; p.r = r; p.q = q;
                p.sigma = true_iv; p.T = T; p.right = right;

                BSResult res;
                try { res = bs_price(p); }
                catch (...) { continue; }

                const double mid = res.price;
                if (mid <= 0.0) continue;

                const double spread = std::max(0.01, mid * half_spread_frac * 2.0);
                const double jitter = half_spread_frac > 0.0
                                     ? noise(rng) * spread * 0.1 : 0.0;

                OptionContract c;
                c.underlying = underlying;
                c.expiration = expiry;
                c.right      = right;
                c.strike     = K;
                c.raw_symbol = make_occ_symbol(underlying, expiry, right, K);

                OptionQuote oq;
                oq.contract   = c;
                oq.bid_price  = std::max(0.01, mid - spread / 2.0 + jitter);
                oq.ask_price  = mid + spread / 2.0 + jitter;
                oq.mid_price  = (oq.bid_price + oq.ask_price) / 2.0;
                oq.spread     = oq.ask_price - oq.bid_price;
                oq.spread_bps = (mid > 0) ? (oq.spread / mid) * 10000.0 : 0.0;
                oq.bid_size   = 10;
                oq.ask_size   = 10;
                oq.ts_event   = ts;
                oq.ts_recv    = ts;

                out.push_back({ oq, true_iv, T });
            }
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1 (IV solver recovery) is intentionally NOT a standalone function.
// It's exercised inside stage2_and_3() below by diffing build_raw_surface()'s
// solved IVs against the true_iv ground truth per contract. This is the
// correct way to test it: it exercises solve_iv() through the exact call
// path production code uses (build_raw_surface -> solve_iv), rather than
// calling solve_iv() in isolation with hand-built OptionParams that could
// drift from what the real pipeline actually constructs.
// ─────────────────────────────────────────────────────────────────────────────
// Stage 2 + 3: SVI fit recovery, run through the REAL pipeline
// (build_raw_surface -> fit -> find_mispricing), diffed against ground truth.
// ─────────────────────────────────────────────────────────────────────────────

struct ScenarioResult {
    std::string label;
    int    n_points        = 0;
    int    n_rejected       = 0;
    double mean_iv_err      = 0.0;
    double max_iv_err       = 0.0;
    double max_param_err    = 0.0;   // worst |fit - true| across a,b,rho,m,sigma
    std::size_t n_signals   = 0;
    bool   all_converged    = true;
};

static ScenarioResult stage2_and_3(
    const std::string&       underlying,
    double                    spot,
    const std::string&        date,
    const std::vector<int>&   dtes,
    const SVIParams&          true_params,
    double                    half_spread_frac,
    const std::string&        label
) {
    std::cout << "\n=== " << label << " ===\n";

    auto gt = generate_svi_chain(underlying, spot, date, dtes,
                                  true_params, 0.05, 0.013, half_spread_frac);

    // Map raw_symbol -> true_iv for lookup after the real pipeline runs
    std::map<std::string, double> true_iv_by_symbol;
    std::vector<OptionQuote> chain;
    chain.reserve(gt.size());
    for (const auto& g : gt) {
        true_iv_by_symbol[g.quote.contract.raw_symbol] = g.true_iv;
        chain.push_back(g.quote);
    }

    SVISurfaceConfig cfg;
    cfg.risk_free_rate = 0.05;
    cfg.dividend_yield = 0.013;
    SVISurfaceBuilder builder(cfg);

    const auto result = builder.run(chain, spot, date_to_ms(date), underlying);

    // ── Stage 1 (proper): IV solver recovery, diffed via the real pipeline
    double max_iv_err = 0.0, sum_iv_err = 0.0;
    int n = 0;
    for (const auto& pt : result.raw.points) {
        auto it = true_iv_by_symbol.find(pt.contract.raw_symbol);
        if (it == true_iv_by_symbol.end()) continue;
        const double err = std::abs(pt.implied_volatility - it->second);
        max_iv_err = std::max(max_iv_err, err);
        sum_iv_err += err;
        ++n;
    }
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "[IV solver]  n=" << n
              << "  mean_err=" << (n ? sum_iv_err / n : 0.0)
              << "  max_err=" << max_iv_err
              << "  rejected=" << result.raw.rejected_points << "\n";

    // ── Stage 2: SVI fit param recovery
    std::cout << "[SVI fit]  true: a=" << true_params.a << " b=" << true_params.b
              << " rho=" << true_params.rho << " m=" << true_params.m
              << " sigma=" << true_params.sigma << "\n";

    ScenarioResult sr;
    sr.label       = label;
    sr.n_points    = n;
    sr.n_rejected  = static_cast<int>(result.raw.rejected_points);
    sr.mean_iv_err = n ? sum_iv_err / n : 0.0;
    sr.max_iv_err  = max_iv_err;
    sr.n_signals   = result.signals.size();

    for (const auto& slice : result.fitted.slices) {
        const auto& p = slice.params;
        const double da   = std::abs(p.a - true_params.a);
        const double db   = std::abs(p.b - true_params.b);
        const double drho = std::abs(p.rho - true_params.rho);
        const double dm   = std::abs(p.m - true_params.m);
        const double dsig = std::abs(p.sigma - true_params.sigma);
        sr.max_param_err = std::max({sr.max_param_err, da, db, drho, dm, dsig});
        sr.all_converged = sr.all_converged && slice.converged;

        std::cout << "  expiry=" << slice.expiration
                  << "  fit: a=" << p.a << " b=" << p.b
                  << " rho=" << p.rho << " m=" << p.m << " sigma=" << p.sigma
                  << "  |da|=" << da << " |db|=" << db << " |drho|=" << drho
                  << "  rmse_vol_pts=" << slice.fit_rmse
                  << "  converged=" << slice.converged
                  << "  arb_free=" << slice.arb_free << "\n";
    }

    // ── Stage 3: mispricing signal count (should be ~0 at zero noise)
    std::cout << "[signals]  count=" << result.signals.size()
              << "  (expect ~0 at zero noise, scaling up with injected noise)\n";

    return sr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 4: degenerate cases — deep ITM/OTM, short tenor
// ─────────────────────────────────────────────────────────────────────────────

static void stage4_degenerate_cases() {
    std::cout << "\n=== STAGE 4: degenerate case handling ===\n";

    struct Case { std::string name; double S, K, T, sigma; OptionRight right; };
    std::vector<Case> cases = {
        {"deep ITM call, 1y",   500, 100, 1.0, 0.20, OptionRight::Call},
        {"deep OTM call, 1y",   500, 2000, 1.0, 0.20, OptionRight::Call},
        {"ATM, 1 day",          500, 500, 1.0/365.0, 0.20, OptionRight::Call},
        {"deep OTM put, 1 wk",  500, 100, 7.0/365.0, 0.20, OptionRight::Put},
    };

    for (const auto& c : cases) {
        OptionParams p;
        p.S = c.S; p.K = c.K; p.T = c.T; p.sigma = c.sigma;
        p.r = 0.05; p.q = 0.013; p.right = c.right;

        double true_price;
        try { true_price = bs_price(p).price; }
        catch (const std::exception& e) {
            std::cout << "  [" << c.name << "] price failed: " << e.what() << "\n";
            continue;
        }

        const auto ivr = solve_iv(p, true_price);
        std::cout << "  [" << c.name << "]  true_sigma=" << c.sigma
                  << "  recovered=" << ivr.iv
                  << "  converged=" << ivr.converged
                  << "  iters=" << ivr.iterations << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 5: injected mispricing — true-positive / false-positive detection
//
// Takes a baseline synthetic surface, deliberately shifts a small known
// subset of contracts by a known IV offset, runs the REAL pipeline, and
// checks:
//   (a) were the perturbed contracts flagged (true positive)?
//   (b) did untouched contracts stay quiet (false positive)?
// Swept across offset magnitudes to find the actual detection threshold.
// ─────────────────────────────────────────────────────────────────────────────

struct InjectionResult {
    double offset_vol_pts;
    int    n_injected;
    int    n_detected;      // injected contracts that appeared in signals
    int    n_false_positive; // non-injected contracts that appeared in signals
    int    n_total_points;
};

static InjectionResult run_injection_test(
    const std::string&      underlying,
    double                   spot,
    const std::string&       date,
    const std::vector<int>&  dtes,
    const SVIParams&         true_params,
    double                    background_noise_frac,
    double                    injected_iv_offset,   // e.g. +0.03 = 3 vol pts rich
    double                    r,
    double                    q
) {
    auto gt = generate_svi_chain(underlying, spot, date, dtes,
                                  true_params, r, q, background_noise_frac);

    // Pick 3 target points per expiry slice worth injecting into: near-ATM
    // call, a moderately OTM call, a moderately OTM put. Selecting by
    // approximate log-moneyness rather than a fixed index so it's robust
    // to strike-grid changes.
    std::vector<std::size_t> targets;
    std::map<std::string, int> expiry_hits; // cap 1 ATM/OTM-call/OTM-put per expiry
    for (std::size_t i = 0; i < gt.size(); ++i) {
        const auto& c = gt[i].quote.contract;
        const double lm = std::log(c.strike / spot);
        const bool is_atm    = std::abs(lm) < 0.02 && c.right == OptionRight::Call;
        const bool is_otm_c  = lm > 0.08 && lm < 0.12 && c.right == OptionRight::Call;
        const bool is_otm_p  = lm < -0.08 && lm > -0.12 && c.right == OptionRight::Put;
        if (!(is_atm || is_otm_c || is_otm_p)) continue;

        const std::string key = c.expiration + (is_atm ? "atm" : is_otm_c ? "otmc" : "otmp");
        if (expiry_hits[key]++ > 0) continue; // one per expiry per bucket
        targets.push_back(i);
    }

    std::map<std::string, double> injected_symbols; // raw_symbol -> offset applied
    for (auto idx : targets) {
        auto& g = gt[idx];
        auto& c = g.quote.contract;

        // Reprice this single contract at true_iv + offset, preserving the
        // same relative bid/ask spread fraction it already had.
        const double shifted_iv = std::max(0.01, g.true_iv + injected_iv_offset);

        OptionParams p;
        p.S = spot; p.K = c.strike; p.r = r; p.q = q;
        p.sigma = shifted_iv; p.T = g.time_to_expiry; p.right = c.right;

        double new_mid;
        try { new_mid = bs_price(p).price; }
        catch (...) { continue; }
        if (new_mid <= 0.0) continue;

        const double old_mid = g.quote.mid_price;
        const double old_spread = g.quote.spread;
        const double spread_frac = (old_mid > 0) ? old_spread / old_mid : 0.02;
        const double new_spread = std::max(0.01, new_mid * spread_frac);

        g.quote.bid_price  = std::max(0.01, new_mid - new_spread / 2.0);
        g.quote.ask_price  = new_mid + new_spread / 2.0;
        g.quote.mid_price  = (g.quote.bid_price + g.quote.ask_price) / 2.0;
        g.quote.spread     = g.quote.ask_price - g.quote.bid_price;
        g.quote.spread_bps = (new_mid > 0) ? (g.quote.spread / new_mid) * 10000.0 : 0.0;

        injected_symbols[c.raw_symbol] = injected_iv_offset;
    }

    std::vector<OptionQuote> chain;
    chain.reserve(gt.size());
    for (const auto& g : gt) chain.push_back(g.quote);

    SVISurfaceConfig cfg;
    cfg.risk_free_rate = r;
    cfg.dividend_yield = q;
    SVISurfaceBuilder builder(cfg);
    const auto result = builder.run(chain, spot, date_to_ms(date), underlying);

    InjectionResult ir;
    ir.offset_vol_pts  = injected_iv_offset * 100.0;
    ir.n_injected      = static_cast<int>(injected_symbols.size());
    ir.n_total_points  = static_cast<int>(chain.size());
    ir.n_detected      = 0;
    ir.n_false_positive = 0;

    for (const auto& sig : result.signals) {
        const auto& sym = sig.point.contract.raw_symbol;
        if (injected_symbols.count(sym)) ++ir.n_detected;
        else ++ir.n_false_positive;
    }

    return ir;
}

static void stage5_injection_sweep() {
    std::cout << "\n=== STAGE 5: injected mispricing detection sweep ===\n";

    const std::string underlying = "SPY";
    const double spot = 500.0;
    const std::string date = "2024-01-02";
    const std::vector<int> dtes = {14, 30, 60, 90};
    SVIParams steep_skew{0.030, 0.12, -0.35, 0.0, 0.15};

    std::vector<double> offsets = {0.005, 0.01, 0.02, 0.03, 0.05, 0.08};

    std::cout << std::left << std::setw(14) << "bg_noise"
              << std::right << std::setw(12) << "offset(vp)"
              << std::setw(10) << "injected"
              << std::setw(10) << "detected"
              << std::setw(10) << "false_pos"
              << std::setw(12) << "total_pts" << "\n";

    for (double bg_noise : { 0.0, 0.015 }) {
        for (double off : offsets) {
            auto r = run_injection_test(underlying, spot, date, dtes, steep_skew,
                                         bg_noise, off, 0.05, 0.013);
            std::cout << std::left << std::setw(14) << (bg_noise == 0.0 ? "zero" : "realistic")
                      << std::right << std::setw(12) << r.offset_vol_pts
                      << std::setw(10) << r.n_injected
                      << std::setw(10) << r.n_detected
                      << std::setw(10) << r.n_false_positive
                      << std::setw(12) << r.n_total_points << "\n";
        }
    }

    std::cout << "\n[note] detected/injected gives true-positive rate at each\n"
                 "offset; false_pos should stay ~0. The offset where detected\n"
                 "first reaches injected consistently is your practical\n"
                 "detection threshold under that noise regime.\n";
}



int main() {
    const std::string underlying = "SPY";
    const double spot = 500.0;
    const std::string date = "2024-01-02";
    const std::vector<int> dtes = {14, 30, 60, 90};  // 4 expiry slices, not 2

    // Four SVI configurations spanning distinct surface shapes: flat/calm,
    // steep negative skew (equity-like crash-fear smirk), high curvature
    // (choppy/short-dated), and a low-vol regime. Testing only one shape
    // risks the fitter looking good by coincidence on that shape alone.
    struct NamedParams { std::string name; SVIParams p; };
    std::vector<NamedParams> scenarios = {
        {"flat_calm",        SVIParams{0.020, 0.08, -0.10, 0.0, 0.20}},
        {"steep_skew",       SVIParams{0.030, 0.12, -0.35, 0.0, 0.15}},
        {"high_curvature",   SVIParams{0.015, 0.20, -0.25, 0.0, 0.08}},
        {"low_vol_regime",   SVIParams{0.005, 0.06, -0.15, 0.0, 0.12}},
    };

    std::vector<ScenarioResult> all_results;

    for (const auto& sc : scenarios) {
        all_results.push_back(
            stage2_and_3(underlying, spot, date, dtes, sc.p, /*half_spread_frac=*/0.0,
                         "ZERO-NOISE: " + sc.name));
        all_results.push_back(
            stage2_and_3(underlying, spot, date, dtes, sc.p, /*half_spread_frac=*/0.015,
                         "REALISTIC-NOISE: " + sc.name));
    }

    stage4_degenerate_cases();
    stage5_injection_sweep();

    // ── Aggregate summary — this is the table worth screenshotting ────────
    std::cout << "\n=== SUMMARY (" << all_results.size() << " scenario runs) ===\n";
    std::cout << std::left << std::setw(28) << "scenario"
              << std::right << std::setw(8)  << "n_pts"
              << std::setw(9)  << "rej"
              << std::setw(12) << "mean_iv_err"
              << std::setw(11) << "max_iv_err"
              << std::setw(13) << "max_param_err"
              << std::setw(10) << "signals"
              << std::setw(11) << "converged" << "\n";
    double worst_iv_err = 0.0, worst_param_err = 0.0;
    bool any_nonconverged = false;
    for (const auto& r : all_results) {
        std::cout << std::left << std::setw(28) << r.label
                  << std::right << std::setw(8)  << r.n_points
                  << std::setw(9)  << r.n_rejected
                  << std::setw(12) << r.mean_iv_err
                  << std::setw(11) << r.max_iv_err
                  << std::setw(13) << r.max_param_err
                  << std::setw(10) << r.n_signals
                  << std::setw(11) << (r.all_converged ? "yes" : "NO") << "\n";
        worst_iv_err    = std::max(worst_iv_err, r.max_iv_err);
        worst_param_err = std::max(worst_param_err, r.max_param_err);
        any_nonconverged = any_nonconverged || !r.all_converged;
    }
    std::cout << "\nworst max_iv_err across all scenarios:    " << worst_iv_err << "\n";
    std::cout << "worst max_param_err across all scenarios: " << worst_param_err << "\n";
    std::cout << "any non-converged fit:                     "
              << (any_nonconverged ? "YES - investigate" : "no") << "\n";

    std::cout << "\n[done] This table is your validation evidence. Screenshot it\n"
                 "and cite the worst-case numbers, not the best-case ones.\n";
    return 0;
}