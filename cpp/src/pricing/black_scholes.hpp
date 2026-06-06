// #pragma once
// #include <cmath>
// #include <stdexcept>
// #include <string>

// //Normal distribution helpers
// namespace stats {

//     // Cumulative standard normal  Φ(x)
//     inline double ncdf(double x) noexcept {
//         return 0.5 * std::erfc(-x * M_SQRT1_2);
//     }

//     // Standard normal PDF  φ(x)
//     inline double npdf(double x) noexcept {
//         constexpr double inv_sqrt_2pi = 0.3989422804014327;
//         return inv_sqrt_2pi * std::exp(-0.5 * x * x);
//     }

// } // namespace stats


// //Option contract descriptor
// enum class OptionType { Call, Put };

// struct OptionParams {
//     double S;       // spot price
//     double K;       // strike
//     double r;       // risk-free rate (continuously compounded, annualised)
//     double q;       // continuous dividend yield
//     double sigma;   // implied/model volatility (annualised)
//     double T;       // time to expiry in years
//     OptionType type;
// };


// // Black–Scholes closed-form pricer
// struct BSResult {
//     double price;
//     double delta;
//     double gamma;
//     double vega;
//     double theta;
//     double rho;
//     double d1, d2;  // expose for downstream use
// };

// inline BSResult bs_price(const OptionParams& p) {
//     if (p.T <= 0.0)
//         throw std::domain_error("bs_price: time to expiry must be positive");
//     if (p.sigma <= 0.0)
//         throw std::domain_error("bs_price: sigma must be positive");
//     if (p.S <= 0.0 || p.K <= 0.0)
//         throw std::domain_error("bs_price: S and K must be positive");

//     const double sqrt_T   = std::sqrt(p.T);
//     const double disc_r   = std::exp(-p.r * p.T);
//     const double disc_q   = std::exp(-p.q * p.T);

//     const double d1 = (std::log(p.S / p.K) + (p.r - p.q + 0.5 * p.sigma * p.sigma) * p.T)
//                       / (p.sigma * sqrt_T);
//     const double d2 = d1 - p.sigma * sqrt_T;

//     BSResult res{};
//     res.d1 = d1;
//     res.d2 = d2;

//     // ── Price ─
//     if (p.type == OptionType::Call) {
//         res.price = p.S * disc_q * stats::ncdf(d1)
//                   - p.K * disc_r * stats::ncdf(d2);
//     } else {
//         res.price = p.K * disc_r * stats::ncdf(-d2)
//                   - p.S * disc_q * stats::ncdf(-d1);
//     }

//     // ── Greeks ───
//     const double phi_d1 = stats::npdf(d1);

//     // Delta: ∂V/∂S
//     if (p.type == OptionType::Call)
//         res.delta = disc_q * stats::ncdf(d1);
//     else
//         res.delta = -disc_q * stats::ncdf(-d1);

//     // Gamma: ∂²V/∂S² (same for calls and puts)
//     res.gamma = (disc_q * phi_d1) / (p.S * p.sigma * sqrt_T);

//     // Vega: ∂V/∂σ  (returned per 1% move in vol, i.e. divided by 100)
//     res.vega = p.S * disc_q * phi_d1 * sqrt_T / 100.0;

//     // Theta: ∂V/∂T  (returned as daily decay, i.e. divided by 365)
//     const double base_theta =
//         - (p.S * disc_q * phi_d1 * p.sigma) / (2.0 * sqrt_T)
//         - p.r * p.K * disc_r * (p.type == OptionType::Call
//               ? stats::ncdf(d2) : -stats::ncdf(-d2))
//         + p.q * p.S * disc_q * (p.type == OptionType::Call
//               ? stats::ncdf(d1) : -stats::ncdf(-d1));
//     res.theta = base_theta / 365.0;

//     // Rho: ∂V/∂r  (returned per 1% move in rates)
//     if (p.type == OptionType::Call)
//         res.rho = p.K * p.T * disc_r * stats::ncdf(d2) / 100.0;
//     else
//         res.rho = -p.K * p.T * disc_r * stats::ncdf(-d2) / 100.0;

//     return res;
// }

#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pricing/black_scholes.hpp
//
// Black-Scholes closed-form pricer + Greeks.
//
// Type alignment
// ──────────────
// Uses volarb::data::models::OptionRight (Call/Put/Unknown) exclusively.
// The old OptionType enum is gone — do not reintroduce it.
//
// Vega / Theta conventions (unchanged, documented here explicitly):
//   vega  — per 1% move in vol   (divide annualised vega by 100)
//   theta — per calendar day     (divide annualised theta by 365)
//   rho   — per 1% move in rates (divide annualised rho by 100)
// ─────────────────────────────────────────────────────────────────────────────

#include "src/data/models/option_quote.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Normal distribution helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace volarb::stats {

// Cumulative standard normal Φ(x)
inline double ncdf(double x) noexcept {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

// Standard normal PDF φ(x)
inline double npdf(double x) noexcept {
    constexpr double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

} // namespace volarb::stats


// ─────────────────────────────────────────────────────────────────────────────
// OptionParams — inputs to the BS pricer
// ─────────────────────────────────────────────────────────────────────────────

namespace volarb::pricing {

using volarb::data::models::OptionRight;

struct OptionParams {
    double      S;       // spot price
    double      K;       // strike
    double      r;       // risk-free rate (continuously compounded, annualised)
    double      q;       // continuous dividend yield
    double      sigma;   // implied/model volatility (annualised)
    double      T;       // time to expiry in years
    OptionRight right;   // Call / Put  (Unknown → throws)
};

struct BSResult {
    double price;
    double delta;
    double gamma;
    double vega;    // per 1% vol move
    double theta;   // per calendar day
    double rho;     // per 1% rate move
    double d1;
    double d2;
};

// ─────────────────────────────────────────────────────────────────────────────
// bs_price  —  main pricer
// ─────────────────────────────────────────────────────────────────────────────

inline BSResult bs_price(const OptionParams& p) {
    if (p.right == OptionRight::Unknown)
        throw std::invalid_argument("bs_price: OptionRight::Unknown is not valid");
    if (p.T <= 0.0)
        throw std::domain_error("bs_price: time to expiry must be positive");
    if (p.sigma <= 0.0)
        throw std::domain_error("bs_price: sigma must be positive");
    if (p.S <= 0.0 || p.K <= 0.0)
        throw std::domain_error("bs_price: S and K must be positive");

    const bool is_call = (p.right == OptionRight::Call);

    const double sqrt_T = std::sqrt(p.T);
    const double disc_r = std::exp(-p.r * p.T);
    const double disc_q = std::exp(-p.q * p.T);

    const double d1 =
        (std::log(p.S / p.K) + (p.r - p.q + 0.5 * p.sigma * p.sigma) * p.T)
        / (p.sigma * sqrt_T);
    const double d2 = d1 - p.sigma * sqrt_T;

    BSResult res{};
    res.d1 = d1;
    res.d2 = d2;

    // ── Price ──────────────────────────────────────────────────────────────
    if (is_call) {
        res.price = p.S * disc_q * volarb::stats::ncdf(d1)
                  - p.K * disc_r * volarb::stats::ncdf(d2);
    } else {
        res.price = p.K * disc_r * volarb::stats::ncdf(-d2)
                  - p.S * disc_q * volarb::stats::ncdf(-d1);
    }

    const double phi_d1 = volarb::stats::npdf(d1);

    // ── Delta ──────────────────────────────────────────────────────────────
    res.delta = is_call
        ?  disc_q * volarb::stats::ncdf(d1)
        : -disc_q * volarb::stats::ncdf(-d1);

    // ── Gamma  (call == put) ───────────────────────────────────────────────
    res.gamma = (disc_q * phi_d1) / (p.S * p.sigma * sqrt_T);

    // ── Vega  (per 1% vol move) ───────────────────────────────────────────
    res.vega = p.S * disc_q * phi_d1 * sqrt_T / 100.0;

    // ── Theta  (per calendar day) ─────────────────────────────────────────
    const double base_theta =
        -(p.S * disc_q * phi_d1 * p.sigma) / (2.0 * sqrt_T)
        - p.r * p.K * disc_r
          * (is_call ? volarb::stats::ncdf(d2) : -volarb::stats::ncdf(-d2))
        + p.q * p.S * disc_q
          * (is_call ? volarb::stats::ncdf(d1) : -volarb::stats::ncdf(-d1));
    res.theta = base_theta / 365.0;

    // ── Rho  (per 1% rate move) ───────────────────────────────────────────
    res.rho = is_call
        ?  p.K * p.T * disc_r * volarb::stats::ncdf(d2)  / 100.0
        : -p.K * p.T * disc_r * volarb::stats::ncdf(-d2) / 100.0;

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// Intrinsic value floor — used by IV solver to gate validity
// ─────────────────────────────────────────────────────────────────────────────

inline double bs_intrinsic(const OptionParams& p) noexcept {
    const double disc_r = std::exp(-p.r * p.T);
    const double disc_q = std::exp(-p.q * p.T);
    if (p.right == OptionRight::Call)
        return std::max(0.0, p.S * disc_q - p.K * disc_r);
    else
        return std::max(0.0, p.K * disc_r - p.S * disc_q);
}

} // namespace volarb::pricing