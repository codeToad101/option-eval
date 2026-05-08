#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace volarb::data::models {

enum class OptionRight {
    Call,
    Put,
    Unknown
};

enum class QuoteCondition {
    Regular,
    Halted,
    Wide,
    FastMarket,
    Unknown
};

struct OptionContract {
    //
    // Canonical identifier.
    //
    // Example:
    // SPY250117C00550000
    //
    std::string raw_symbol;

    //
    // Underlying root.
    //
    // Example:
    // SPY
    //
    std::string underlying;

    //
    // Expiration as YYYY-MM-DD.
    //
    std::string expiration;

    OptionRight right = OptionRight::Unknown;

    //
    // Strike in dollars.
    //
    double strike = 0.0;
};

struct OptionQuote {
    OptionContract contract;

    //
    // Best bid/ask.
    //
    double bid_price = 0.0;
    double ask_price = 0.0;

    //
    // Sizes/contracts.
    //
    std::uint32_t bid_size = 0;
    std::uint32_t ask_size = 0;

    //
    // Exchange identifiers.
    //
    std::string bid_exchange;
    std::string ask_exchange;

    //
    // Nanosecond timestamps.
    //
    std::int64_t ts_event = 0;
    std::int64_t ts_recv = 0;

    //
    // Computed midpoint.
    //
    double mid_price = 0.0;

    //
    // Spread diagnostics.
    //
    double spread = 0.0;
    double spread_bps = 0.0;

    QuoteCondition condition =
        QuoteCondition::Regular;

    //
    // Optional analytics fields.
    //
    std::optional<double> implied_volatility;
    std::optional<double> delta;
    std::optional<double> gamma;
    std::optional<double> vega;
    std::optional<double> theta;

    //
    // Validation helper.
    //
    [[nodiscard]]
    bool is_valid() const {
        return bid_price > 0.0 &&
               ask_price > 0.0 &&
               ask_price >= bid_price;
    }
};

struct UnderlyingTrade {
    std::string symbol;

    double price = 0.0;
    std::uint64_t size = 0;

    std::int64_t ts_event = 0;
};

} // namespace volarb::data::models