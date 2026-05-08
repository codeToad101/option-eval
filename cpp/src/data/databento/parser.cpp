#include "parser.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>

namespace volarb::data::databento {

using namespace volarb::data::models;

std::vector<OptionQuote>
DBNParser::parse_option_quotes(
    const std::string& dbn_file) {

    //
    // ==================================================
    // PLACEHOLDER IMPLEMENTATION
    // ==================================================
    //
    // Later:
    //
    // - open DBNStore
    // - iterate DBN records
    // - detect schema type
    // - normalize records
    //
    // ==================================================
    //

    std::ifstream in(dbn_file);

    if (!in.is_open()) {
        throw std::runtime_error(
            "Failed to open DBN file");
    }

    //
    // Placeholder synthetic quote.
    //
    OptionQuote quote;

    quote.contract =
        parse_contract_symbol(
            "SPY250117C00550000");

    quote.bid_price = 5.10;
    quote.ask_price = 5.20;

    quote.bid_size = 100;
    quote.ask_size = 120;

    quote.bid_exchange = "CBOE";
    quote.ask_exchange = "CBOE";

    quote.ts_event = 1735689600000000000LL;
    quote.ts_recv  = 1735689600001000000LL;

    quote.mid_price =
        compute_mid(
            quote.bid_price,
            quote.ask_price);

    quote.spread =
        quote.ask_price -
        quote.bid_price;

    quote.spread_bps =
        compute_spread_bps(
            quote.bid_price,
            quote.ask_price);

    return { quote }; //should be converted to normalized later for safety (?) whole section experimental tbh
}

OptionContract
DBNParser::parse_contract_symbol(
    const std::string& raw_symbol) const {

    //
    // OCC option format:
    //
    // SPY250117C00550000
    //

    static const std::regex pattern(
        R"(([A-Z]+)(\d{6})([CP])(\d{8}))");

    std::smatch match;

    if (!std::regex_match(
            raw_symbol,
            match,
            pattern)) {

        throw std::runtime_error(
            "Invalid OCC option symbol: " +
            raw_symbol);
    }

    OptionContract contract;

    contract.raw_symbol = raw_symbol;

    contract.underlying =
        match[1].str();

    //
    // YYMMDD
    //
    const std::string expiry =
        match[2].str();

    contract.expiration =
        "20" + expiry.substr(0, 2) + "-" +
        expiry.substr(2, 2) + "-" +
        expiry.substr(4, 2);

    const std::string right =
        match[3].str();

    contract.right =
        (right == "C")
            ? OptionRight::Call
            : OptionRight::Put;

    //
    // Strike encoded in mills.
    //
    const std::string strike_str =
        match[4].str();

    contract.strike =
        std::stod(strike_str) / 1000.0;

    return contract;
}

double DBNParser::compute_mid(
    double bid,
    double ask) const {

    if (bid <= 0.0 || ask <= 0.0) {
        return 0.0;
    }

    return (bid + ask) * 0.5;
}

double DBNParser::compute_spread_bps(
    double bid,
    double ask) const {

    const double mid =
        compute_mid(bid, ask);

    if (mid <= 0.0) {
        return 0.0;
    }

    return ((ask - bid) / mid) * 10000.0;
}

} // namespace volarb::data::databento