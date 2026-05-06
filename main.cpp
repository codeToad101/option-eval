#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include "black_scholes.hpp"
#include "iv_solver.hpp"


// Pretty-printing helpers
static void print_separator(char c = '-', int n = 68) {
    std::cout << std::string(n, c) << '\n';
}

static void print_bs_result(const BSResult& r, const OptionParams& p) {
    const std::string type_str = (p.type == OptionType::Call) ? "Call" : "Put";

    print_separator();
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  Option type    : " << type_str             << '\n'
              << "  Spot (S)       : " << p.S                  << '\n'
              << "  Strike (K)     : " << p.K                  << '\n'
              << "  Maturity (T)   : " << p.T  << " yr"        << '\n'
              << "  Risk-free (r)  : " << p.r * 100 << " %"    << '\n'
              << "  Div yield (q)  : " << p.q * 100 << " %"    << '\n'
              << "  Vol (σ)        : " << p.sigma * 100 << " %"<< '\n'
              << "  ─── Output ───────────────────────────────\n"
              << "  Price          : " << r.price              << '\n'
              << "  Delta          : " << r.delta              << '\n'
              << "  Gamma          : " << r.gamma              << '\n'
              << "  Vega (per 1%)  : " << r.vega               << '\n'
              << "  Theta (daily)  : " << r.theta              << '\n'
              << "  Rho   (per 1%) : " << r.rho                << '\n';
    print_separator();
}

static void print_iv_result(const IVResult& iv, double mkt_price) {
    std::cout << std::fixed << std::setprecision(6);
    std::cout << "  Market price   : " << mkt_price            << '\n'
              << "  Implied vol    : " << iv.iv * 100 << " %"  << '\n'
              << "  Converged      : " << (iv.converged ? "yes" : "NO") << '\n'
              << "  Iterations     : " << iv.iterations        << '\n';
    print_separator();
}


// Demo scenarios
int main() {
    std::cout << "\n";
    print_separator('=');
    std::cout << "  Black-Scholes Pricer + IV Solver Demo\n";
    print_separator('=');

    // ── 1. Standard ATM call ──────────────────────────────────────────────
    {
        std::cout << "\n[1] ATM Call  –  price → greeks\n";
        OptionParams p{
            .S     = 100.0,
            .K     = 100.0,
            .r     = 0.05,
            .q     = 0.02,
            .sigma = 0.20,
            .T     = 1.0,
            .type  = OptionType::Call
        };
        auto res = bs_price(p);
        print_bs_result(res, p);
    }

    // ── 2. OTM put at 3-month expiry ─────────────────────────────────────
    {
        std::cout << "\n[2] OTM Put  –  price → greeks\n";
        OptionParams p{
            .S     = 100.0,
            .K     = 90.0,
            .r     = 0.05,
            .q     = 0.0,
            .sigma = 0.25,
            .T     = 0.25,
            .type  = OptionType::Put
        };
        auto res = bs_price(p);
        print_bs_result(res, p);
    }

    // ── 3. IV inversion: give market price, recover vol ───────────────────
    {
        std::cout << "\n[3] IV Solver  –  market price → implied vol\n";

        // Construct a call, price it, then pretend we see only the market price
        OptionParams p{
            .S     = 150.0,
            .K     = 155.0,
            .r     = 0.045,
            .q     = 0.01,
            .sigma = 0.0,   // irrelevant for the query direction
            .T     = 0.5,
            .type  = OptionType::Call
        };

        // "True" price generated from a known vol = 30%
        const double true_vol   = 0.30;
        OptionParams pricing_p  = p;
        pricing_p.sigma         = true_vol;
        const double mkt_price  = bs_price(pricing_p).price;

        auto iv = solve_iv(p, mkt_price);
        print_separator();
        std::cout << "  Known vol (σ_true) : " << true_vol * 100 << " %\n";
        print_iv_result(iv, mkt_price);
    }

    // ── 4. Stress test: sweep strikes, verify IV round-trips ─────────────
    {
        std::cout << "\n[4] IV round-trip across moneyness  (S=100, T=1yr, σ=20%)\n";
        print_separator('-');
        std::cout << std::setw(10) << "Strike"
                  << std::setw(14) << "BS Price"
                  << std::setw(16) << "Solved IV (%)"
                  << std::setw(12) << "Error (%)"
                  << std::setw(10) << "Iters"   << '\n';
        print_separator('-');

        const double S = 100.0, r = 0.05, q = 0.0, sigma = 0.20, T = 1.0;
        for (double K : {70.0, 80.0, 90.0, 95.0, 100.0, 105.0, 110.0, 120.0, 130.0}) {
            OptionParams p{ S, K, r, q, 0.20, T, OptionType::Call };
            const double price = bs_price(p).price;

            // Reset sigma (solver will ignore it)
            p.sigma = 0.0;
            auto iv = solve_iv(p, price);

            const double err_bps = std::abs(iv.iv - sigma) / sigma * 10000.0;
            std::cout << std::fixed << std::setprecision(2)
                      << std::setw(10) << K
                      << std::setw(14) << price
                      << std::setw(16) << iv.iv * 100.0
                      << std::setw(12) << err_bps      // should be < 0.01 bps
                      << std::setw(10) << iv.iterations << '\n';
        }
        print_separator('-');
        std::cout << "  (Error is in basis-points relative to true vol)\n";
    }

    // ── 5. Put-call parity check ──────────────────────────────────────────
    {
        std::cout << "\n[5] Put-call parity check  (C - P = S·e^{-qT} - K·e^{-rT})\n";
        OptionParams base{
            .S = 100.0, .K = 100.0, .r = 0.05, .q = 0.02,
            .sigma = 0.20, .T = 1.0
        };
        base.type = OptionType::Call;
        const double C = bs_price(base).price;
        base.type = OptionType::Put;
        const double P = bs_price(base).price;

        const double lhs     = C - P;
        const double rhs     = base.S * std::exp(-base.q * base.T)
                             - base.K * std::exp(-base.r * base.T);
        print_separator();
        std::cout << std::fixed << std::setprecision(8)
                  << "  C - P           : " << lhs << '\n'
                  << "  S·e^{-qT}-K·e^{-rT} : " << rhs << '\n'
                  << "  Difference      : " << std::abs(lhs - rhs) << "  ← should be ~0\n";
        print_separator();
    }

    std::cout << '\n';
    return 0;
}