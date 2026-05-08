Initial Notes:

Data pipeline upgrade plan --> WRDS + OptionMetrics thru CMU, VPN needed

black_scholes.hpp -->
- Black-Scholes-Merton (extension for dividend yields)
- Greeks -->
    - Delta: ∂C/∂S, hedge ratio AKA how many units of the underlying you need to hold to be delta-neutral
    - Gamma: ∂²C/∂S², delta rate of cange
    - Vega: ∂C/∂σ (normalized, PnL change rep.)
    - Theta: ∂C/∂T, time decay rep. (per year)

iv_solver.hpp -->
Newton-Raphson primary:
- initial fast convergence algo, initial at Manaster and Koehler (1982) — "The Calculation of Implied Variances from the Black-Scholes Model"
- seed Brenner-Subrahmanyam: key promotion to convergence  from Brenner and Subrahmanyam (1988) — "A Simple Formula to Compute the Implied Standard Deviation"
    - NOTE TO REVISE: latest (which adds account for moneyness) by Corrado and Miller (1996) — "A Note on a Simple, Accurate Formula to Compute Implied Standard Deviations"
--> fallback exists for convergence difficulties (divergence, computation restraints, etc) or inaccuraries (can happen for significant ITM or OTM options)
- bisection fallback as it is guaranteed to converge w/ predictable speed, tradeoff is more iterations
- more on IV solver design -->
    - Li (2006) — "You Don't Have to Bother Newton for Implied Volatility" in Applied Mathematics Letters
    - Jäckel (2015) — "Let's Be Rational" in Wilmott Magazine
