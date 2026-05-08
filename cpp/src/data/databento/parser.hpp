#pragma once

#include "../models/option_quote.hpp"

#include <memory>
#include <string>
#include <vector>

namespace volarb::data::databento {

class DBNParser {
public:
    DBNParser() = default;

    //
    // Parse raw DBN file into normalized quotes.
    //
    std::vector<models::OptionQuote>
    parse_option_quotes(
        const std::string& dbn_file);

private:
    models::OptionContract
    parse_contract_symbol(
        const std::string& raw_symbol) const;

    double compute_mid(
        double bid,
        double ask) const;

    double compute_spread_bps(
        double bid,
        double ask) const;
};

} // namespace volarb::data::databento