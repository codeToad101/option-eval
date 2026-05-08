#pragma once

#include "option_quote.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace volarb::data::models {

//
// Point on the implied volatility surface.
//
// One contract / strike / expiry observation.
//
struct SurfacePoint {

    //
    // Contract metadata.
    //
    OptionContract contract;

    //
    // Underlying spot price at observation.
    //
    double underlying_price = 0.0;

    //
    // Market prices.
    //
    double bid_price = 0.0;
    double ask_price = 0.0;
    double mid_price = 0.0;

    //
    // Time to expiration in years.
    //
    // IMPORTANT:
    // analytics should NEVER repeatedly recompute this.
    //
    double time_to_expiry = 0.0;

    //
    // Risk-free + dividend assumptions.
    //
    // These should remain explicit because:
    // - backtests differ
    // - live trading differs
    // - rates change over time
    //
    double risk_free_rate = 0.0;
    double dividend_yield = 0.0;

    //
    // Solved implied volatility.
    //
    double implied_volatility = 0.0;

    //
    // Greeks from solved IV.
    //
    double delta = 0.0;
    double gamma = 0.0;
    double vega  = 0.0;
    double theta = 0.0;

    //
    // Moneyness metrics.
    //
    // These are CRITICAL for surface analytics.
    //
    double log_moneyness = 0.0;
    double strike_distance_pct = 0.0;

    //
    // Quality diagnostics.
    //
    double spread = 0.0;
    double spread_bps = 0.0;

    bool iv_converged = false;

    //
    // Timestamping.
    //
    std::int64_t ts_event = 0;
    std::int64_t ts_recv  = 0;
};

//
// Full volatility surface snapshot.
//
struct VolatilitySurface {

    //
    // Underlying root.
    //
    std::string underlying;

    //
    // Observation timestamp.
    //
    std::int64_t snapshot_ts = 0;

    //
    // Spot at snapshot time.
    //
    double underlying_price = 0.0;

    //
    // Surface observations.
    //
    std::vector<SurfacePoint> points;

    //
    // Aggregated diagnostics.
    //
    std::optional<double> atm_iv;
    std::optional<double> front_month_iv;
    std::optional<double> realized_vol_20d;

    //
    // Surface-level quality metrics.
    //
    std::size_t valid_points = 0;
    std::size_t rejected_points = 0;

    //
    // Convenience helpers.
    //
    [[nodiscard]]
    bool empty() const {
        return points.empty();
    }
};

//
// Expiry slice.
//
// Useful for:
// - skew fitting
// - smile analysis
// - term structure
//
struct ExpirySlice {

    std::string expiration;

    double time_to_expiry = 0.0;

    std::vector<SurfacePoint> points;
};

//
// Term structure node.
//
// ATM IV by expiry.
//
struct TermStructurePoint {

    std::string expiration;

    double time_to_expiry = 0.0;

    double atm_iv = 0.0;
};

//
// Surface quality filters.
//
// These are EXTREMELY important.
// Most option chains are noisy.
//
struct SurfaceFilterConfig {

    //
    // Reject crossed/wide markets.
    //
    double max_spread_bps = 500.0;

    //
    // Reject tiny premium contracts.
    //
    double min_option_price = 0.05;

    //
    // Reject impossible IVs.
    //
    double min_iv = 0.01;

    double max_iv = 5.0;

    //
    // Avoid near-zero tenor instability.
    //
    double min_time_to_expiry = 1.0 / 365.0;

    //
    // Liquidity filtering.
    //
    std::uint32_t min_bid_size = 1;
    std::uint32_t min_ask_size = 1;
};

} // namespace volarb::data::models

//eventually add class to reflect on data quality for realism ++ safety