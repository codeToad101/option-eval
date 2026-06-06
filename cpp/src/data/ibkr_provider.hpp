#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// src/data/ibkr_provider.hpp
//
// IBKRProvider — replaces the stub in market_data.hpp.
//
// This implementation does NOT use the TWS C++ SDK at all.
// Instead it talks to ibkr_bridge.py over a local TCP socket using a simple
// newline-delimited JSON protocol.  The Python bridge handles all TWS/IB
// Gateway interaction via ib_insync.
//
// Setup
// ─────
//   1. pip install ib_insync
//   2. Start IB Gateway (paper port 7497 or live port 7496)
//   3. python ibkr_bridge.py --paper        # starts bridge on localhost:19999
//   4. Build your C++ project — no TWS SDK, no protobuf, no libbid needed.
//
// The bridge protocol
// ───────────────────
//   Request  (us → bridge):  newline-terminated JSON object
//   Response (bridge → us):  newline-terminated JSON object
//
//   option_chain request:
//     { "id":1, "method":"option_chain",
//       "underlying":"SPY", "date":"2025-01-15", "underlying_price":0.0 }
//
//   underlying_trade request:
//     { "id":2, "method":"underlying_trade", "symbol":"SPY" }
//
// Dependencies
// ────────────
//   nlohmann/json  (header-only, already pulled in by most modern C++ projects)
//   POSIX sockets  (macOS / Linux — no Winsock needed unless you target Windows)
//
// Add to CMakeLists.txt (instead of all the TWS/protobuf/absl mess):
//
//   find_package(nlohmann_json REQUIRED)   # brew install nlohmann-json
//
//   add_executable(bs_pricer src/pricing/main.cpp)
//
//   target_include_directories(bs_pricer PRIVATE ${CMAKE_SOURCE_DIR}/src)
//
//   target_link_libraries(bs_pricer PRIVATE nlohmann_json::nlohmann_json)
//
// ─────────────────────────────────────────────────────────────────────────────

#include "models/option_quote.hpp"
#include "models/vol_snapshot.hpp"

#include <iostream>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

namespace volarb::data {

using models::OptionQuote;
using models::OptionContract;
using models::OptionRight;
using models::UnderlyingTrade;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// BridgeSocket — thin RAII wrapper around a blocking TCP socket
// ─────────────────────────────────────────────────────────────────────────────

class BridgeSocket {
public:

    struct Config {
        std::string host       = "127.0.0.1";
        int         port       = 19999;           // must match --port in ibkr_bridge.py
        int         timeout_s  = 30;
    };

    explicit BridgeSocket(Config cfg) : cfg_(std::move(cfg)) {
        connect();
    }

    ~BridgeSocket() {
        if (fd_ >= 0) ::close(fd_);
    }

    // Non-copyable, movable
    BridgeSocket(const BridgeSocket&) = delete;
    BridgeSocket& operator=(const BridgeSocket&) = delete;
    BridgeSocket(BridgeSocket&& o) noexcept : fd_(o.fd_), cfg_(std::move(o.cfg_)) { o.fd_ = -1; }

    // Send one JSON request, read one JSON response.
    json rpc(json req) {
        req["id"] = ++next_id_;

        const std::string msg = req.dump() + "\n";
        if (::write(fd_, msg.data(), msg.size()) < 0)
            throw std::runtime_error("BridgeSocket: write failed: " + std::string(strerror(errno)));

        return read_line();
    }

private:
    int           fd_      = -1;
    Config        cfg_;
    std::atomic<int> next_id_{0};
    std::string   buf_;         // partial line buffer

    void connect() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("BridgeSocket: socket() failed");

        // Set send/recv timeouts
        struct timeval tv{ cfg_.timeout_s, 0 };
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(cfg_.port));
        if (::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) <= 0)
            throw std::runtime_error("BridgeSocket: invalid host: " + cfg_.host);

        if (::connect(fd_,
                      reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) < 0)
        {
            throw std::runtime_error(
                "BridgeSocket: cannot connect to " + cfg_.host + ":" +
                std::to_string(cfg_.port) +
                " — is ibkr_bridge.py running?\n"
                "  python ibkr_bridge.py --paper"
            );
        }
    }

    json read_line() {
        // Read bytes until we find a newline, buffering the rest
        while (true) {
            const auto nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return json::parse(line);
            }
            char tmp[4096];
            const ssize_t n = ::read(fd_, tmp, sizeof(tmp));
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    throw std::runtime_error("BridgeSocket: response timeout");
                throw std::runtime_error("BridgeSocket: connection closed by bridge");
            }
            buf_.append(tmp, static_cast<size_t>(n));
        }
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// IBKRProvider — IMarketData backed by ibkr_bridge.py
// ─────────────────────────────────────────────────────────────────────────────

class IBKRProvider final : public IMarketData {
public:

    struct Config {
        std::string bridge_host = "127.0.0.1";
        int         bridge_port = 19999;
        int         timeout_s   = 30;
        bool        paper       = true;   // informational only; actual mode set in bridge
    };

    explicit IBKRProvider(Config cfg)
        : cfg_(std::move(cfg))
        , sock_({ cfg_.bridge_host, cfg_.bridge_port, cfg_.timeout_s })
    {
        std::cerr << "[IBKRProvider] Connected to bridge at "
                  << cfg_.bridge_host << ":" << cfg_.bridge_port
                  << "  (mode: " << (cfg_.paper ? "PAPER" : "LIVE") << ")\n";
    }

    // ── IMarketData ──────────────────────────────────────────────────────

    std::vector<OptionQuote> option_chain(
        const std::string& underlying,
        const std::string& date,
        double             underlying_price = 0.0
    ) override {
        const json resp = sock_.rpc({
            { "method",           "option_chain" },
            { "underlying",       underlying      },
            { "date",             date            },
            { "underlying_price", underlying_price },
        });

        check_ok(resp);

        std::vector<OptionQuote> quotes;
        for (const auto& item : resp.at("data")) {
            OptionContract c;
            c.raw_symbol  = item.at("raw_symbol").get<std::string>();
            c.underlying  = item.at("underlying").get<std::string>();
            c.expiration  = item.at("expiration").get<std::string>();
            c.strike      = item.at("strike").get<double>();
            c.right       = parse_right(item.at("right").get<std::string>());

            OptionQuote q;
            q.contract    = c;
            q.bid_price   = item.value("bid",      0.0);
            q.ask_price   = item.value("ask",      0.0);
            q.mid_price   = item.value("mid",      0.0);
            q.spread      = q.ask_price - q.bid_price;
            q.spread_bps  = (q.mid_price > 0)
                          ? (q.spread / q.mid_price) * 10000.0 : 0.0;
            q.bid_size    = item.value("bid_size", 0);
            q.ask_size    = item.value("ask_size", 0);
            q.ts_event    = item.value("ts_ms",    int64_t{0});
            q.ts_recv     = q.ts_event;

            quotes.push_back(std::move(q));
        }
        return quotes;
    }

    UnderlyingTrade underlying_trade(const std::string& symbol) override {
        const json resp = sock_.rpc({
            { "method", "underlying_trade" },
            { "symbol", symbol             },
        });

        check_ok(resp);
        const auto& d = resp.at("data");

        UnderlyingTrade t;
        t.symbol   = d.at("symbol").get<std::string>();
        t.price    = d.at("price").get<double>();
        t.size     = d.value("size",   0);
        t.ts_event = d.value("ts_ms",  int64_t{0});
        return t;
    }

    [[nodiscard]] bool is_paper() const noexcept { return cfg_.paper; }

private:
    Config      cfg_;
    BridgeSocket sock_;

    static void check_ok(const json& resp) {
        if (!resp.value("ok", false)) {
            throw std::runtime_error(
                "IBKRProvider bridge error: " +
                resp.value("error", "(no message)")
            );
        }
    }

    static OptionRight parse_right(const std::string& s) noexcept {
        if (s == "C" || s == "c") return OptionRight::Call;
        if (s == "P" || s == "p") return OptionRight::Put;
        return OptionRight::Unknown;
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// Factory helpers  (same signatures as market_data.hpp)
// ─────────────────────────────────────────────────────────────────────────────

inline std::unique_ptr<IMarketData> make_ibkr_provider(
    bool               paper     = true,
    const std::string& cache_dir = "data/cache",
    int                ttl_hours = 1
) {
    IBKRProvider::Config cfg;
    cfg.paper = paper;
    auto inner = std::make_unique<IBKRProvider>(std::move(cfg));
    return std::make_unique<CachedProvider>(std::move(inner), cache_dir, ttl_hours);
}

inline std::unique_ptr<IMarketData> make_provider(
    const std::string& cache_dir = "data/cache",
    int                ttl_hours = 24
) {
    const char* mode = std::getenv("VOL_PROVIDER");
    if (mode) {
        const std::string m(mode);
        if (m == "ibkr_paper") return make_ibkr_provider(true,  cache_dir, 1);
        if (m == "ibkr_live")  return make_ibkr_provider(false, cache_dir, 0);
    }
    return make_synthetic_provider({}, cache_dir, ttl_hours);
}

} // namespace volarb::data
