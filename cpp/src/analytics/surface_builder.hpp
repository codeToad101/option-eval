#pragma once

#include "../data/models/option_quote.hpp"
#include "../data/models/vol_snapshot.hpp"
#include "../pricing/black_scholes.hpp"
#include "../pricing/iv_solver.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace volarb::analytics {

using namespace volarb::data::models;

//
// Underlying reference snapshot.
//
// IMPORTANT:
//
// Surface construction should NEVER repeatedly
// query underlying prices contract-by-contract.
//
// Build once.
// Reuse everywhere.
//
struct UnderlyingSnapshot {

    std::string symbol;

    double spot_price = 0.0;

    std::int64_t ts_event = 0;
};

//
// Surface construction inputs.
//
struct SurfaceBuilderConfig {

    //
    // IV solver configuration.
    //
    IVSolverConfig iv_solver_config;

    //
    // Surface quality controls.
    //
    SurfaceFilterConfig filter_config;

    //
    // Default rates.
    //
    // Later:
    // replace with yield curve provider.
    //
    double default_risk_free_rate = 0.04;
    double default_dividend_yield = 0.0;

    //
    // If true:
    // reject quotes with failed IV convergence.
    //
    bool reject_failed_iv = true;

    //
    // Midpoint pricing mode.
    //
    // Later:
    // add bid/ask side IV estimation.
    //
    bool use_mid_price = true;

    //
    // Reject stale quotes.
    //
    std::int64_t max_quote_age_ns =
        5LL * 1000LL * 1000LL * 1000LL;

    //
    // Reject crossed markets.
    //
    bool reject_crossed_markets = true;

    //
    // Minimum underlying spot.
    //
    double min_underlying_price = 1.0;
};

//
// Diagnostics from a surface build.
//
struct SurfaceBuildDiagnostics {

    std::size_t total_quotes = 0;

    std::size_t accepted_quotes = 0;

    std::size_t rejected_quotes = 0;

    //
    // Rejection breakdown.
    //
    std::size_t rejected_invalid_market = 0;
    std::size_t rejected_wide_market = 0;
    std::size_t rejected_low_premium = 0;
    std::size_t rejected_bad_expiry = 0;
    std::size_t rejected_failed_iv = 0;
    std::size_t rejected_bad_iv = 0;
    std::size_t rejected_stale = 0;
    std::size_t rejected_missing_underlying = 0;
};

//
// Main surface construction engine.
//
class SurfaceBuilder {
public:

    explicit SurfaceBuilder(
        SurfaceBuilderConfig config = {});

    //
    // Build full surface from normalized quotes.
    //
    VolatilitySurface build_surface(
        const std::string& underlying,
        const std::vector<OptionQuote>& quotes,
        const UnderlyingSnapshot& underlying_snapshot,
        std::int64_t snapshot_ts);

    //
    // Access diagnostics from latest build.
    //
    [[nodiscard]]
    const SurfaceBuildDiagnostics&
    diagnostics() const;

private:

    SurfaceBuilderConfig config_;

    SurfaceBuildDiagnostics diagnostics_;

    //
    // Per-contract transformation.
    //
    std::optional<SurfacePoint>
    build_surface_point(
        const OptionQuote& quote,
        const UnderlyingSnapshot& underlying_snapshot,
        std::int64_t snapshot_ts);

    //
    // Validation layer.
    //
    bool passes_filters(
        const OptionQuote& quote,
        double option_price,
        double time_to_expiry) const;

    //
    // Utility helpers.
    //
    double compute_time_to_expiry(
        const std::string& expiration,
        std::int64_t snapshot_ts) const;

    double compute_log_moneyness(
        double strike,
        double spot) const;

    double compute_strike_distance_pct(
        double strike,
        double spot) const;

    std::optional<double>
    compute_atm_iv(
        const std::vector<SurfacePoint>& points) const;
};

} // namespace volarb::analytics