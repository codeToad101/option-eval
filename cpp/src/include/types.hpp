#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// include/types.hpp
//
// Canonical data types for the vol-research pipeline.
//
// Design rules
// ─────────────────
// • Plain structs with value semantics — no inheritance, no virtuals.
// • All monetary values in USD, vols in annualised decimal (0.20 = 20%).
// • Dates as std::chrono::year_month_day; times as Unix ms int64.
// • Optional<> for fields that are computed downstream (IV, greeks).
//   Raw ingestion leaves them as nullopt; analytics layer fills them.
// ─────────────────────────────────────────────────────────────────────────────
 
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <cmath>
 
using Date      = std::chrono::year_month_day;
using Timestamp = int64_t;   // Unix milliseconds
 
 
// ─────────────────────────────────────────────────────────────────────────────
// Date helpers
// ─────────────────────────────────────────────────────────────────────────────
 
// Days between two dates (always non-negative if a <= b)
inline int days_between(const Date& a, const Date& b) {
    using namespace std::chrono;
    return static_cast<int>(
        (sys_days{b} - sys_days{a}).count()
    );
}
 
// Convert to years (calendar-day basis, consistent with BSM)
inline double dte_to_years(int dte) noexcept {
    return static_cast<double>(dte) / 365.0;
}
 
// Parse "YYYY-MM-DD" from Databento expiration strings
inline Date parse_date(const std::string& s) {
    // s expected as "YYYY-MM-DD" (10 chars)
    int y = std::stoi(s.substr(0, 4));
    int m = std::stoi(s.substr(5, 2));
    int d = std::stoi(s.substr(8, 2));
    return Date{
        std::chrono::year{y},
        std::chrono::month{static_cast<unsigned>(m)},
        std::chrono::day{static_cast<unsigned>(d)}
    };
}
 
// Format Date as "YYYY-MM-DD"
inline std::string format_date(const Date& d) {
    auto y = static_cast<int>(d.year());
    auto m = static_cast<unsigned>(d.month());
    auto day = static_cast<unsigned>(d.day());
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, day);
    return {buf};
}
 
 
// ─────────────────────────────────────────────────────────────────────────────
// Enumerations
// ─────────────────────────────────────────────────────────────────────────────
 
enum class OptionRight : char { Call = 'C', Put = 'P' };
 
inline OptionRight parse_right(char c) {
    return (c == 'C' || c == 'c') ? OptionRight::Call : OptionRight::Put;
}
 
inline char right_char(OptionRight r) noexcept {
    return r == OptionRight::Call ? 'C' : 'P';
}
 
 
// ─────────────────────────────────────────────────────────────────────────────
// EquityBar  —  one OHLCV day bar
// ─────────────────────────────────────────────────────────────────────────────
// Usage:
//   • Realised vol estimation (Yang-Zhang, Parkinson)
//   • Forward price construction  F = S·e^{(r-q)T}
//   • Delta-hedge PnL simulation in backtest
 
struct EquityBar {
    std::string ticker;
    Date        date;
    double      open;
    double      high;
    double      low;
    double      close;
    int64_t     volume;
 
    double mid()  const noexcept { return (high + low) * 0.5; }
    double range() const noexcept { return high - low; }
};
 
 
// ─────────────────────────────────────────────────────────────────────────────
// OptionQuote  —  one EOD options quote
// ─────────────────────────────────────────────────────────────────────────────
// implied_vol and greeks are optional:  nullopt until analytics fills them.
 
struct OptionQuote {
    // Identity
    std::string  ticker;
    Date         expiry;
    double       strike;
    OptionRight  right;
 
    // Market data
    double       bid;
    double       ask;
    double       last;
    int64_t      volume;
    int64_t      open_interest;
 
    // Underlying at quote time (needed for moneyness / IV)
    double       underlying_price;
    Date         quote_date;
 
    // Computed by analytics layer
    std::optional<double> implied_vol;
    std::optional<double> delta;
    std::optional<double> gamma;
    std::optional<double> vega;
    std::optional<double> theta;
 
    // ── Derived ────────────────────────────────────────────────────────────
 
    double mid()   const noexcept { return (bid + ask) * 0.5; }
    double spread() const noexcept { return ask - bid; }
 
    double spread_pct() const noexcept {
        double m = mid();
        return (m > 0) ? spread() / m : std::numeric_limits<double>::infinity();
    }
 
    int dte() const noexcept {
        return days_between(quote_date, expiry);
    }
 
    double tte() const noexcept {          // time-to-expiry in years
        return dte_to_years(dte());
    }
 
    double moneyness() const noexcept {    // K/S
        return (underlying_price > 0) ? strike / underlying_price : std::numeric_limits<double>::quiet_NaN();
    }
 
    double log_moneyness() const noexcept { // ln(K/S)
        double m = moneyness();
        return std::isnan(m) ? std::numeric_limits<double>::quiet_NaN() : std::log(m);
    }
 
    bool is_liquid(int min_oi = 100, double max_spread_pct = 0.20) const noexcept {
        return open_interest >= min_oi
            && spread_pct()  <= max_spread_pct
            && bid           >  0.0;
    }
};
 
 
// ─────────────────────────────────────────────────────────────────────────────
// IVPoint  —  one point on the fitted IV surface
// ─────────────────────────────────────────────────────────────────────────────
// Surface is parameterised as (log_moneyness, tte) → iv.
// Vega weight used for OI/vega-weighted calibration in Heston (Part 3).
 
struct IVPoint {
    std::string  ticker;
    Date         quote_date;
    Date         expiry;
    double       strike;
    OptionRight  right;
    double       tte;            // years
    double       log_moneyness;  // ln(K/S)
    double       iv;             // annualised implied vol
    double       mid_price;
    int64_t      open_interest;
    double       vega_weight;    // dollar vega per 1% move
};
 
 
// ─────────────────────────────────────────────────────────────────────────────
// RealisedVol  —  one RV estimate over a lookback window
// ─────────────────────────────────────────────────────────────────────────────
 
enum class RVEstimator { CloseToClose, Parkinson, YangZhang };
 
struct RealisedVol {
    std::string  ticker;
    Date         date;
    double       rv;             // annualised
    int          lookback_days;
    RVEstimator  estimator;
};
 
 
// ─────────────────────────────────────────────────────────────────────────────
// MispricingSignal  —  model vs market comparison (Part 3/4 output)
// ─────────────────────────────────────────────────────────────────────────────
 
enum class SignalDir { Buy, Sell, Flat };
 
struct MispricingSignal {
    std::string ticker;
    Date        quote_date;
    Date        expiry;
    double      strike;
    OptionRight right;
 
    double      market_iv;
    double      model_iv;
 
    double iv_diff()     const noexcept { return market_iv - model_iv; }
    double iv_diff_pct() const noexcept {
        return (model_iv > 0) ? iv_diff() / model_iv
                              : std::numeric_limits<double>::quiet_NaN();
    }
 
    SignalDir direction(double threshold = 0.02) const noexcept {
        if (iv_diff() >  threshold) return SignalDir::Sell;
        if (iv_diff() < -threshold) return SignalDir::Buy;
        return SignalDir::Flat;
    }
};