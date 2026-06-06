// // ─────────────────────────────────────────────────────────────────────────────
// // src/main.cpp
// //
// // bs_pricer — entry point
// //
// // Usage
// // ─────
// //   ./bs_pricer                        # synthetic data (no IB Gateway needed)
// //   VOL_PROVIDER=ibkr_paper ./bs_pricer
// //   VOL_PROVIDER=ibkr_live  ./bs_pricer
// //
// // What it does right now
// // ──────────────────────
// //   1. Creates a market data provider (synthetic by default, IBKR if env set)
// //   2. Fetches today's option chain for SPY
// //   3. Prints a summary table to stdout
// //   4. Runs a simple Black-Scholes pricer on each quote and prints IV/delta
// //
// // Extend from here: add your surface builder, arb scanner, strategy logic, etc.
// // ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// src/main.cpp
//
// bs_pricer — entry point
//
// Usage
// ─────
//   ./bs_pricer                        # synthetic data (no IB Gateway needed)
//   VOL_PROVIDER=ibkr_paper ./bs_pricer
//   VOL_PROVIDER=ibkr_live  ./bs_pricer
//
// Pipeline
// ────────
//   1. Fetch spot + option chain
//   2. Build raw vol surface (IV solve per quote)
//   3. Fit SVI surface per expiry slice
//   4. Find mispricings (market IV vs SVI model IV)
//   5. Print summary
// ─────────────────────────────────────────────────────────────────────────────

#include "data/market_data.hpp"
#include "pricing/svi_surface.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::string today_str() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[11];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return {buf};
}

static std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static void print_separator(int width = 100) {
    std::cout << std::string(width, '-') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Print raw surface summary
// ─────────────────────────────────────────────────────────────────────────────

static void print_raw_surface(
    const volarb::data::models::VolatilitySurface& surf,
    int max_rows = 40
) {
    std::cout << "\n[raw surface]  " << surf.underlying
              << "  spot=$" << std::fixed << std::setprecision(2)
              << surf.underlying_price
              << "  valid=" << surf.valid_points
              << "  rejected=" << surf.rejected_points;
    if (surf.atm_iv)
        std::cout << "  ATM IV=" << std::setprecision(1)
                  << *surf.atm_iv * 100.0 << "%";
    std::cout << "\n";
    print_separator();

    std::cout << std::left
              << std::setw(22) << "Symbol"
              << std::setw(12) << "Expiry"
              << std::setw(5)  << "R"
              << std::right
              << std::setw(8)  << "Strike"
              << std::setw(8)  << "Mid"
              << std::setw(9)  << "IV%"
              << std::setw(9)  << "Delta"
              << std::setw(9)  << "LogMny"
              << std::setw(9)  << "Sprd bp"
              << "\n";
    print_separator();

    int rows = 0;
    for (const auto& pt : surf.points) {
        if (rows++ >= max_rows) {
            std::cout << "  ... (" << (surf.points.size() - max_rows)
                      << " more)\n";
            break;
        }
        std::cout << std::fixed
                  << std::left  << std::setw(22) << pt.contract.raw_symbol
                  << std::left  << std::setw(12) << pt.contract.expiration
                  << std::left  << std::setw(5)
                      << (pt.contract.right == volarb::data::models::OptionRight::Call ? "C" : "P")
                  << std::right << std::setw(8)  << std::setprecision(2) << pt.contract.strike
                  << std::right << std::setw(8)  << std::setprecision(3) << pt.mid_price
                  << std::right << std::setw(8)  << std::setprecision(1) << pt.implied_volatility * 100.0
                  << "%"
                  << std::right << std::setw(8)  << std::setprecision(3) << pt.delta
                  << std::right << std::setw(9)  << std::setprecision(3) << pt.log_moneyness
                  << std::right << std::setw(9)  << std::setprecision(0) << pt.spread_bps
                  << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Print SVI fit summary
// ─────────────────────────────────────────────────────────────────────────────

static void print_svi_surface(const volarb::pricing::SVISurface& surf) {
    std::cout << "\n[SVI surface]  " << surf.underlying
              << "  " << surf.slices.size() << " fitted slices\n";
    print_separator(80);

    std::cout << std::left  << std::setw(12) << "Expiry"
              << std::right << std::setw(7)  << "T(yr)"
              << std::right << std::setw(7)  << "N"
              << std::right << std::setw(9)  << "RMSE vp"
              << std::right << std::setw(9)  << "a"
              << std::right << std::setw(9)  << "b"
              << std::right << std::setw(9)  << "rho"
              << std::right << std::setw(9)  << "m"
              << std::right << std::setw(9)  << "sigma"
              << std::right << std::setw(7)  << "ArbFr"
              << "\n";
    print_separator(80);

    for (const auto& sl : surf.slices) {
        const auto& p = sl.params;
        std::cout << std::fixed
                  << std::left  << std::setw(12) << sl.expiration
                  << std::right << std::setw(7)  << std::setprecision(3) << sl.time_to_expiry
                  << std::right << std::setw(7)  << sl.n_points
                  << std::right << std::setw(9)  << std::setprecision(2) << sl.fit_rmse
                  << std::right << std::setw(9)  << std::setprecision(4) << p.a
                  << std::right << std::setw(9)  << std::setprecision(4) << p.b
                  << std::right << std::setw(9)  << std::setprecision(4) << p.rho
                  << std::right << std::setw(9)  << std::setprecision(4) << p.m
                  << std::right << std::setw(9)  << std::setprecision(4) << p.sigma
                  << std::right << std::setw(7)  << (sl.arb_free ? "Y" : "N")
                  << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Print mispricing signals
// ─────────────────────────────────────────────────────────────────────────────

static void print_signals(
    const std::vector<volarb::pricing::MispricingSignal>& signals,
    int max_rows = 30
) {
    std::cout << "\n[signals]  " << signals.size()
              << " mispricings found\n";
    if (signals.empty()) return;
    print_separator(100);

    std::cout << std::left  << std::setw(22) << "Symbol"
              << std::left  << std::setw(12) << "Expiry"
              << std::left  << std::setw(5)  << "R"
              << std::right << std::setw(8)  << "Strike"
              << std::right << std::setw(10) << "Mkt IV%"
              << std::right << std::setw(10) << "Mdl IV%"
              << std::right << std::setw(10) << "Diff vp"
              << std::right << std::setw(9)  << "Signal"
              << "\n";
    print_separator(100);

    int rows = 0;
    for (const auto& sig : signals) {
        if (rows++ >= max_rows) {
            std::cout << "  ... (" << (signals.size() - max_rows) << " more)\n";
            break;
        }
        const auto& pt = sig.point;
        std::cout << std::fixed
                  << std::left  << std::setw(22) << pt.contract.raw_symbol
                  << std::left  << std::setw(12) << pt.contract.expiration
                  << std::left  << std::setw(5)
                      << (pt.contract.right == volarb::data::models::OptionRight::Call ? "C" : "P")
                  << std::right << std::setw(8)  << std::setprecision(2) << pt.contract.strike
                  << std::right << std::setw(9)  << std::setprecision(1) << sig.market_iv * 100.0 << "%"
                  << std::right << std::setw(9)  << std::setprecision(1) << sig.model_iv  * 100.0 << "%"
                  << std::right << std::setw(8)  << std::setprecision(1)
                      << sig.iv_diff * 100.0 << "vp"
                  << std::right << std::setw(9)  << (sig.is_rich ? "RICH" : "CHEAP")
                  << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    using namespace volarb::data;
    using namespace volarb::pricing;

    constexpr const char* UNDERLYING = "SPY";

    // 1. Build provider
    std::unique_ptr<IMarketData> provider;
    try {
        provider = make_provider();
    } catch (const std::exception& e) {
        std::cerr << "FATAL: provider init failed: " << e.what() << "\n";
        return 1;
    }

    // 2. Fetch spot
    double spot = 0.0;
    try {
        spot = provider->underlying_trade(UNDERLYING).price;
        std::cout << "[spot]  " << UNDERLYING << " = $"
                  << std::fixed << std::setprecision(2) << spot << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[warn] spot fetch failed (" << e.what() << "), using 500\n";
        spot = 500.0;
    }

    // 3. Fetch chain
    const std::string date = today_str();
    const std::int64_t ts  = now_ms();
    std::cout << "[chain] Fetching " << UNDERLYING << " for " << date << "...\n";

    std::vector<volarb::data::models::OptionQuote> chain;
    try {
        chain = provider->option_chain(UNDERLYING, date, spot);
    } catch (const std::exception& e) {
        std::cerr << "FATAL: option_chain failed: " << e.what() << "\n";
        return 1;
    }

    if (chain.empty()) {
        std::cout << "[chain] Empty — market closed or no data for " << date << "\n";
        return 0;
    }
    std::cout << "[chain] " << chain.size() << " quotes received\n";

    // 4. Build SVI surface
    SVISurfaceConfig svi_cfg;
    svi_cfg.risk_free_rate          = 0.05;
    svi_cfg.dividend_yield          = 0.013;
    svi_cfg.signal_threshold_vol    = 0.02;   // 2 vol points
    svi_cfg.filter.max_spread_bps   = 500.0;
    svi_cfg.filter.min_option_price = 0.05;

    SVISurfaceBuilder builder(svi_cfg);
    const auto result = builder.run(chain, spot, ts, UNDERLYING);

    // 5. Print results
    print_raw_surface(result.raw);
    print_svi_surface(result.fitted);
    print_signals(result.signals);

    std::cout << "\n[done]\n";
    return 0;
}

// #include "data/market_data.hpp"

// #include <algorithm>
// #include <chrono>
// #include <cmath>
// #include <ctime>
// #include <iomanip>
// #include <iostream>
// #include <string>
// #include <vector>

// // ─────────────────────────────────────────────────────────────────────────────
// // Tiny Black-Scholes (self-contained, no external pricer dependency yet)
// // ─────────────────────────────────────────────────────────────────────────────

// namespace bs {

// static double N(double x) {
//     return 0.5 * std::erfc(-x * M_SQRT1_2);
// }

// struct Greeks {
//     double price = 0;
//     double delta = 0;
//     double gamma = 0;
//     double vega  = 0;
//     double theta = 0;
// };

// Greeks price(double S, double K, double T, double iv,
//              double r, double q,
//              volarb::data::models::OptionRight right) {
//     if (T <= 0 || iv <= 0 || S <= 0 || K <= 0) return {};
//     using R = volarb::data::models::OptionRight;

//     const double sqrtT = std::sqrt(T);
//     const double d1 = (std::log(S / K) + (r - q + 0.5 * iv * iv) * T) / (iv * sqrtT);
//     const double d2 = d1 - iv * sqrtT;
//     const double pdf_d1 = std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * M_PI);
//     const double df  = std::exp(-r * T);
//     const double dfq = std::exp(-q * T);

//     Greeks g;
//     if (right == R::Call) {
//         g.price = S * dfq * N(d1) - K * df * N(d2);
//         g.delta = dfq * N(d1);
//     } else {
//         g.price = K * df * N(-d2) - S * dfq * N(-d1);
//         g.delta = -dfq * N(-d1);
//     }
//     g.gamma = dfq * pdf_d1 / (S * iv * sqrtT);
//     g.vega  = S * dfq * pdf_d1 * sqrtT / 100.0;   // per 1 vol point
//     g.theta = (-(S * dfq * pdf_d1 * iv) / (2.0 * sqrtT)
//                - r * K * df * (right == R::Call ? N(d2)  : N(-d2))
//                + q * S * dfq * (right == R::Call ? N(d1)  : N(-d1))) / 365.0;
//     return g;
// }

// // Bisection implied vol — fast enough for batch quotes
// double implied_vol(double market_price, double S, double K, double T,
//                    double r, double q,
//                    volarb::data::models::OptionRight right,
//                    double lo = 0.001, double hi = 5.0,
//                    int    max_iter = 50, double tol = 1e-6) {
//     for (int i = 0; i < max_iter; ++i) {
//         const double mid  = (lo + hi) * 0.5;
//         const double pMid = price(S, K, T, mid, r, q, right).price;
//         if (std::abs(pMid - market_price) < tol) return mid;
//         if (pMid < market_price) lo = mid; else hi = mid;
//     }
//     return (lo + hi) * 0.5;
// }

// } // namespace bs


// // ─────────────────────────────────────────────────────────────────────────────
// // Helpers
// // ─────────────────────────────────────────────────────────────────────────────

// static std::string today_str() {
//     const std::time_t t = std::time(nullptr);
//     std::tm tm{};
//     gmtime_r(&t, &tm);
//     char buf[11];
//     std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
//                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
//     return {buf};
// }

// static std::string right_str(volarb::data::models::OptionRight r) {
//     using R = volarb::data::models::OptionRight;
//     if (r == R::Call) return "C";
//     if (r == R::Put)  return "P";
//     return "?";
// }


// // ─────────────────────────────────────────────────────────────────────────────
// // main
// // ─────────────────────────────────────────────────────────────────────────────

// int main() {
//     using namespace volarb::data;

//     constexpr const char* UNDERLYING = "SPY";
//     constexpr double      R          = 0.05;   // risk-free rate
//     constexpr double      Q          = 0.013;  // SPY dividend yield (approx)
//     constexpr int         MAX_ROWS   = 40;     // cap console output

//     // 1. Build provider — env var selects live/paper/synthetic
//     std::unique_ptr<IMarketData> provider;
//     try {
//         provider = make_provider();
//     } catch (const std::exception& e) {
//         std::cerr << "FATAL: could not create market data provider: "
//                   << e.what() << "\n";
//         return 1;
//     }

//     // 2. Fetch spot price
//     double spot = 0.0;
//     try {
//         const auto trade = provider->underlying_trade(UNDERLYING);
//         spot = trade.price;
//         std::cout << "[spot]  " << UNDERLYING << " = $"
//                   << std::fixed << std::setprecision(2) << spot << "\n\n";
//     } catch (const std::exception& e) {
//         std::cerr << "[warn] underlying_trade failed (" << e.what()
//                   << "); using synthetic spot\n";
//         spot = 500.0;
//     }

//     // 3. Fetch option chain for today
//     const std::string date = today_str();
//     std::cout << "[chain] Fetching " << UNDERLYING
//               << " options for " << date << "…\n";

//     std::vector<OptionQuote> chain;
//     try {
//         chain = provider->option_chain(UNDERLYING, date, spot);
//     } catch (const std::exception& e) {
//         std::cerr << "FATAL: option_chain failed: " << e.what() << "\n";
//         return 1;
//     }

//     if (chain.empty()) {
//         std::cout << "[chain] Empty — market closed or no data for " << date << "\n";
//         return 0;
//     }

//     std::cout << "[chain] " << chain.size() << " quotes received\n\n";

//     // 4. Sort by expiry, then strike, then right
//     std::sort(chain.begin(), chain.end(),
//               [](const OptionQuote& a, const OptionQuote& b) {
//                   if (a.contract.expiration != b.contract.expiration)
//                       return a.contract.expiration < b.contract.expiration;
//                   if (a.contract.strike != b.contract.strike)
//                       return a.contract.strike < b.contract.strike;
//                   return a.contract.right < b.contract.right;
//               });

//     // 5. Print table header
//     std::cout
//         << std::left
//         << std::setw(20) << "Symbol"
//         << std::setw(12) << "Expiry"
//         << std::setw(5)  << "R"
//         << std::setw(9)  << "Strike"
//         << std::setw(9)  << "Bid"
//         << std::setw(9)  << "Ask"
//         << std::setw(9)  << "Mid"
//         << std::setw(10) << "IV %"
//         << std::setw(9)  << "Delta"
//         << std::setw(9)  << "Spread"
//         << "\n"
//         << std::string(101, '-') << "\n";

//     // 6. Price each quote
//     int rows = 0;
//     for (const auto& q : chain) {
//         if (rows++ >= MAX_ROWS) {
//             std::cout << "  … (" << (chain.size() - MAX_ROWS) << " more rows)\n";
//             break;
//         }

//         const auto& c  = q.contract;
//         const double T = [&] {
//             // rough DTE from expiry string
//             std::tm exp{};
//             exp.tm_year = std::stoi(c.expiration.substr(0, 4)) - 1900;
//             exp.tm_mon  = std::stoi(c.expiration.substr(5, 2)) - 1;
//             exp.tm_mday = std::stoi(c.expiration.substr(8, 2));
//             const std::time_t exp_t = timegm(&exp);
//             const double days = std::difftime(exp_t, std::time(nullptr)) / 86400.0;
//             return std::max(0.0, days) / 365.0;
//         }();

//         // Implied vol from mid price
//         double iv = 0.0;
//         double delta = 0.0;
//         if (q.mid_price > 0.01 && T > 0) {
//             iv    = bs::implied_vol(q.mid_price, spot, c.strike, T, R, Q, c.right);
//             delta = bs::price(spot, c.strike, T, iv, R, Q, c.right).delta;
//         }

//         std::cout << std::fixed
//                   << std::left  << std::setw(20) << c.raw_symbol
//                   << std::left  << std::setw(12) << c.expiration
//                   << std::left  << std::setw(5)  << right_str(c.right)
//                   << std::right << std::setw(7)  << std::setprecision(2) << c.strike  << "  "
//                   << std::right << std::setw(7)  << q.bid_price                       << "  "
//                   << std::right << std::setw(7)  << q.ask_price                       << "  "
//                   << std::right << std::setw(7)  << q.mid_price                       << "  "
//                   << std::right << std::setw(7)  << std::setprecision(1) << iv * 100  << "%  "
//                   << std::right << std::setw(7)  << std::setprecision(3) << delta     << "  "
//                   << std::right << std::setw(7)  << std::setprecision(4) << q.spread
//                   << "\n";
//     }

//     std::cout << "\n[done]\n";
//     return 0;
// }