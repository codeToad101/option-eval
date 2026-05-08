#include "client.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace volarb::data::databento {

//
// UsageGuard
//

UsageGuard::UsageGuard(double warning_limit_usd,
                       double hard_limit_usd)
    : warning_limit_(warning_limit_usd),
      hard_limit_(hard_limit_usd),
      spent_usd_(0.0) {}

bool UsageGuard::approve(double estimated_cost) const {
    return (spent_usd_ + estimated_cost) <= hard_limit_;
}

void UsageGuard::record_spend(double actual_cost) {
    spent_usd_ += actual_cost;

    if (spent_usd_ >= warning_limit_) {
        std::cerr << "[WARNING] Databento spend approaching limit: $"
                  << spent_usd_ << '\n';
    }
}

double UsageGuard::spent() const {
    return spent_usd_;
}

double UsageGuard::remaining() const {
    return hard_limit_ - spent_usd_;
}

//
// DatabentoClient
//

DatabentoClient::DatabentoClient(std::string api_key)
    : api_key_(std::move(api_key)) {

    if (api_key_.empty()) {
        throw std::runtime_error(
            "Databento API key cannot be empty");
    }
}

DatabentoClient::~DatabentoClient() = default;

void DatabentoClient::set_usage_guard(
    std::shared_ptr<UsageGuard> guard) {

    usage_guard_ = std::move(guard);
}

QueryEstimate DatabentoClient::estimate_cost(
    const HistoricalRequest& request) const {

    validate_request(request);

    QueryEstimate estimate;

    //
    // Placeholder heuristic.
    //
    // Replace later with:
    // - schema-aware estimation
    // - symbol-count scaling
    // - Databento metadata queries
    //

    const std::size_t symbol_count =
        request.symbols.size();

    if (request.schema == "ohlcv-1m") {
        estimate.estimated_cost_usd =
            0.02 * static_cast<double>(symbol_count);

        estimate.estimated_rows =
            390 * symbol_count;
    }
    else if (request.schema == "mbp-1") {
        estimate.estimated_cost_usd =
            1.50 * static_cast<double>(symbol_count);

        estimate.estimated_rows =
            1'000'000 * symbol_count;
    }
    else {
        estimate.estimated_cost_usd =
            0.25 * static_cast<double>(symbol_count);

        estimate.estimated_rows =
            50'000 * symbol_count;
    }

    return estimate;
}

DownloadResult DatabentoClient::download_historical(
    const HistoricalRequest& request,
    const std::string& output_file) {

    validate_request(request);

    if (file_exists(output_file)) {
        std::cout << "[INFO] File already cached: "
                  << output_file << '\n';

        return {
            true,
            output_file,
            fs::file_size(output_file),
            0.0
        };
    }

    const QueryEstimate estimate =
        estimate_cost(request);

    if (usage_guard_ &&
        !usage_guard_->approve(
            estimate.estimated_cost_usd)) {

        throw std::runtime_error(
            "Query rejected: Databento "
            "hard spending limit exceeded");
    }

    //
    // ==================================================
    // PLACEHOLDER IMPLEMENTATION
    // ==================================================
    //
    // Real implementation later:
    //
    // 1. Create databento::Historical client
    // 2. Submit query
    // 3. Stream DBN data
    // 4. Persist to disk
    //
    // ==================================================
    //

    fs::create_directories(
        fs::path(output_file).parent_path());

    std::ofstream out(output_file,
                      std::ios::binary);

    if (!out.is_open()) {
        throw std::runtime_error(
            "Failed to create output file");
    }

    out << "placeholder dbn data";

    out.close();

    const double simulated_cost =
        estimate.estimated_cost_usd;

    if (usage_guard_) {
        usage_guard_->record_spend(
            simulated_cost);
    }

    DownloadResult result;

    result.success = true;
    result.output_path = output_file;
    result.bytes_written =
        fs::file_size(output_file);
    result.actual_cost_usd =
        simulated_cost;

    return result;
}

void DatabentoClient::validate_request(
    const HistoricalRequest& request) const {

    if (request.dataset.empty()) {
        throw std::runtime_error(
            "Dataset cannot be empty");
    }

    if (request.schema.empty()) {
        throw std::runtime_error(
            "Schema cannot be empty");
    }

    if (request.symbols.empty()) {
        throw std::runtime_error(
            "At least one symbol required");
    }

    if (request.start.empty() ||
        request.end.empty()) {

        throw std::runtime_error(
            "Start/end timestamps required");
    }
}

bool DatabentoClient::file_exists(
    const std::string& path) const {

    return fs::exists(path);
}

} // namespace volarb::data::databento