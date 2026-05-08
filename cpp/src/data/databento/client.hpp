#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace volarb::data::databento {

struct HistoricalRequest {
    std::string dataset;      // e.g. "OPRA.PILLAR"
    std::string schema;       // e.g. "mbp-1", "ohlcv-1m"
    std::vector<std::string> symbols;

    std::string start;        // ISO8601
    std::string end;          // ISO8601

    std::optional<int64_t> limit;
};

struct QueryEstimate {
    double estimated_cost_usd = 0.0;
    std::size_t estimated_rows = 0;
};

struct DownloadResult {
    bool success = false;

    std::string output_path;
    std::size_t bytes_written = 0;
    double actual_cost_usd = 0.0;
};

class UsageGuard {
public:
    UsageGuard(double warning_limit_usd,
               double hard_limit_usd);

    bool approve(double estimated_cost) const;

    void record_spend(double actual_cost);

    double spent() const;
    double remaining() const;

private:
    double warning_limit_;
    double hard_limit_;
    double spent_usd_;
};

class DatabentoClient {
public:
    explicit DatabentoClient(std::string api_key);

    ~DatabentoClient();

    DatabentoClient(const DatabentoClient&) = delete;
    DatabentoClient& operator=(const DatabentoClient&) = delete;

    void set_usage_guard(std::shared_ptr<UsageGuard> guard);

    QueryEstimate estimate_cost(
        const HistoricalRequest& request) const;

    DownloadResult download_historical(
        const HistoricalRequest& request,
        const std::string& output_file);

private:
    std::string api_key_;

    std::shared_ptr<UsageGuard> usage_guard_;

    void validate_request(
        const HistoricalRequest& request) const;

    bool file_exists(
        const std::string& path) const;
};

} // namespace volarb::data::databento